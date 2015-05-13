#include <string.h>
#include <gio/gio.h>
#include <systemd/sd-daemon.h>

static gboolean opt_session = FALSE;
static int opt_inc_timeout_min_ms = 100;
static int opt_inc_timeout_max_ms = 2000;

typedef struct {
  GMainContext *mainctx;
  gboolean running;

  guint expected_counter;
  guint idle_inc_id;
  guint idle_status_id;
  guint n_increments;

  GDBusProxy *counter;
  GError *async_error;
} App;

static void
consume_error (GError *error)
{
  g_printerr ("%s\n", error->message);
  g_error_free (error);
}

static void
handle_async_error (App *self)
{
  if (self->async_error)
    self->running = FALSE;
}

static GError **
get_async_error (App *self)
{
  if (self->async_error)
    return NULL;
  return &self->async_error;
}

static gboolean
idle_do_inc (gpointer user_data);

static void
on_inc_return (GDBusProxy          *counter,
	       GAsyncResult        *result,
	       App                 *self)
{
  GVariant *resultv =
    g_dbus_proxy_call_finish (counter, result, get_async_error (self));

  if (!resultv)
    goto out;

  g_assert (g_variant_is_of_type (resultv, G_VARIANT_TYPE ("()")));

 out:
  g_clear_pointer (&resultv, g_variant_unref);
  handle_async_error (self);
}

static void
on_get_return (GDBusProxy          *counter,
	       GAsyncResult        *result,
	       App                 *self)
{
  GVariant *resultv =
    g_dbus_proxy_call_finish (counter, result, get_async_error (self));
  guint counterv;

  if (!resultv)
    goto out;

  g_assert (g_variant_is_of_type (resultv, G_VARIANT_TYPE ("(u)")));

  g_variant_get (resultv, "(u)", &counterv);

  g_assert_cmpuint (counterv, ==, self->expected_counter);

  {
    guint msec = g_random_int_range (opt_inc_timeout_min_ms, opt_inc_timeout_max_ms);
    self->idle_inc_id = g_timeout_add (msec, (GSourceFunc)idle_do_inc, self);
  }

 out:
  g_clear_pointer (&resultv, g_variant_unref);
  handle_async_error (self);
}

static gboolean
idle_do_inc (gpointer user_data)
{
  App *self = user_data;
  guint msec = g_random_int_range (opt_inc_timeout_min_ms, opt_inc_timeout_max_ms);

  g_dbus_proxy_call (self->counter, "Inc", NULL, 0, -1, NULL,
		     (GAsyncReadyCallback)on_inc_return, self);

  g_dbus_proxy_call (self->counter, "Get", NULL, 0, -1, NULL,
		     (GAsyncReadyCallback)on_get_return, self);

  self->expected_counter++;
  self->n_increments++;

  return FALSE;
}

static gboolean
idle_print_status (App *self)
{
  g_printerr ("counter: %u; increments: %u\n",
	      self->expected_counter,
	      self->n_increments);
  
  return TRUE;
}

static void
on_initial_get_return (GDBusProxy          *counter,
		       GAsyncResult        *result,
		       App                 *self)
{
  GVariant *resultv =
    g_dbus_proxy_call_finish (counter, result, get_async_error (self));

  if (!resultv)
    goto out;

  g_assert (g_variant_is_of_type (resultv, G_VARIANT_TYPE ("(u)")));

  g_variant_get (resultv, "(u)", &self->expected_counter);
  g_printerr ("Initial counter: %u\n", self->expected_counter);

  if (!self->idle_inc_id)
    self->idle_inc_id = g_idle_add ((GSourceFunc)idle_do_inc, self);

  if (!self->idle_status_id)
    self->idle_status_id = g_timeout_add_seconds (3, (GSourceFunc)idle_print_status, self);

 out:
  g_clear_pointer (&resultv, g_variant_unref);
  handle_async_error (self);
}

static void
on_name_appeared (GDBusConnection *connection,
                  const gchar     *name,
                  const gchar     *name_owner,
                  gpointer         user_data)
{
  App *self = user_data;

  g_printerr ("name now owned by: %s\n", name_owner);

  if (!self->counter)
    {
      /* NOTE: We always make a proxy for the well-known name, to avoid race conditions
       * when the service is auto-restarting.
       */
      g_printerr ("Creating initial proxy\n");
      self->counter = g_dbus_proxy_new_sync (connection,
					     G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
					     NULL,                      /* GDBusInterfaceInfo */
					     name,
					     "/org/verbum/counter", /* object path */
					     "org.verbum.Counter",
					     NULL, /* GCancellable */
					     get_async_error (self));
      if (!self->counter)
	goto out;

      g_dbus_proxy_call (self->counter, "Get", NULL, 0, -1, NULL,
			 (GAsyncReadyCallback)on_initial_get_return, self);
    }

 out:
  handle_async_error (self);
}

static void
on_name_vanished (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  App *self = user_data;

  g_printerr ("name now owned by: <none>\n");
}

int
main (int argc, char **argv)
{
  App app = { 0, };
  App *self = &app;
  GError *local_error = NULL;
  guint watcher_id;
  GOptionContext *option_context;
  GOptionEntry option_entries[] =
    {
      { "session", 'y', 0, G_OPTION_ARG_NONE, &opt_session, "Use the session bus, not system", NULL },
      { "min-freq", 0, 0, G_OPTION_ARG_INT, &opt_inc_timeout_min_ms, "Minimum timeout", "MSEC" },
      { "max-freq", 0, 0, G_OPTION_ARG_INT, &opt_inc_timeout_max_ms, "Max timeout", "MSEC" },
      { NULL}
    };

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  option_context = g_option_context_new ("GDBus exit on idle test");
  g_option_context_add_main_entries (option_context, option_entries, NULL);

  if (!g_option_context_parse (option_context, &argc, &argv, &local_error))
    goto out;

  watcher_id = g_bus_watch_name (opt_session ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM,
                                 "org.verbum.TestExitOnIdle",
                                 G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                 on_name_appeared,
                                 on_name_vanished,
                                 self,
                                 NULL);

  app.running = TRUE;

  while (app.running)
    g_main_context_iteration (app.mainctx, TRUE);

  if (app.async_error)
    g_propagate_error (&local_error, app.async_error);

 out:
  if (local_error)
    {
      consume_error (local_error);
      return 1;
    }
  return 0;
}
