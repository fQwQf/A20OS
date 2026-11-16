/* Production callbacks on a real private GIO bus. No GTK/display is needed. */
#include <gio/gio.h>
#include "gtk-settings-exported.h"
typedef GObject XfconfChannel;
typedef struct _XfceGtkSettingsHelper XfceGtkSettingsHelper;
typedef struct {GObjectClass parent;} XfceGtkSettingsHelperClass;
/* PRODUCTION_STRUCT */
#define XFCE_GTK_SETTINGS_HELPER(object) ((XfceGtkSettingsHelper *)(object))
#define WINDOWING_IS_WAYLAND() TRUE
static void xfce_gtk_settings_helper_init(XfceGtkSettingsHelper *helper);
static void xfce_gtk_settings_helper_class_init(XfceGtkSettingsHelperClass *klass);
G_DEFINE_TYPE(XfceGtkSettingsHelper, xfce_gtk_settings_helper, G_TYPE_OBJECT)
static GDBusConnection *connection;
static GBusAcquiredCallback acquired_callback;
static GBusNameLostCallback lost_callback;
static guint acquired_count,lost_count;
static guint test_own(GBusType,const gchar *,GBusNameOwnerFlags,
    GBusAcquiredCallback,GBusNameAcquiredCallback,GBusNameLostCallback,gpointer,GDestroyNotify);
#define g_bus_own_name test_own
/* PRODUCTION_CALLBACKS */
static void xfce_gtk_settings_helper_init(XfceGtkSettingsHelper *helper) {
/* PRODUCTION_REGISTRATION */
}
#undef g_bus_own_name
static void xfce_gtk_settings_helper_class_init(XfceGtkSettingsHelperClass *klass) {
    G_OBJECT_CLASS(klass)->finalize=xfce_gtk_settings_helper_finalize;
}
static void observed_acquired(GDBusConnection *bus,const gchar *name,gpointer data) {
    /* Permit testing the callback lifecycle after a disconnect rather than
       terminating the fixture through the application's exit-on-close policy. */
    g_dbus_connection_set_exit_on_close(bus,FALSE);
    g_set_object(&connection,bus);
    acquired_callback(bus,name,data);
    acquired_count++;
}
static void observed_lost(GDBusConnection *bus,const gchar *name,gpointer data) {
    lost_callback(bus,name,data);
    if(bus==NULL)lost_count++;
}
static guint test_own(GBusType type,const gchar *name,GBusNameOwnerFlags flags,
    GBusAcquiredCallback acquired,GBusNameAcquiredCallback named,
    GBusNameLostCallback lost,gpointer data,GDestroyNotify destroy) {
    acquired_callback=acquired;lost_callback=lost;
    return g_bus_own_name(type,name,flags,observed_acquired,named,observed_lost,data,destroy);
}
static gboolean wait_for(guint *count) {
    gint64 end=g_get_monotonic_time()+2*G_TIME_SPAN_SECOND;
    while(!*count && g_get_monotonic_time()<end) {
        while(g_main_context_iteration(NULL,FALSE));
        g_usleep(1000);
    }
    return *count!=0;
}
static void destroyed(gpointer data,GObject *object) {(void)object;*(gboolean *)data=TRUE;}
int main(int argc,char **argv) {
    g_test_init(&argc,&argv,NULL);
    g_log_set_always_fatal(G_LOG_FATAL_MASK);
    GTestDBus *bus=g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    XfceGtkSettingsHelper *helper=g_object_new(xfce_gtk_settings_helper_get_type(),NULL);
    gboolean dead=FALSE;g_object_weak_ref(G_OBJECT(helper),destroyed,&dead);
    g_assert_true(wait_for(&acquired_count));
    g_assert_false(dead);
    g_assert_nonnull(helper->skeleton);
    g_assert_true(g_dbus_connection_close_sync(connection,NULL,NULL));
    g_assert_true(wait_for(&lost_count));
    if(dead) {
        g_printerr("LIFETIME_BAD: disconnect consumed main's helper reference\n");
        g_test_dbus_stop(bus);
        return 2;
    }
    g_object_unref(helper);
    g_assert_true(dead);
    while(g_main_context_iteration(NULL,FALSE));
    g_clear_object(&connection);
    /* Finalization before a queued bus-acquired callback must also be safe. */
    helper=g_object_new(xfce_gtk_settings_helper_get_type(),NULL);
    dead=FALSE;g_object_weak_ref(G_OBJECT(helper),destroyed,&dead);
    g_object_unref(helper);g_assert_true(dead);
    for(unsigned n=0;n<20;n++) {
        while(g_main_context_iteration(NULL,FALSE));
        g_usleep(1000);
    }
    g_clear_object(&connection);
    g_test_dbus_down(bus);g_object_unref(bus);
    g_print("LIFETIME_OK: bus loss preserves main reference and unown tolerates queued callbacks\n");
    return 0;
}
