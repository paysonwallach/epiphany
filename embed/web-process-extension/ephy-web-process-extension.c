/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2014 Igalia S.L.
 *
 *  This file is part of Epiphany.
 *
 *  Epiphany is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Epiphany is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Epiphany.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"
#include "ephy-web-process-extension.h"

#include "ephy-dbus-names.h"
#include "ephy-dbus-util.h"
#include "ephy-debug.h"
#include "ephy-file-helpers.h"
#include "ephy-permissions-manager.h"
#include "ephy-prefs.h"
#include "ephy-settings.h"
#include "ephy-uri-helpers.h"
#include "ephy-web-overview-model.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <gtk/gtk.h>
#include <jsc/jsc.h>
#include <libsoup/soup.h>
#include <string.h>
#include <webkit2/webkit-web-extension.h>
#include <JavaScriptCore/JavaScript.h>

struct _EphyWebProcessExtension {
  GObject parent_instance;

  WebKitWebExtension *extension;
  gboolean initialized;

  GDBusConnection *dbus_connection;
  GArray *page_created_signals_pending;

  EphyWebOverviewModel *overview_model;
  EphyPermissionsManager *permissions_manager;

  WebKitScriptWorld *script_world;

  gboolean is_private_profile;

  GHashTable *frames_map;
};

static const char introspection_xml[] =
  "<node>"
  " <interface name='org.gnome.Epiphany.WebProcessExtension'>"
  "  <signal name='PageCreated'>"
  "   <arg type='t' name='page_id' direction='out'/>"
  "  </signal>"
  "  <method name='HistorySetURLs'>"
  "   <arg type='a(ss)' name='urls' direction='in'/>"
  "  </method>"
  "  <method name='HistorySetURLThumbnail'>"
  "   <arg type='s' name='url' direction='in'/>"
  "   <arg type='s' name='path' direction='in'/>"
  "  </method>"
  "  <method name='HistorySetURLTitle'>"
  "   <arg type='s' name='url' direction='in'/>"
  "   <arg type='s' name='title' direction='in'/>"
  "  </method>"
  "  <method name='HistoryDeleteURL'>"
  "   <arg type='s' name='url' direction='in'/>"
  "  </method>"
  "  <method name='HistoryDeleteHost'>"
  "   <arg type='s' name='host' direction='in'/>"
  "  </method>"
  "  <method name='HistoryClear'/>"
  "  <method name='PasswordQueryResponse'>"
  "    <arg type='s' name='username' direction='in'/>"
  "    <arg type='s' name='password' direction='in'/>"
  "    <arg type='i' name='promise_id' direction='in'/>"
  "    <arg type='t' name='frame_id' direction='in'/>"
  "  </method>"
  "  <method name='PasswordQueryUsernamesResponse'>"
  "    <arg type='as' name='users' direction='in'/>"
  "    <arg type='i' name='promise_id' direction='in'/>"
  "    <arg type='t' name='frame_id' direction='in'/>"
  "  </method>"
  " </interface>"
  "</node>";

G_DEFINE_TYPE (EphyWebProcessExtension, ephy_web_process_extension, G_TYPE_OBJECT)

static gboolean
web_page_send_request (WebKitWebPage           *web_page,
                       WebKitURIRequest        *request,
                       WebKitURIResponse       *redirected_response,
                       EphyWebProcessExtension *extension)
{
  if (g_settings_get_boolean (EPHY_SETTINGS_WEB_PROCESS_EXTENSION_WEB, EPHY_PREFS_WEB_DO_NOT_TRACK)) {
    const char *request_uri = webkit_uri_request_get_uri (request);
    g_autofree char *modified_uri = ephy_remove_tracking_from_uri (request_uri);
    if (modified_uri && g_strcmp0 (request_uri, modified_uri) != 0) {
      LOG ("Rewrote %s to %s", request_uri, modified_uri);
      webkit_uri_request_set_uri (request, modified_uri);
    }
  }
  return FALSE;
}

static void
web_page_will_submit_form (WebKitWebPage            *web_page,
                           WebKitDOMHTMLFormElement *dom_form,
                           WebKitFormSubmissionStep  step,
                           WebKitFrame              *source_frame,
                           WebKitFrame              *target_frame,
                           GPtrArray                *text_field_names,
                           GPtrArray                *text_field_values)
{
  EphyWebProcessExtension *extension;
  gboolean form_submit_handled;
  g_autoptr (JSCContext) js_context = NULL;
  g_autoptr (JSCValue) js_ephy = NULL;
  g_autoptr (JSCValue) js_form = NULL;
  g_autoptr (JSCValue) js_result = NULL;

  form_submit_handled =
    GPOINTER_TO_INT (g_object_get_data (G_OBJECT (dom_form),
                                        "ephy-form-submit-handled"));
  if (form_submit_handled)
    return;

  g_object_set_data (G_OBJECT (dom_form),
                     "ephy-form-submit-handled",
                     GINT_TO_POINTER (TRUE));

  extension = ephy_web_process_extension_get ();
  js_context = webkit_frame_get_js_context_for_script_world (source_frame, extension->script_world);
  js_ephy = jsc_context_get_value (js_context, "Ephy");
  js_form = webkit_frame_get_js_value_for_dom_object_in_script_world (source_frame, WEBKIT_DOM_OBJECT (dom_form), extension->script_world);
  js_result = jsc_value_object_invoke_method (js_ephy,
                                              "handleFormSubmission",
                                              G_TYPE_UINT64, webkit_web_page_get_id (web_page),
                                              G_TYPE_UINT64, webkit_frame_get_id (source_frame),
                                              JSC_TYPE_VALUE, js_form,
                                              G_TYPE_NONE);
}

static char *
password_form_message_serializer (guint64  frame_id,
                                  gboolean is_insecure_action)
{
  GVariant *variant;
  char *message;

  variant = g_variant_new ("(tb)", frame_id, is_insecure_action);
  message = g_variant_print (variant, FALSE);
  g_variant_unref (variant);

  return message;
}

static gboolean
remove_if_value_matches_user_data (gpointer key,
                                   gpointer value,
                                   gpointer user_data)
{
  return value == user_data;
}

static void
frame_destroyed_notify (EphyWebProcessExtension *extension,
                        GObject                 *where_the_object_was)
{
  /* This is awkward. We can't just remove the frame from the table
   * directly since we don't have any way to get its ID except by
   * checking every entry in the map....
   */
  g_hash_table_foreach_remove (extension->frames_map,
                               remove_if_value_matches_user_data,
                               where_the_object_was);
}

static void
web_page_form_controls_associated (WebKitWebPage           *web_page,
                                   GPtrArray               *elements,
                                   WebKitFrame             *frame,
                                   EphyWebProcessExtension *extension)
{
  g_autoptr (GPtrArray) form_controls = NULL;
  g_autoptr (JSCContext) js_context = NULL;
  g_autoptr (JSCValue) js_ephy = NULL;
  g_autoptr (JSCValue) js_serializer = NULL;
  g_autoptr (JSCValue) js_result = NULL;
  guint i;

  js_context = webkit_frame_get_js_context_for_script_world (frame, extension->script_world);

  form_controls = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < elements->len; ++i) {
    WebKitDOMObject *element = WEBKIT_DOM_OBJECT (g_ptr_array_index (elements, i));

    g_ptr_array_add (form_controls, webkit_frame_get_js_value_for_dom_object_in_script_world (frame, element, extension->script_world));
  }

  js_ephy = jsc_context_get_value (js_context, "Ephy");
  js_serializer = jsc_value_new_function (js_context,
                                          "passwordFormMessageSerializer",
                                          G_CALLBACK (password_form_message_serializer), NULL, NULL,
                                          G_TYPE_STRING, 2,
                                          G_TYPE_UINT64, G_TYPE_BOOLEAN);
  js_result = jsc_value_object_invoke_method (js_ephy,
                                              "formControlsAssociated",
                                              G_TYPE_UINT64, webkit_web_page_get_id (web_page),
                                              G_TYPE_UINT64, webkit_frame_get_id (frame),
                                              G_TYPE_PTR_ARRAY, form_controls,
                                              JSC_TYPE_VALUE, js_serializer,
                                              G_TYPE_NONE);

  if (!g_hash_table_contains (extension->frames_map, GINT_TO_POINTER (webkit_frame_get_id (frame)))) {
    g_hash_table_insert (extension->frames_map, GINT_TO_POINTER (webkit_frame_get_id (frame)), frame);
    g_object_weak_ref (G_OBJECT (frame), (GWeakNotify)frame_destroyed_notify, extension);
  }
}

static gboolean
web_page_context_menu (WebKitWebPage          *web_page,
                       WebKitContextMenu      *context_menu,
                       WebKitWebHitTestResult *hit_test_result,
                       gpointer                user_data)
{
  EphyWebProcessExtension *extension;
  g_autofree char *string = NULL;
  GVariantBuilder builder;
  WebKitFrame *frame;
  g_autoptr (JSCContext) js_context = NULL;
  g_autoptr (JSCValue) js_value = NULL;

  extension = ephy_web_process_extension_get ();
  /* FIXME: this is wrong, see https://gitlab.gnome.org/GNOME/epiphany/issues/442
   * We need a way to get the right frame to use here.
   */
  frame = webkit_web_page_get_main_frame (web_page);
  js_context = webkit_frame_get_js_context_for_script_world (frame, extension->script_world);

  js_value = jsc_context_evaluate (js_context, "window.getSelection().toString();", -1);
  if (!jsc_value_is_null (js_value) && !jsc_value_is_undefined (js_value))
    string = jsc_value_to_string (js_value);

  if (!string || *string == '\0')
    return FALSE;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "SelectedText", g_variant_new_string (g_strstrip (string)));
  webkit_context_menu_set_user_data (context_menu,
                                     g_variant_builder_end (&builder));

  return TRUE;
}

static void
ephy_web_process_extension_emit_page_created (EphyWebProcessExtension *extension,
                                              guint64                  page_id)
{
  g_autoptr (GError) error = NULL;

  g_dbus_connection_emit_signal (extension->dbus_connection,
                                 NULL,
                                 EPHY_WEB_PROCESS_EXTENSION_OBJECT_PATH,
                                 EPHY_WEB_PROCESS_EXTENSION_INTERFACE,
                                 "PageCreated",
                                 g_variant_new ("(t)", page_id),
                                 &error);
  if (error)
    g_warning ("Error emitting signal PageCreated: %s\n", error->message);
}

static void
ephy_web_process_extension_emit_page_created_signals_pending (EphyWebProcessExtension *extension)
{
  guint i;

  if (!extension->page_created_signals_pending)
    return;

  for (i = 0; i < extension->page_created_signals_pending->len; i++) {
    guint64 page_id;

    page_id = g_array_index (extension->page_created_signals_pending, guint64, i);
    ephy_web_process_extension_emit_page_created (extension, page_id);
  }

  g_array_free (extension->page_created_signals_pending, TRUE);
  extension->page_created_signals_pending = NULL;
}

static void
ephy_web_process_extension_queue_page_created_signal_emission (EphyWebProcessExtension *extension,
                                                               guint64                  page_id)
{
  if (!extension->page_created_signals_pending)
    extension->page_created_signals_pending = g_array_new (FALSE, FALSE, sizeof (guint64));
  extension->page_created_signals_pending = g_array_append_val (extension->page_created_signals_pending, page_id);
}

static void
ephy_web_process_extension_page_created_cb (EphyWebProcessExtension *extension,
                                            WebKitWebPage           *web_page)
{
  guint64 page_id;
  g_autoptr (JSCContext) js_context = NULL;

  /* Enforce the creation of the script world global context in the main frame */
  js_context = webkit_frame_get_js_context_for_script_world (webkit_web_page_get_main_frame (web_page), extension->script_world);

  page_id = webkit_web_page_get_id (web_page);
  if (extension->dbus_connection)
    ephy_web_process_extension_emit_page_created (extension, page_id);
  else
    ephy_web_process_extension_queue_page_created_signal_emission (extension, page_id);

  g_signal_connect (web_page, "send-request",
                    G_CALLBACK (web_page_send_request),
                    extension);
  g_signal_connect (web_page, "context-menu",
                    G_CALLBACK (web_page_context_menu),
                    extension);
  g_signal_connect (web_page, "will-submit-form",
                    G_CALLBACK (web_page_will_submit_form),
                    extension);
  g_signal_connect (web_page, "form-controls-associated-for-frame",
                    G_CALLBACK (web_page_form_controls_associated),
                    extension);
}

static JSCValue *
get_password_manager (EphyWebProcessExtension *self,
                      guint64                  frame_id)
{
  WebKitFrame *frame;
  g_autoptr (JSCContext) js_context = NULL;
  g_autoptr (JSCValue) js_ephy = NULL;

  frame = g_hash_table_lookup (self->frames_map, GINT_TO_POINTER (frame_id));
  if (!frame)
    return NULL;

  js_context = webkit_frame_get_js_context_for_script_world (frame, self->script_world);
  js_ephy = jsc_context_get_value (js_context, "Ephy");

  return jsc_value_object_get_property (js_ephy, "passwordManager");
}

static void
handle_method_call (GDBusConnection       *connection,
                    const char            *sender,
                    const char            *object_path,
                    const char            *interface_name,
                    const char            *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
{
  EphyWebProcessExtension *extension = EPHY_WEB_PROCESS_EXTENSION (user_data);

  if (g_strcmp0 (interface_name, EPHY_WEB_PROCESS_EXTENSION_INTERFACE) != 0)
    return;

  if (g_strcmp0 (method_name, "HistorySetURLs") == 0) {
    if (extension->overview_model) {
      GVariantIter iter;
      g_autoptr (GVariant) array = NULL;
      const char *url;
      const char *title;
      GList *items = NULL;

      g_variant_get (parameters, "(@a(ss))", &array);
      g_variant_iter_init (&iter, array);

      while (g_variant_iter_loop (&iter, "(&s&s)", &url, &title))
        items = g_list_prepend (items, ephy_web_overview_model_item_new (url, title));

      ephy_web_overview_model_set_urls (extension->overview_model, g_list_reverse (items));
    }
    g_dbus_method_invocation_return_value (invocation, NULL);
  } else if (g_strcmp0 (method_name, "HistorySetURLThumbnail") == 0) {
    if (extension->overview_model) {
      const char *url;
      const char *path;

      g_variant_get (parameters, "(&s&s)", &url, &path);
      ephy_web_overview_model_set_url_thumbnail (extension->overview_model, url, path, TRUE);
    }
    g_dbus_method_invocation_return_value (invocation, NULL);
  } else if (g_strcmp0 (method_name, "HistorySetURLTitle") == 0) {
    if (extension->overview_model) {
      const char *url;
      const char *title;

      g_variant_get (parameters, "(&s&s)", &url, &title);
      ephy_web_overview_model_set_url_title (extension->overview_model, url, title);
    }
    g_dbus_method_invocation_return_value (invocation, NULL);
  } else if (g_strcmp0 (method_name, "HistoryDeleteURL") == 0) {
    if (extension->overview_model) {
      const char *url;

      g_variant_get (parameters, "(&s)", &url);
      ephy_web_overview_model_delete_url (extension->overview_model, url);
    }
    g_dbus_method_invocation_return_value (invocation, NULL);
  } else if (g_strcmp0 (method_name, "HistoryDeleteHost") == 0) {
    if (extension->overview_model) {
      const char *host;

      g_variant_get (parameters, "(&s)", &host);
      ephy_web_overview_model_delete_host (extension->overview_model, host);
    }
    g_dbus_method_invocation_return_value (invocation, NULL);
  } else if (g_strcmp0 (method_name, "HistoryClear") == 0) {
    if (extension->overview_model)
      ephy_web_overview_model_clear (extension->overview_model);
    g_dbus_method_invocation_return_value (invocation, NULL);
  } else if (g_strcmp0 (method_name, "PasswordQueryUsernamesResponse") == 0) {
    g_autofree const char **users;
    g_autoptr (JSCValue) ret = NULL;
    g_autoptr (JSCValue) password_manager = NULL;
    gint32 promise_id;
    guint64 frame_id;

    users = g_variant_get_strv (g_variant_get_child_value (parameters, 0), NULL);
    g_variant_get_child (parameters, 1, "i", &promise_id);
    g_variant_get_child (parameters, 2, "t", &frame_id);

    password_manager = get_password_manager (extension, frame_id);
    if (password_manager != NULL)
      ret = jsc_value_object_invoke_method (password_manager, "_onQueryUsernamesResponse",
                                            G_TYPE_STRV, users, G_TYPE_INT, promise_id, G_TYPE_NONE);
  } else if (g_strcmp0 (method_name, "PasswordQueryResponse") == 0) {
    const char *username;
    const char *password;
    gint32 promise_id;
    guint64 frame_id;
    g_autoptr (JSCValue) ret = NULL;
    g_autoptr (JSCValue) password_manager = NULL;

    g_variant_get (parameters, "(&s&sit)", &username, &password, &promise_id, &frame_id);
    password_manager = get_password_manager (extension, frame_id);
    if (password_manager != NULL)
      ret = jsc_value_object_invoke_method (password_manager, "_onQueryResponse",
                                            G_TYPE_STRING, username,
                                            G_TYPE_STRING, password,
                                            G_TYPE_INT, promise_id, G_TYPE_NONE);
  }
}

static const GDBusInterfaceVTable interface_vtable = {
  handle_method_call,
  NULL,
  NULL
};

static void
drop_frame_weak_ref (gpointer key,
                     gpointer value,
                     gpointer extension)
{
  g_object_weak_unref (G_OBJECT (value), (GWeakNotify)frame_destroyed_notify, extension);
}

static void
ephy_web_process_extension_dispose (GObject *object)
{
  EphyWebProcessExtension *extension = EPHY_WEB_PROCESS_EXTENSION (object);

  g_clear_object (&extension->overview_model);
  g_clear_object (&extension->permissions_manager);

  if (extension->page_created_signals_pending) {
    g_array_free (extension->page_created_signals_pending, TRUE);
    extension->page_created_signals_pending = NULL;
  }

  g_clear_object (&extension->script_world);
  g_clear_object (&extension->dbus_connection);
  g_clear_object (&extension->extension);

  if (extension->frames_map) {
    g_hash_table_foreach (extension->frames_map, drop_frame_weak_ref, extension);
    g_clear_pointer (&extension->frames_map, g_hash_table_unref);
  }

  G_OBJECT_CLASS (ephy_web_process_extension_parent_class)->dispose (object);
}

static void
ephy_web_process_extension_class_init (EphyWebProcessExtensionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = ephy_web_process_extension_dispose;
}

static void
ephy_web_process_extension_init (EphyWebProcessExtension *extension)
{
  extension->overview_model = ephy_web_overview_model_new ();
}

static gpointer
ephy_web_process_extension_create_instance (gpointer data)
{
  return g_object_new (EPHY_TYPE_WEB_PROCESS_EXTENSION, NULL);
}

EphyWebProcessExtension *
ephy_web_process_extension_get (void)
{
  static GOnce once_init = G_ONCE_INIT;
  return EPHY_WEB_PROCESS_EXTENSION (g_once (&once_init, ephy_web_process_extension_create_instance, NULL));
}

static void
dbus_connection_created_cb (GObject                 *source_object,
                            GAsyncResult            *result,
                            EphyWebProcessExtension *extension)
{
  static GDBusNodeInfo *introspection_data = NULL;
  g_autoptr (GDBusConnection) connection = NULL;
  guint registration_id;
  g_autoptr (GError) error = NULL;

  if (!introspection_data)
    introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);

  connection = g_dbus_connection_new_for_address_finish (result, &error);
  if (error) {
    g_warning ("Failed to connect to UI process: %s", error->message);
    return;
  }

  registration_id =
    g_dbus_connection_register_object (connection,
                                       EPHY_WEB_PROCESS_EXTENSION_OBJECT_PATH,
                                       introspection_data->interfaces[0],
                                       &interface_vtable,
                                       extension,
                                       NULL,
                                       &error);
  if (!registration_id) {
    g_warning ("Failed to register web process extension object: %s\n", error->message);
    return;
  }

  extension->dbus_connection = g_steal_pointer (&connection);
  ephy_web_process_extension_emit_page_created_signals_pending (extension);
}

static gboolean
authorize_authenticated_peer_cb (GDBusAuthObserver       *observer,
                                 GIOStream               *stream,
                                 GCredentials            *credentials,
                                 EphyWebProcessExtension *extension)
{
  return ephy_dbus_peer_is_authorized (credentials);
}

static void
js_log (const char *message)
{
  LOG ("%s", message);
}

static char *
js_gettext (const char *message)
{
  return g_strdup (g_dgettext (GETTEXT_PACKAGE, message));
}

static void
js_auto_fill (JSCValue   *js_element,
              const char *value)
{
  WebKitDOMNode *node;
  WebKitDOMElement *element;

  node = webkit_dom_node_for_js_value (js_element);
  element = WEBKIT_DOM_ELEMENT (node);

  webkit_dom_element_html_input_element_set_auto_filled (element, TRUE);
  webkit_dom_element_html_input_element_set_editing_value (element, value);
}

static gboolean
js_is_web_application (void)
{
  return ephy_profile_dir_is_web_application ();
}

static gboolean
js_is_edited (JSCValue *js_element)
{
  WebKitDOMNode *node = webkit_dom_node_for_js_value (js_element);

  return webkit_dom_element_html_input_element_is_user_edited (WEBKIT_DOM_ELEMENT (node));
}

static gboolean
js_should_remember_passwords (EphyWebProcessExtension *extension)
{
  g_assert (EPHY_IS_WEB_PROCESS_EXTENSION (extension));

  return !extension->is_private_profile && g_settings_get_boolean (EPHY_SETTINGS_WEB_PROCESS_EXTENSION_WEB, EPHY_PREFS_WEB_REMEMBER_PASSWORDS);
}

static void
js_exception_handler (JSCContext   *context,
                      JSCException *exception)
{
  g_autoptr (JSCValue) js_console = NULL;
  g_autoptr (JSCValue) js_result = NULL;
  g_autofree char *report = NULL;

  js_console = jsc_context_get_value (context, "console");
  js_result = jsc_value_object_invoke_method (js_console, "error", JSC_TYPE_EXCEPTION, exception, G_TYPE_NONE);
  report = jsc_exception_report (exception);
  g_warning ("%s", report);

  jsc_context_throw_exception (context, exception);
}

static void
window_object_cleared_cb (WebKitScriptWorld       *world,
                          WebKitWebPage           *page,
                          WebKitFrame             *frame,
                          EphyWebProcessExtension *extension)
{
  g_autoptr (JSCContext) js_context = NULL;
  g_autoptr (GBytes) bytes = NULL;
  const char *data;
  gsize data_size;
  g_autoptr (JSCValue) js_ephy = NULL;
  g_autoptr (JSCValue) js_function = NULL;
  g_autoptr (JSCValue) result = NULL;

  js_context = webkit_frame_get_js_context_for_script_world (frame, world);
  jsc_context_push_exception_handler (js_context, (JSCExceptionHandler)js_exception_handler, NULL, NULL);

  bytes = g_resources_lookup_data ("/org/gnome/epiphany-web-process-extension/js/ephy.js", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  data = g_bytes_get_data (bytes, &data_size);
  result = jsc_context_evaluate_with_source_uri (js_context, data, data_size, "resource:///org/gnome/epiphany-web-process-extension/js/ephy.js", 1);
  g_clear_pointer (&bytes, g_bytes_unref);
  g_clear_object (&result);

  js_ephy = jsc_context_get_value (js_context, "Ephy");

  js_function = jsc_value_new_function (js_context,
                                        "log",
                                        G_CALLBACK (js_log), NULL, NULL,
                                        G_TYPE_NONE, 1,
                                        G_TYPE_STRING);
  jsc_value_object_set_property (js_ephy, "log", js_function);
  g_clear_object (&js_function);

  js_function = jsc_value_new_function (js_context,
                                        "gettext",
                                        G_CALLBACK (js_gettext), NULL, NULL,
                                        G_TYPE_STRING, 1,
                                        G_TYPE_STRING);
  jsc_value_object_set_property (js_ephy, "_", js_function);
  g_clear_object (&js_function);

  if (g_strcmp0 (webkit_web_page_get_uri (page), "ephy-about:overview") == 0) {
    g_autoptr (JSCValue) js_overview = NULL;
    g_autoptr (JSCValue) js_overview_ctor = NULL;
    g_autoptr (JSCValue) jsc_overview_model = NULL;

    bytes = g_resources_lookup_data ("/org/gnome/epiphany-web-process-extension/js/overview.js", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    data = g_bytes_get_data (bytes, &data_size);
    result = jsc_context_evaluate_with_source_uri (js_context, data, data_size, "resource:///org/gnome/epiphany-web-process-extension/js/overview.js", 1);
    g_clear_pointer (&bytes, g_bytes_unref);
    g_clear_object (&result);

    jsc_overview_model = ephy_web_overview_model_export_to_js_context (extension->overview_model,
                                                                       js_context);

    js_overview_ctor = jsc_value_object_get_property (js_ephy, "Overview");
    js_overview = jsc_value_constructor_call (js_overview_ctor,
                                              JSC_TYPE_VALUE, jsc_overview_model,
                                              G_TYPE_NONE);
    jsc_value_object_set_property (js_ephy, "overview", js_overview);
  }

  ephy_permissions_manager_export_to_js_context (extension->permissions_manager,
                                                 js_context,
                                                 js_ephy);

  if (!extension->is_private_profile) {
    g_autoptr (JSCValue) js_password_manager_ctor = jsc_value_object_get_property (js_ephy, "PasswordManager");
    g_autoptr (JSCValue) js_password_manager = jsc_value_constructor_call (js_password_manager_ctor,
                                                                           G_TYPE_UINT64, webkit_web_page_get_id (page),
                                                                           G_TYPE_UINT64, webkit_frame_get_id (frame),
                                                                           G_TYPE_NONE);
    jsc_value_object_set_property (js_ephy, "passwordManager", js_password_manager);

    js_function = jsc_value_new_function (js_context,
                                          "autoFill",
                                          G_CALLBACK (js_auto_fill), NULL, NULL,
                                          G_TYPE_NONE, 2,
                                          JSC_TYPE_VALUE, G_TYPE_STRING);
    jsc_value_object_set_property (js_ephy, "autoFill", js_function);
    g_clear_object (&js_function);
  }

  js_function = jsc_value_new_function (js_context,
                                        "isWebApplication",
                                        G_CALLBACK (js_is_web_application), NULL, NULL,
                                        G_TYPE_BOOLEAN, 0);
  jsc_value_object_set_property (js_ephy, "isWebApplication", js_function);
  g_clear_object (&js_function);

  js_function = jsc_value_new_function (js_context,
                                        "isEdited",
                                        G_CALLBACK (js_is_edited), NULL, NULL,
                                        G_TYPE_BOOLEAN, 1,
                                        JSC_TYPE_VALUE);
  jsc_value_object_set_property (js_ephy, "isEdited", js_function);
  g_clear_object (&js_function);

  js_function = jsc_value_new_function (js_context,
                                        "shouldRememberPasswords",
                                        G_CALLBACK (js_should_remember_passwords),
                                        g_object_ref (extension), g_object_unref,
                                        G_TYPE_BOOLEAN, 0);
  jsc_value_object_set_property (js_ephy, "shouldRememberPasswords", js_function);
  g_clear_object (&js_function);
}

void
ephy_web_process_extension_initialize (EphyWebProcessExtension *extension,
                                       WebKitWebExtension      *wk_extension,
                                       const char              *guid,
                                       const char              *server_address,
                                       gboolean                 is_private_profile,
                                       gboolean                 is_browser_mode)
{
  g_autoptr (GDBusAuthObserver) observer = NULL;

  g_assert (EPHY_IS_WEB_PROCESS_EXTENSION (extension));

  if (extension->initialized)
    return;

  extension->initialized = TRUE;

  extension->script_world = webkit_script_world_new_with_name (guid);
  g_signal_connect (extension->script_world,
                    "window-object-cleared",
                    G_CALLBACK (window_object_cleared_cb),
                    extension);

  extension->extension = g_object_ref (wk_extension);

  extension->is_private_profile = is_private_profile;

  extension->permissions_manager = ephy_permissions_manager_new ();

  g_signal_connect_swapped (extension->extension, "page-created",
                            G_CALLBACK (ephy_web_process_extension_page_created_cb),
                            extension);

  observer = g_dbus_auth_observer_new ();
  g_signal_connect (observer, "authorize-authenticated-peer",
                    G_CALLBACK (authorize_authenticated_peer_cb), extension);

  g_dbus_connection_new_for_address (server_address,
                                     G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                     observer,
                                     NULL,
                                     (GAsyncReadyCallback)dbus_connection_created_cb,
                                     extension);

  extension->frames_map = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                 NULL, NULL);
}
