#include <string.h>
#include <gio/gio.h>
#include <systemd/sd-daemon.h>

static gboolean opt_session = FALSE;
static int opt_inc_timeout_ms = 100;

typedef struct {
  GMainContext *mainctx;
  gboolean running;

  guint expected_counter;

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
  if (app->async_error)
    app.running = FALSE;
}

static gboolean
idle_do_inc (gpointer user_data)
{
  App *self = user_data;

  return TRUE;
}

static void
on_get_return (GDBusProxy          *counter,
	       GAsyncResult        *result,
	       App                 *self)
{
  GVariant *resultv =
    g_dbus_proxy_call_finish (counter, result, &self.async_error);

  if (!resultv)
    goto out;

  g_assert (g_variant_is_of_type (resultv, "(u)"));

  g_variant_get (resultv, "(u)", &app->expected_counter);

  if (!app->idle_inc_id)
    {
      app->idle_inc_id = g_timeout_add (opt_inc_timeout_ms, (GSourceFunc)idle_do_inc, app);
      (void) idle_do_inc (app);
    }

 out:
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

  g_clear_object (&app->counter);
  app->counter = g_dbus_proxy_new_sync (connection,
					G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
					NULL,                      /* GDBusInterfaceInfo */
					name_owner, /* name */
					"/org/verbum/counter", /* object path */
					"org.verbum.Counter",
					NULL, /* GCancellable */
					&app.async_error);
  if (!app->counter)
    goto out;

  g_dbus_proxy_call (app->counter, "Get", NULL, 0, -1, NULL,
		     (GAsyncReadyCallback)on_get_return, app);

 out:
  handle_async_error (self);
}

static void
on_name_vanished (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_printerr ("name now owned by: <none>\n");
  if (app->idle_inc_id)
    {
      g_source_remove (app->idle_inc_id);
      app->idle_inc_id = 0;
    }
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
                                 NULL,
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
