/******************************************************************************
* Copyright 1998-2024 NetBurner, Inc.  ALL RIGHTS RESERVED
*
*    Permission is hereby granted to purchasers of NetBurner Hardware to use or
*    modify this computer program for any use as long as the resultant program
*    is only executed on NetBurner provided hardware.
*
*    No other rights to use this program or its derivatives in part or in
*    whole are granted.
*
*    It may be possible to license this or other NetBurner software for use on
*    non-NetBurner Hardware. Contact sales@Netburner.com for more information.
*
*    NetBurner makes no representation or warranties with respect to the
*    performance of this computer program, and specifically disclaims any
*    responsibility for any damages, special or consequential, connected with
*    the use of this program.
*
* NetBurner
* 16855 W Bernardo Dr
* San Diego, CA 92127
* www.netburner.com
******************************************************************************/

// HTTP handlers for admin UI authentication and MQTT user management APIs.
// CheckHttpAccess gates pages by monitor_config credentials; JSON endpoints
// under api/password and api/mqtt/users persist settings to NV/config storage.

#include "broker_config.hpp"
#include "mqtt_auth_store.h"
#include "nvsettings.h"
#include "webui_config.h"

#include <config_obj.h>
#include <config_server.h>
#include <fdprintf.h>
#include <http.h>
#include <httppost.h>

extern WebUIConfig gWebUIConfig;

// NNDK hook: returns HTTP_NEED_PASSWORD when the page requires login and
// credentials are missing or wrong. Empty system_pass disables auth entirely.
HTTP_ACCESS CheckHttpAccess(int sock, int accessGroup, HTTP_Request &Req)
{
    (void)sock;

    if (accessGroup > 1 || gWebUIConfig.m_requireAuthForAll) {
        NBString sysPass = monitor_config.system_pass;

        if (sysPass == "") {
            return HTTP_OK_TO_SERVE;
        }

        NBString sysUser = monitor_config.system_user;

        char *providedPass;
        char *providedUser;
        if (Req.ExtractAuthentication(&providedPass, &providedUser)) {
            NBString pp = providedPass;
            if (sysPass == pp && sysUser == providedUser) {
                return HTTP_OK_TO_SERVE;
            }
        }

        return HTTP_NEED_PASSWORD;
    }

    return HTTP_OK_TO_SERVE;
}

// POST api/password — set or clear the admin password (empty clears auth).
void PasswordChangeHandler(int sock, ParsedJsonDataSet &JsonSet)
{
    NBString oldPassword = JsonSet.FindString("oldPassword");
    NBString newPassword = JsonSet.FindString("newPassword");
    NBString repeatPassword = JsonSet.FindString("repeatPassword");

    NBString currentPassword = monitor_config.system_pass;
    const bool accessControlEnabled = NV_Settings.setupComplete;

    if (newPassword != repeatPassword) {
        writestring(sock, "HTTP/1.0 400 Bad Request\r\n");
        writestring(sock, "Content-Type: application/json\r\n\r\n");
        writestring(sock, "{\"success\":false,\"message\":\"New passwords do not match\"}\r\n");
        return;
    }

    // First password set skips old-password check until setupComplete is true.
    if (accessControlEnabled) {
        if (oldPassword != currentPassword) {
            writestring(sock, "HTTP/1.0 401 Unauthorized\r\n");
            writestring(sock, "Content-Type: application/json\r\n\r\n");
            writestring(sock, "{\"success\":false,\"message\":\"Current password is incorrect\"}\r\n");
            return;
        }
    }

    if (newPassword.empty()) {
        // Empty password disables web UI login entirely.
        monitor_config.system_user = "";
        monitor_config.system_pass = "";
    } else {
        monitor_config.system_user = "admin";
        monitor_config.system_pass = newPassword;
    }

    NV_Settings.setupComplete = true;
    SaveNVSettings();
    SaveConfigToStorage();

    writestring(sock, "HTTP/1.0 200 OK\r\n");
    writestring(sock, "Content-Type: application/json\r\n\r\n");
    writestring(sock, "{\"success\":true,\"message\":\"Password updated successfully\"}\r\n");
}

JsonPostCallbackHandler PasswordPost("api/password", PasswordChangeHandler, 99);

// Reject control chars and quotes so usernames embed safely in JSON responses.
static bool username_json_safe(const char *name)
{
    for (const char *p = name; *p != '\0'; ++p) {
        if (*p < 0x20 || *p == '"' || *p == '\\' || *p > 0x7E) {
            return false;
        }
    }
    return name[0] != '\0';
}

static void json_reply(int sock, bool ok, const char *message)
{
    writestring(sock, ok ? "HTTP/1.0 200 OK\r\n" : "HTTP/1.0 400 Bad Request\r\n");
    writestring(sock, "Content-Type: application/json\r\n\r\n");
    fdprintf(sock, "{\"success\":%s,\"message\":\"%s\"}\r\n", ok ? "true" : "false", message);
}

// GET api/mqtt/users — list stored usernames and broker auth policy flags.
int MqttUsersGetHandler(int sock, HTTP_Request &req)
{
    (void)req;
    writestring(sock, "HTTP/1.0 200 OK\r\n");
    writestring(sock, "Content-Type: application/json\r\n");
    writestring(sock, "Cache-Control: no-cache\r\n\r\n");
    fdprintf(sock, "{\"auth_required\":%s,\"allow_anonymous\":%s,\"max_users\":%d,\"users\":[",
             BrokerConfigAuthRequired() ? "true" : "false",
             BrokerConfigAllowAnonymous() ? "true" : "false", kMqttAuthMaxUsers);
    int count = MqttAuthUserCount();
    bool first = true;
    for (int i = 0; i < count; ++i) {
        char name[kMqttAuthMaxUserLen];
        if (MqttAuthUserAt(i, name, sizeof(name)) && username_json_safe(name)) {
            fdprintf(sock, "%s\"%s\"", first ? "" : ",", name);
            first = false;
        }
    }
    writestring(sock, "]}\r\n");
    return 1;
}

// POST api/mqtt/users — action "settings", "add", or "delete" for MQTT credentials.
void MqttUsersPostHandler(int sock, ParsedJsonDataSet &JsonSet)
{
    NBString action = JsonSet.FindString("action");

    if (action == "settings") {
        NBString required = JsonSet.FindString("authRequired");
        NBString anonymous = JsonSet.FindString("allowAnonymous");
        BrokerConfigSetAuth(required == "true", anonymous == "true");
        json_reply(sock, true, "Authentication settings updated");
        return;
    }

    NBString username = JsonSet.FindString("username");
    if (username.empty() || !username_json_safe(username.c_str())) {
        json_reply(sock, false, "Invalid username");
        return;
    }
    if (username.length() >= (size_t)kMqttAuthMaxUserLen) {
        json_reply(sock, false, "Username too long");
        return;
    }

    if (action == "add") {
        NBString password = JsonSet.FindString("password");
        if (password.empty()) {
            json_reply(sock, false, "Password must not be empty");
            return;
        }
        if (MqttAuthSetUser(username.c_str(), password.c_str(), password.length())) {
            json_reply(sock, true, "User saved");
        } else {
            json_reply(sock, false, "User table full");
        }
        return;
    }

    if (action == "delete") {
        if (MqttAuthDeleteUser(username.c_str())) {
            json_reply(sock, true, "User deleted");
        } else {
            json_reply(sock, false, "User not found");
        }
        return;
    }

    json_reply(sock, false, "Unknown action");
}

CallBackFunctionPageHandler MqttUsersGet("api/mqtt/users", MqttUsersGetHandler, tGet, 1);
JsonPostCallbackHandler MqttUsersPost("api/mqtt/users", MqttUsersPostHandler, 99);
