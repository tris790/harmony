#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>

// Simple Portal Wrapper

static DBusConnection *conn;

// Helper: Wait for Response signal on a Request path
// Returns 0 on success, non-zero on failure.
// If success, populates out_results (DBusMessage with the variant dict) - caller must ref it? 
// Actually, we'll just extract what we need inside the loop to specific pointers if provided.
// For simplicity, let's just make specific helpers or a generic one.
// Generic: Returns the DBusMessage* of the signal (Response). Caller parses results.
static DBusMessage* WaitForResponse(const char *request_path) {
    DBusError err;
    dbus_error_init(&err);
    
    char match[512];
    snprintf(match, sizeof(match), "type='signal',interface='org.freedesktop.portal.Request',path='%s',member='Response'", request_path);
    dbus_bus_add_match(conn, match, &err);
    dbus_connection_flush(conn);
    
    while(true) {
        dbus_connection_read_write(conn, 100);
        DBusMessage *msg = dbus_connection_pop_message(conn);
        if(!msg) continue;
        
        if(dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
            // Check path (paranoid, though match handles it mostly)
            if(strcmp(dbus_message_get_path(msg), request_path) == 0) {
                return msg;
            }
        }
        dbus_message_unref(msg);
    }
}

static void ExtractSessionHandle(DBusMessage *response, char *out_handle, size_t max_len) {
    DBusMessageIter iter, results_iter, dict, v;
    dbus_message_iter_init(response, &iter);
    
    uint32_t response_code;
    dbus_message_iter_get_basic(&iter, &response_code);
    
    if(response_code != 0) return;
    
    dbus_message_iter_next(&iter);
    dbus_message_iter_recurse(&iter, &results_iter);
    
    while(dbus_message_iter_get_arg_type(&results_iter) != DBUS_TYPE_INVALID) {
        dbus_message_iter_recurse(&results_iter, &dict);
        char *key;
        dbus_message_iter_get_basic(&dict, &key);
        dbus_message_iter_next(&dict);
        dbus_message_iter_recurse(&dict, &v);
        
        if(strcmp(key, "session_handle") == 0) {
            char *h;
            dbus_message_iter_get_basic(&v, &h);
            strncpy(out_handle, h, max_len - 1);
        }
        dbus_message_iter_next(&results_iter);
    }
}

uint32_t Portal_RequestScreenCast() {
    DBusError err;
    dbus_error_init(&err);

    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "DBus Connection Error: %s\n", err.message);
        dbus_error_free(&err);
        return 0;
    }

    const char *portal_bus_name = "org.freedesktop.portal.Desktop";
    const char *portal_obj_path = "/org/freedesktop/portal/desktop";
    const char *screencast_iface = "org.freedesktop.portal.ScreenCast";
    
    // 1. CreateSession
    DBusMessage *msg = dbus_message_new_method_call(portal_bus_name, portal_obj_path, screencast_iface, "CreateSession");
    DBusMessageIter args, array;
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &array);
    
    // Add "session_handle_token" -> "harmony_token"
    const char *key = "session_handle_token";
    const char *value = "harmony_token";
    DBusMessageIter dict, val;
    dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
    dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "s", &val);
    dbus_message_iter_append_basic(&val, DBUS_TYPE_STRING, &value);
    dbus_message_iter_close_container(&dict, &val);
    dbus_message_iter_close_container(&array, &dict);
    
    dbus_message_iter_close_container(&args, &array);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg);
    if (!reply) {
         fprintf(stderr, "CreateSession Call Failed: %s\n", err.message);
         dbus_error_free(&err);
         return 0;
    }
    
    char *request_path = NULL;
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &request_path, DBUS_TYPE_INVALID);
    char create_req_path[256];
    strncpy(create_req_path, request_path, 255);
    dbus_message_unref(reply);
    
    printf("CreateSession Request: %s\n", create_req_path);
    
    DBusMessage *resp = WaitForResponse(create_req_path);
    char session_handle[256] = {0};
    ExtractSessionHandle(resp, session_handle, sizeof(session_handle));
    dbus_message_unref(resp);
    
    if (strlen(session_handle) == 0) {
        fprintf(stderr, "Failed to get session handle\n");
        return 0;
    }
    printf("Session Handle: %s\n", session_handle);

    // 2. SelectSources
    msg = dbus_message_new_method_call(portal_bus_name, portal_obj_path, screencast_iface, "SelectSources");
    dbus_message_iter_init_append(msg, &args);
    const char *sess_ptr = session_handle;
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sess_ptr);
    
    // Reuse existing iterators
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &array);
    
    // cursor_mode = 2
    key = "cursor_mode"; 
    uint32_t uval = 2; 
    dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
    dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "u", &val);
    dbus_message_iter_append_basic(&val, DBUS_TYPE_UINT32, &uval);
    dbus_message_iter_close_container(&dict, &val);
    dbus_message_iter_close_container(&array, &dict);

    // types = 3
    key = "types";
    uval = 3;
    dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, NULL, &dict);
    dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&dict, DBUS_TYPE_VARIANT, "u", &val);
    dbus_message_iter_append_basic(&val, DBUS_TYPE_UINT32, &uval);
    dbus_message_iter_close_container(&dict, &val);
    dbus_message_iter_close_container(&array, &dict);
    
    dbus_message_iter_close_container(&args, &array);
    
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg);
    
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &request_path, DBUS_TYPE_INVALID);
    char select_req_path[256];
    strncpy(select_req_path, request_path, 255);
    dbus_message_unref(reply);
    
    printf("SelectSources Request: %s\n", select_req_path);
    resp = WaitForResponse(select_req_path); // Just wait for success
    dbus_message_unref(resp);
    
    // 3. Start
    msg = dbus_message_new_method_call(portal_bus_name, portal_obj_path, screencast_iface, "Start");
    dbus_message_iter_init_append(msg, &args);
    dbus_message_iter_append_basic(&args, DBUS_TYPE_OBJECT_PATH, &sess_ptr);
    const char *parent = "";
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &array);
    dbus_message_iter_close_container(&args, &array);

    printf("Starting Session... Please allow.\n");
    reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg);
    if (!reply) {
         fprintf(stderr, "Start Call Failed: %s\n", err.message);
         dbus_error_free(&err);
         return 0;
    }
    
    dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &request_path, DBUS_TYPE_INVALID);
    char start_req_path[256];
    strncpy(start_req_path, request_path, 255);
    dbus_message_unref(reply);
    
    resp = WaitForResponse(start_req_path);
    
    // Parse Start response for streams
    uint32_t node_id = 0;
    
    dbus_message_iter_init(resp, &args);
    uint32_t rc;
    dbus_message_iter_get_basic(&args, &rc);
    if (rc == 0) {
        dbus_message_iter_next(&args);
        DBusMessageIter r_iter, r_dict, r_v, str_arr, str_entry;
        dbus_message_iter_recurse(&args, &r_iter);
        
        while(dbus_message_iter_get_arg_type(&r_iter) != DBUS_TYPE_INVALID) {
            dbus_message_iter_recurse(&r_iter, &r_dict);
            dbus_message_iter_get_basic(&r_dict, &key);
            dbus_message_iter_next(&r_dict);
            dbus_message_iter_recurse(&r_dict, &r_v);
            
            if(strcmp(key, "streams") == 0) {
                dbus_message_iter_recurse(&r_v, &str_arr);
                 // Array of (node_id, map)
                 dbus_message_iter_recurse(&str_arr, &str_entry); // Struct
                 dbus_message_iter_get_basic(&str_entry, &node_id);
            }
            dbus_message_iter_next(&r_iter);
        }
    }
    
    dbus_message_unref(resp);
    
    return node_id;
}
