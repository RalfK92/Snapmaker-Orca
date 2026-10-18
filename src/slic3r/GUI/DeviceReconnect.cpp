#include "DeviceReconnect.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "SSWCP.hpp"
#include "slic3r/Utils/MQTT.hpp"

#include <boost/log/trivial.hpp>

#include <wx/timer.h>
#if wxUSE_SECRETSTORE
#include <wx/secretstore.h>
#endif

#include <map>
#include <memory>
#include <mutex>

namespace Slic3r { namespace GUI { namespace DeviceReconnect {

using json = nlohmann::json;

namespace {

const char *const CONFIG_KEY     = "last_connected_device"; // non-secret parameters (app section)
const char *const ENABLED_KEY    = "reconnect_last_device";
const char *const SECRET_SERVICE = "Snapmaker_Orca/last_connected_device";

// Parameters that must only ever live in the keyring.
const char *const SECRET_FIELDS[] = {"ca", "cert", "key", "username", "user", "password", "code"};

const int POLL_MS          = 250;
const int STEP_TIMEOUT_MS  = 30000;

// sw_create_mqtt_client parameters per webview, until the connection is completed.
std::map<wxWebView *, json> g_pending_clients;
std::mutex                  g_pending_mutex;

bool g_reconnect_attempted = false;

bool enabled() { return wxGetApp().app_config && wxGetApp().app_config->get_bool(ENABLED_KEY); }

// Move SECRET_FIELDS from `params` into `secrets`.
void split_secrets(json &params, json &secrets)
{
    secrets = json::object();
    for (const char *field : SECRET_FIELDS)
        if (params.contains(field)) {
            secrets[field] = params[field];
            params.erase(field);
        }
}

void merge_secrets(json &params, const json &secrets)
{
    if (secrets.is_object())
        for (auto it = secrets.begin(); it != secrets.end(); ++it)
            params[it.key()] = it.value();
}

#if wxUSE_SECRETSTORE
bool save_secret(const std::string &account, const std::string &secret)
{
    wxSecretStore store = wxSecretStore::GetDefault();
    wxString      err;
    if (!store.IsOk(&err)) {
        BOOST_LOG_TRIVIAL(warning) << "DeviceReconnect: no keyring available, printer is not remembered: " << err.ToUTF8();
        return false;
    }
    if (!store.Save(SECRET_SERVICE, wxString::FromUTF8(account), wxSecretValue(wxString::FromUTF8(secret)))) {
        BOOST_LOG_TRIVIAL(warning) << "DeviceReconnect: storing the printer credentials in the keyring failed";
        return false;
    }
    return true;
}

bool load_secret(std::string &secret)
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return false;
    wxString      account;
    wxSecretValue value;
    if (!store.Load(SECRET_SERVICE, account, value) || !value.IsOk())
        return false;
    secret = value.GetAsString().ToStdString(wxConvUTF8);
    return true;
}

void delete_secret()
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk())
        store.Delete(SECRET_SERVICE);
}
#else
// No keyring support in this wxWidgets build: never store credentials.
bool save_secret(const std::string &, const std::string &)
{
    BOOST_LOG_TRIVIAL(warning) << "DeviceReconnect: built without wxSecretStore, printer is not remembered";
    return false;
}
bool load_secret(std::string &) { return false; }
void delete_secret() {}
#endif

std::shared_ptr<MqttClient> replay_engine()
{
    // Replayed instances have no webview, so their engine is keyed by nullptr.
    std::lock_guard<std::mutex> lock(SSWCP_MqttAgent_Instance::m_engine_map_mtx);
    auto it = SSWCP_MqttAgent_Instance::m_mqtt_engine_map.find(nullptr);
    return it != SSWCP_MqttAgent_Instance::m_mqtt_engine_map.end() ? it->second.second : nullptr;
}

bool device_connected(const std::string &sn)
{
    for (const auto &device : wxGetApp().app_config->get_devices())
        if (device.connected && (sn.empty() || device.sn == sn || device.dev_id == sn))
            return true;
    return false;
}

// Replays sw_create_mqtt_client -> sw_mqtt_connect -> sw_mqtt_set_engine without a
// webview, polling for the result of each asynchronous step.
class Replay : public wxTimer
{
public:
    Replay(json create, json engine) : m_create(std::move(create)), m_engine(std::move(engine)) {}

    void start()
    {
        m_step = run("sw_create_mqtt_client", m_create);
        if (!m_step || m_step->m_status != 200 || !m_step->m_res_data.contains("id")) {
            finish("creating the MQTT client failed");
            return;
        }
        m_engine_id = m_step->m_res_data["id"].get<std::string>();
        m_stage     = Stage::Connect;
        m_step      = run("sw_mqtt_connect", json{{"id", m_engine_id}});
        Start(POLL_MS);
    }

    void Notify() override
    {
        m_elapsed += POLL_MS;
        if (!m_step || m_step->m_status != 200) {
            finish(m_step ? m_step->m_msg : "step failed");
            return;
        }
        if (m_stage == Stage::Connect) {
            auto engine = replay_engine();
            if (engine && engine->CheckConnected()) {
                json params         = m_engine;
                params["engine_id"] = m_engine_id;
                params["need_reload"] = true;
                m_stage   = Stage::SetEngine;
                m_elapsed = 0;
                m_step    = run("sw_mqtt_set_engine", params);
                return;
            }
        } else if (device_connected(m_engine.value("sn", ""))) {
            BOOST_LOG_TRIVIAL(warning) << "DeviceReconnect: reconnected to " << m_engine.value("sn", "");
            finish("");
            return;
        }
        if (m_elapsed >= STEP_TIMEOUT_MS)
            finish("timeout");
    }

private:
    enum class Stage { Create, Connect, SetEngine };

    static std::shared_ptr<SSWCP_Instance> run(const std::string &cmd, const json &params)
    {
        auto instance = SSWCP::create_sswcp_instance(cmd, json::object(), params, "", nullptr);
        if (instance)
            instance->process();
        return instance;
    }

    void finish(const std::string &error);

    json                            m_create, m_engine;
    std::string                     m_engine_id;
    std::shared_ptr<SSWCP_Instance> m_step;
    Stage                           m_stage   = Stage::Create;
    int                             m_elapsed = 0;
};

std::unique_ptr<Replay> g_replay;

void Replay::finish(const std::string &error)
{
    Stop();
    if (!error.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "DeviceReconnect: reconnecting to the last printer failed: " << error;
        if (m_stage != Stage::SetEngine || !device_connected("")) {
            std::string msg;
            if (auto engine = replay_engine())
                engine->Disconnect(msg);
        }
    }
    // Destroy the replay outside of its own timer callback.
    wxGetApp().CallAfter([] { g_replay.reset(); });
}

void start_replay()
{
    json stored;
    try {
        stored = json::parse(wxGetApp().app_config->get(CONFIG_KEY));
    } catch (...) {
        return;
    }
    std::string secret_str;
    if (!load_secret(secret_str)) {
        BOOST_LOG_TRIVIAL(warning) << "DeviceReconnect: printer credentials not found in the keyring";
        return;
    }
    json secrets;
    try {
        secrets = json::parse(secret_str);
    } catch (...) {
        return;
    }
    json create = stored.value("create", json::object()), engine = stored.value("engine", json::object());
    merge_secrets(create, secrets.value("create", json::object()));
    merge_secrets(engine, secrets.value("engine", json::object()));

    BOOST_LOG_TRIVIAL(warning) << "DeviceReconnect: reconnecting to " << engine.value("sn", "");
    g_replay = std::make_unique<Replay>(std::move(create), std::move(engine));
    g_replay->start();
}

} // namespace

void on_client_created(wxWebView *webview, const json &params)
{
    if (webview == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_pending_mutex);
    g_pending_clients[webview] = params;
}

void on_engine_set(wxWebView *webview, const json &params)
{
    if (webview == nullptr)
        return;
    json create;
    {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        auto it = g_pending_clients.find(webview);
        if (it == g_pending_clients.end())
            return;
        create = it->second;
        g_pending_clients.erase(it);
    }
    if (!enabled()) {
        forget();
        return;
    }

    json engine = params;
    engine.erase("engine_id"); // a fresh client gets a new id on replay
    engine.erase("need_reload");

    json create_secrets, engine_secrets;
    split_secrets(create, create_secrets);
    split_secrets(engine, engine_secrets);

    const std::string sn = engine.value("sn", "");
    if (!save_secret(sn, json{{"create", create_secrets}, {"engine", engine_secrets}}.dump()))
        return;
    wxGetApp().app_config->set(CONFIG_KEY, json{{"create", create}, {"engine", engine}}.dump());
    BOOST_LOG_TRIVIAL(warning) << "DeviceReconnect: remembered printer " << sn;
}

void forget()
{
    if (!wxGetApp().app_config || wxGetApp().app_config->get(CONFIG_KEY).empty())
        return;
    wxGetApp().app_config->set(CONFIG_KEY, "");
    delete_secret();
    BOOST_LOG_TRIVIAL(warning) << "DeviceReconnect: forgot the last printer";
}

void on_user_login(const std::string &user_id)
{
    if (g_reconnect_attempted || user_id.empty() || !enabled())
        return;
    const std::string stored_str = wxGetApp().app_config->get(CONFIG_KEY);
    if (stored_str.empty())
        return;
    std::string stored_user;
    try {
        stored_user = json::parse(stored_str).value("engine", json::object()).value("userid", "");
    } catch (...) {
        return;
    }
    // LAN connections carry no user id; cloud ones must belong to the logged-in user.
    if (!stored_user.empty() && stored_user != user_id)
        return;
    g_reconnect_attempted = true;

    // Let the main window and the Device page finish starting up first.
    wxGetApp().CallAfter([] {
        if (!wxGetApp().mainframe || g_replay || device_connected(""))
            return;
        start_replay();
    });
}

}}} // namespace Slic3r::GUI::DeviceReconnect
