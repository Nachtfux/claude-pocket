#include "store.h"

#include <ArduinoJson.h>
#include <Preferences.h>

namespace settings {

namespace {
constexpr const char* NS = "pocket";
constexpr size_t      MAX_KNOWN_NETWORKS = 8;
Store g_store;

std::string serialize_networks() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (auto& n : g_store.known_networks) {
        JsonObject o = arr.add<JsonObject>();
        o["s"] = n.ssid;
        o["p"] = n.pass;
    }
    std::string out;
    serializeJson(arr, out);
    return out;
}

void deserialize_networks(const std::string& json) {
    g_store.known_networks.clear();
    if (json.empty()) return;
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;
    for (JsonObject o : doc.as<JsonArray>()) {
        Network n;
        n.ssid = (const char*)(o["s"] | "");
        n.pass = (const char*)(o["p"] | "");
        if (!n.ssid.empty()) g_store.known_networks.push_back(n);
    }
}
}  // namespace

Store& store() { return g_store; }

void load() {
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/true)) return;
    g_store.wifi_ssid    = p.getString("ssid", "").c_str();
    g_store.wifi_pass    = p.getString("pass", "").c_str();
    g_store.bluetooth_on   = p.getBool("bt", false);
    g_store.volume_pct     = p.getUChar("vol", 60);
    g_store.brightness_pct = p.getUChar("bright", 80);
    g_store.wake_word_on   = p.getBool("wake", false);
    std::string nets = p.getString("nets", "").c_str();
    p.end();
    deserialize_networks(nets);
    // Back-compat: if we just loaded a non-empty wifi_ssid from an older
    // build that didn't track a roaming pool yet, seed the pool with it.
    if (g_store.known_networks.empty() && !g_store.wifi_ssid.empty()) {
        g_store.known_networks.push_back({g_store.wifi_ssid, g_store.wifi_pass});
    }
}

void save() {
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return;
    p.putString("ssid", g_store.wifi_ssid.c_str());
    p.putString("pass", g_store.wifi_pass.c_str());
    p.putBool("bt", g_store.bluetooth_on);
    p.putUChar("vol", g_store.volume_pct);
    p.putUChar("bright", g_store.brightness_pct);
    p.putBool("wake", g_store.wake_word_on);
    p.putString("nets", serialize_networks().c_str());
    p.end();
}

void remember_network(const std::string& ssid, const std::string& pass) {
    if (ssid.empty()) return;
    // Drop any existing entry for this SSID so we can re-insert at the
    // front with the current password.
    auto& nets = g_store.known_networks;
    for (auto it = nets.begin(); it != nets.end(); ) {
        if (it->ssid == ssid) it = nets.erase(it);
        else                  ++it;
    }
    nets.insert(nets.begin(), {ssid, pass});
    if (nets.size() > MAX_KNOWN_NETWORKS) nets.resize(MAX_KNOWN_NETWORKS);
    g_store.wifi_ssid = ssid;
    g_store.wifi_pass = pass;
}

std::string password_for(const std::string& ssid) {
    for (auto& n : g_store.known_networks) {
        if (n.ssid == ssid) return n.pass;
    }
    return "";
}

}  // namespace settings
