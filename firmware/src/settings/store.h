#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace settings {

struct Network {
    std::string ssid;
    std::string pass;
};

// Persisted user preferences. Single source of truth for runtime; reflects what
// is currently on flash. Mutate then call save().
struct Store {
    std::string wifi_ssid;          // last successfully-connected network
    std::string wifi_pass;
    // Roaming pool: every network the user has ever connected to here. On
    // boot we walk this list if `wifi_ssid` fails, so picking the device
    // up at home / work / a café "just works" without re-entering passwords.
    std::vector<Network> known_networks;
    bool bluetooth_on = false;
    uint8_t volume_pct = 60;
    uint8_t brightness_pct = 80;
    bool wake_word_on = false;
};

Store& store();

void load();
void save();

// Add or update a network in the roaming pool, move it to the front so it
// becomes the next-tried entry, and update `wifi_ssid`/`wifi_pass` to match.
// Caller is responsible for calling save() afterwards.
void remember_network(const std::string& ssid, const std::string& pass);

// Look up a saved password for an SSID, or empty string if unknown.
std::string password_for(const std::string& ssid);

}  // namespace settings
