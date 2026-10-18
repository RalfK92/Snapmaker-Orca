#pragma once

#include <string>

#include "nlohmann/json.hpp"

class wxWebView;

// Reconnect to the last connected printer on startup.
//
// The Device page (Flutter) connects a printer through a sequence of WCP calls:
// sw_create_mqtt_client -> sw_mqtt_connect -> sw_mqtt_set_engine. The app normally
// starts disconnected, so the printer has to be picked again after every restart.
// This module remembers the parameters of the last successful connection and, once
// the same user is logged in again, replays that sequence natively. The Device page
// then finds a connected printer, exactly as after a manual connect.
//
// Certificates, keys and passwords never go to the app config: they are kept in the
// system keyring (wxSecretStore: libsecret / Secret portal, Windows Credential
// Manager, macOS Keychain). Without a usable keyring nothing is remembered.
namespace Slic3r { namespace GUI { namespace DeviceReconnect {

// Hooks called by SSWCP for connections started from a web page.
void on_client_created(wxWebView *webview, const nlohmann::json &params);
void on_engine_set(wxWebView *webview, const nlohmann::json &params);

// The user disconnected the printer on purpose: forget it.
void forget();

// A user logged in; reconnects once per session if the remembered printer belongs to them.
void on_user_login(const std::string &user_id);

}}} // namespace Slic3r::GUI::DeviceReconnect
