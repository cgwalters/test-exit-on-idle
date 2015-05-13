#include <string.h>
#include <gio/gio.h>
#include <systemd/sd-daemon.h>

static void
consume_error (GError *error)
{
  g_printerr ("%s\n", error->message);
  g_error_free (error);
}

static int opt_idle_timeout_ms = 2000;
static int opt_exit_sleep_ms = 3000;
static gboolean opt_session = FALSE;
static gboolean opt_racy_exit = FALSE;
static int opt_save_timeout_ms = 1000;

typedef enum {
  STATE_RUNNING,
  STATE_FLUSHING, /* fwoosh */
  STATE_EXITING
} AppState;

typedef struct {
  GMainContext *mainctx;
  AppState state;

  GFile *counterf;

  guint counter;
  GSource *idle_save_source;
  GSource *idle_exit_source;
} App;

static GDBusNodeInfo *introspection_data = NULL;

/* Introspection data for the service we are exporting */
static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.verbum.Counter'>"
  "    <method name='Inc'>"
  "    </method>"
  "    <method name='Get'>"
  "      <arg direction='out' type='u'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static gboolean
idle_flush_and_exit (App *self)
{
  g_printerr ("Exiting on idle\n");
  g_assert_cmpint (self->state, ==, STATE_RUNNING);
  self->state = STATE_FLUSHING;
  g_main_context_wakeup (self->mainctx);
}

static void
bump_idle_timer (App *self)
{
  if (self->idle_exit_source)
    g_source_destroy (self->idle_exit_source);

  if (self->state == STATE_RUNNING)
    {
      guint ms = g_random_int_range (0, opt_idle_timeout_ms);
      g_printerr ("Reset idle timer (%u ms)\n", ms);
      self->idle_exit_source = g_timeout_source_new (ms);
      g_source_set_callback (self->idle_exit_source, (GSourceFunc)idle_flush_and_exit, self, NULL);
      g_source_attach (self->idle_exit_source, self->mainctx);
    }
}

static gboolean
idle_save (App *self)
{
  GError *local_error = NULL;
  char *counter_str = NULL;

  self->idle_save_source = NULL;

  counter_str = g_strdup_printf ("%u\n", self->counter);

  g_printerr ("Performing idle content save...");
  if (!g_file_replace_contents (self->counterf, counter_str, strlen (counter_str),
				NULL, FALSE, 0,
				NULL, NULL, &local_error))
    goto out;
  g_printerr ("Done\n");

 out:
  if (local_error)
    consume_error (local_error);

  g_free (counter_str);

  return FALSE;
}

static void
handle_method_call (GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  App *self = user_data;

  if (g_strcmp0 (method_name, "Get") == 0)
    {
      g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", self->counter));
    }
  else if (g_strcmp0 (method_name, "Inc") == 0)
    {
      self->counter++;
      g_printerr ("counter=%u\n", self->counter);
      if (self->idle_save_source == NULL)
	{
	  guint ms = g_random_int_range (0, opt_save_timeout_ms);
	  self->idle_save_source = g_timeout_source_new (ms);
	  g_source_set_callback (self->idle_save_source, (GSourceFunc)idle_save, self, NULL);
	  g_source_attach (self->idle_save_source, self->mainctx);
	}
      bump_idle_timer (self);
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
}

/* for now */
static const GDBusInterfaceVTable counter_interface_vtable =
{
  handle_method_call,
  NULL,
  NULL 
};

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  guint id = g_dbus_connection_register_object (connection,
						"/org/verbum/counter",
						introspection_data->interfaces[0],
						&counter_interface_vtable,
						user_data,
						NULL,
						NULL);
  g_assert_cmpuint (id, >, 0);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_printerr ("Bus name acquired\n");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
}

static void
on_bus_name_released (GDBusConnection     *connection,
		      GAsyncResult        *result,
		      App                 *self)
{
  g_assert_cmpint (self->state, ==, STATE_FLUSHING);
  self->state = STATE_EXITING;
  g_main_context_wakeup (self->mainctx);
}

static gboolean
on_sigterm (gpointer user_data)
{
  App *self = user_data;
  g_printerr ("(SIGTERM)\n");
  if (self->state == STATE_RUNNING)
    self->state = STATE_FLUSHING;
  return FALSE;
}

int
main (int argc, char **argv)
{
  App app = { 0, };
  App *self = &app;
  GError *local_error = NULL;
  guint owner_id;
  GOptionContext *option_context;
  GDBusConnection *bus;
  static const char busname[] = "org.verbum.TestExitOnIdle";
  GOptionEntry option_entries[] =
    {
      { "idle-timeout", 'i', 0, G_OPTION_ARG_INT, &opt_idle_timeout_ms, "Idle timeout in milliseconds", "MSEC" },
      { "save-timeout", 's', 0, G_OPTION_ARG_INT, &opt_save_timeout_ms, "Save timeout in milliseconds", "MSEC" },
      { "session", 'y', 0, G_OPTION_ARG_NONE, &opt_session, "Use the session bus, not system", NULL },
      { "racy-exit", 0, 0, G_OPTION_ARG_NONE, &opt_racy_exit, "Avoid using sd_notify when stopping", NULL },
      { NULL}
    };

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
  g_assert (introspection_data != NULL);

  option_context = g_option_context_new ("GDBus exit on idle");
  g_option_context_add_main_entries (option_context, option_entries, NULL);

  if (!g_option_context_parse (option_context, &argc, &argv, &local_error))
    goto out;

  self->counterf = g_file_new_for_path (opt_session ? "counter" : "/var/lib/org.verbum.TestExitOnIdle/counter");

  bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &local_error);
  if (!bus)
    goto out;
  owner_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                             busname,
                             0,
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             self,
                             NULL);
  g_assert_cmpuint (owner_id, >, 0);

  { char *contents = NULL;
    gsize len;

    g_printerr ("loading contents\n");
    if (!g_file_load_contents (self->counterf, NULL, &contents, &len,
			       NULL, &local_error))
      {
	if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
	  {
	    g_clear_error (&local_error);
	  }
	else
	  goto out;
      }

    if (contents)
      self->counter = g_ascii_strtoull (contents, NULL, 10);
  }

  g_printerr ("counter=%u\n", self->counter);

  self->state = STATE_RUNNING;
  bump_idle_timer (self);

  g_unix_signal_add (SIGTERM, on_sigterm, self);

  g_printerr ("=> STATE_RUNNING\n");

  while (self->state == STATE_RUNNING)
    g_main_context_iteration (self->mainctx, TRUE);

  g_printerr ("=> STATE_FLUSHING\n");

  /* Widen the race condition */
  g_usleep (g_random_int_range (0, opt_exit_sleep_ms) * 1000);

  /* Taken from systemd/src/libsystemd/sd-bus/bus-util.c */
  /* Inform the service manager that we are going down, so that it
   * will queue all further start requests, instead of assuming we are
   * already running. */
  if (!opt_racy_exit)
    sd_notify (FALSE, "STOPPING=1");

  g_dbus_connection_call (bus,
			  "org.freedesktop.DBus",
			  "/org/freedesktop/DBus",
			  "org.freedesktop.DBus",
			  "ReleaseName",
			  g_variant_new ("(s)", busname),
			  G_VARIANT_TYPE ("(u)"),
			  G_DBUS_CALL_FLAGS_NONE,
			  -1,
			  NULL,
			  (GAsyncReadyCallback)on_bus_name_released,
			  self);

  g_assert_cmpint (self->state, ==, STATE_FLUSHING);

  while (self->state == STATE_FLUSHING)
    g_main_context_iteration (self->mainctx, TRUE);

  g_printerr ("=> STATE_EXITING\n");
  g_assert_cmpint (self->state, ==, STATE_EXITING);

  /* Widen the race condition */
  g_usleep (g_random_int_range (0, opt_exit_sleep_ms) * 1000);

  if (self->idle_save_source)
    (void) idle_save (self);

 out:
  if (local_error)
    {
      consume_error (local_error);
      return 1;
    }
  g_printerr ("exit(0)\n");
  return 0;
}
