#include "blekbd.h"

#include <M5Cardputer.h>
#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

#include <string.h>

#include "../app.h"
#include "../theme.h"

namespace apps {
namespace blekbd {

namespace {

constexpr const char* DEVICE_NAME = "Claude Pocket KB";
constexpr uint8_t REPORT_ID_KEYBOARD = 1;
constexpr uint16_t APPEARANCE_KEYBOARD = 0x03C1;

// Standard boot-compatible keyboard: 1 byte modifiers, 1 reserved byte,
// 6 key slots; plus a 5-bit LED output report the host writes to.
const uint8_t REPORT_MAP[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, REPORT_ID_KEYBOARD,  //   Report ID
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (Left Ctrl)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Var, Abs) — modifier bits
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const) — reserved byte
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data, Var, Abs) — LED report
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const) — LED padding
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array) — 6 key slots
    0xC0,              // End Collection
};

// HID modifier bits (byte 0 of the report).
constexpr uint8_t MOD_LCTRL  = 0x01;
constexpr uint8_t MOD_LSHIFT = 0x02;
constexpr uint8_t MOD_LALT   = 0x04;
constexpr uint8_t MOD_LGUI   = 0x08;

NimBLEServer*         g_server = nullptr;
NimBLEHIDDevice*      g_hid    = nullptr;
NimBLECharacteristic* g_input  = nullptr;

// Written from the NimBLE host task, read from loop().
volatile bool g_connected     = false;
volatile bool g_needs_repaint = false;

uint8_t  g_last_report[8] = {0};
uint8_t  g_last_key_count = 0;
uint32_t g_last_batt_ms   = 0;
bool     g_exit_armed     = false;

// Small echo of what was typed so the user sees keystrokes going out.
constexpr size_t ECHO_LEN = 28;
char g_echo[ECHO_LEN + 1] = {0};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override {
        g_connected = true;
        g_needs_repaint = true;
    }
    void onDisconnect(NimBLEServer*) override {
        g_connected = false;
        g_needs_repaint = true;
        NimBLEDevice::startAdvertising();
    }
};
ServerCallbacks g_server_cb;

uint16_t to565(uint32_t rgb) {
    return M5.Display.color565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

uint8_t battery_pct() {
    int lvl = M5.Power.getBatteryLevel();
    if (lvl < 0) lvl = 0;
    if (lvl > 100) lvl = 100;
    return (uint8_t)lvl;
}

void ble_start() {
    Serial.printf("[blekbd] starting BLE — heap=%u\n", (unsigned)ESP.getFreeHeap());
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    // "Just works" pairing with bonding — the Cardputer has no way to show
    // or confirm a passkey in a way every host supports. Bonds persist in
    // NVS, so a paired host reconnects on its own next time.
    NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(&g_server_cb, /*deleteCallbacks=*/false);

    g_hid = new NimBLEHIDDevice(g_server);
    g_input = g_hid->inputReport(REPORT_ID_KEYBOARD);
    g_hid->outputReport(REPORT_ID_KEYBOARD);  // LED state; contents ignored
    g_hid->manufacturer()->setValue(std::string("M5Stack"));
    g_hid->pnp(0x02, 0xe502, 0xa111, 0x0210);
    g_hid->hidInfo(0x00, 0x01);
    g_hid->reportMap((uint8_t*)REPORT_MAP, sizeof(REPORT_MAP));
    g_hid->startServices();
    g_hid->setBatteryLevel(battery_pct());

    NimBLEAdvertising* adv = g_server->getAdvertising();
    adv->setAppearance(APPEARANCE_KEYBOARD);
    adv->addServiceUUID(g_hid->hidService()->getUUID());
    adv->setScanResponse(true);
    adv->start();
    Serial.printf("[blekbd] advertising as \"%s\" — heap=%u\n", DEVICE_NAME,
                  (unsigned)ESP.getFreeHeap());
}

void send_report(const uint8_t rep[8]) {
    if (!g_input || !g_connected) return;
    g_input->setValue(rep, 8);
    g_input->notify();
}

void ble_stop() {
    // Release everything on the host side before the link goes away, or a
    // held key keeps auto-repeating on the computer.
    const uint8_t empty[8] = {0};
    send_report(empty);
    delay(20);

    // deinit(true) deletes the server, services and characteristics; the
    // NimBLEHIDDevice wrapper itself is ours to free.
    delete g_hid;
    g_hid = nullptr;
    g_input = nullptr;
    g_server = nullptr;
    NimBLEDevice::deinit(/*clearAll=*/true);
    g_connected = false;
    Serial.printf("[blekbd] BLE stopped — heap=%u\n", (unsigned)ESP.getFreeHeap());
}

// M5Cardputer 1.1.x has no Fn layer, so build it here from the printed
// legends: Fn + ` = Esc, Fn + 1..= = F1..F12, Fn + ; , . / = arrows,
// Fn + Backspace = Delete. Anything else passes through unchanged.
uint8_t fn_layer(uint8_t hid) {
    switch (hid) {
        case 0x35: return 0x29;   // `  -> Escape
        case 0x2D: return 0x44;   // -  -> F11
        case 0x2E: return 0x45;   // =  -> F12
        case 0x33: return 0x52;   // ;  -> Up
        case 0x36: return 0x50;   // ,  -> Left
        case 0x37: return 0x51;   // .  -> Down
        case 0x38: return 0x4F;   // /  -> Right
        case 0x2A: return 0x4C;   // Backspace -> Delete
    }
    if (hid >= 0x1E && hid <= 0x27) return (uint8_t)(0x3A + (hid - 0x1E));  // 1..0 -> F1..F10
    return hid;
}

void push_echo(char c) {
    size_t n = strlen(g_echo);
    if (n >= ECHO_LEN) {
        memmove(g_echo, g_echo + 1, ECHO_LEN - 1);
        n = ECHO_LEN - 1;
    }
    g_echo[n] = c;
    g_echo[n + 1] = '\0';
}

// Echo line — last few characters sent. Repainted on its own per keystroke
// so typing doesn't flicker the whole screen.
void paint_echo() {
    M5.Display.fillRoundRect(8, theme::CONTENT_TOP + 80, theme::SCREEN_W - 16, 18,
                             4, to565(theme::LIGHT_GRAY));
    M5.Display.setTextColor(to565(theme::DARK), to565(theme::LIGHT_GRAY));
    M5.Display.setTextSize(1.2f);
    M5.Display.setTextDatum(middle_left);
    M5.Display.drawString(g_echo, 14, theme::CONTENT_TOP + 89);
    M5.Display.setTextDatum(top_left);
}

void paint() {
    M5.Display.fillRect(0, theme::CONTENT_TOP, theme::SCREEN_W,
                        theme::CONTENT_H, to565(theme::IVORY));
    M5.Display.setTextDatum(top_left);

    M5.Display.setTextColor(to565(theme::DARK), to565(theme::IVORY));
    M5.Display.setTextSize(1.8f);
    M5.Display.drawString("BLE Keyboard", 8, theme::CONTENT_TOP + 4);

    M5.Display.setTextDatum(top_center);
    M5.Display.setTextSize(1.5f);
    if (g_connected) {
        M5.Display.setTextColor(to565(theme::GREEN), to565(theme::IVORY));
        M5.Display.drawString("Connected", theme::SCREEN_W / 2,
                              theme::CONTENT_TOP + 30);
    } else {
        M5.Display.setTextColor(to565(theme::YELLOW), to565(theme::IVORY));
        M5.Display.drawString("Waiting for host...", theme::SCREEN_W / 2,
                              theme::CONTENT_TOP + 30);
    }

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    if (g_connected) {
        M5.Display.drawString("Opt = Cmd/Win   Fn = arrows/F-keys",
                              theme::SCREEN_W / 2, theme::CONTENT_TOP + 52);
    } else {
        M5.Display.drawString("Pair in your Bluetooth settings as",
                              theme::SCREEN_W / 2, theme::CONTENT_TOP + 52);
        M5.Display.setTextColor(to565(theme::ORANGE), to565(theme::IVORY));
        M5.Display.drawString(DEVICE_NAME, theme::SCREEN_W / 2,
                              theme::CONTENT_TOP + 64);
    }

    paint_echo();

    M5.Display.setTextColor(to565(theme::MID_GRAY), to565(theme::IVORY));
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(bottom_left);
    M5.Display.drawString("G0 or Fn+Opt  exit", 4, theme::SCREEN_H - 2);
    M5.Display.setTextDatum(top_left);
}

void leave() {
    ble_stop();
    app::goto_screen(app::Screen::LAUNCHER);
}

}  // namespace

void enter() {
    g_echo[0] = '\0';
    memset(g_last_report, 0, sizeof(g_last_report));
    g_last_key_count = 0;
    g_last_batt_ms = millis();
    // Swallow the Enter that opened this screen: don't forward anything
    // until the keyboard has been fully released once.
    g_exit_armed = false;
    ble_start();
    paint();
}

void tick() {
    if (g_needs_repaint) {
        g_needs_repaint = false;
        if (!g_connected) memset(g_last_report, 0, sizeof(g_last_report));
        paint();
    }

    if (M5.BtnA.wasPressed()) {
        leave();
        return;
    }

    auto& ks = M5Cardputer.Keyboard.keysState();
    const size_t n_keys = M5Cardputer.Keyboard.keyList().size();

    if (!g_exit_armed) {
        if (n_keys == 0) g_exit_armed = true;
        return;
    }

    // Keyboard fallback for leaving: Fn + Opt with nothing else held.
    if (ks.fn && ks.opt && ks.hid_keys.empty()) {
        leave();
        return;
    }

    uint8_t rep[8] = {0};
    uint8_t mods = 0;
    if (ks.ctrl)  mods |= MOD_LCTRL;
    if (ks.shift) mods |= MOD_LSHIFT;
    if (ks.alt)   mods |= MOD_LALT;
    if (ks.opt && !ks.fn) mods |= MOD_LGUI;
    rep[0] = mods;
    int slot = 2;
    for (uint8_t k : ks.hid_keys) {
        if (slot >= 8) break;
        k &= 0x7f;  // strip the library's SHIFT flag, if any
        if (ks.fn) k = fn_layer(k);
        if (k) rep[slot++] = k;
    }

    if (memcmp(rep, g_last_report, sizeof(rep)) != 0) {
        send_report(rep);
        memcpy(g_last_report, rep, sizeof(rep));

        // Echo newly pressed printable characters.
        if (n_keys > g_last_key_count && !ks.word.empty() && !ks.ctrl &&
            !ks.fn) {
            char c = ks.word.back();
            if (c >= 0x20 && c < 0x7f) {
                push_echo(c);
                paint_echo();
            }
        }
    }
    g_last_key_count = (uint8_t)n_keys;

    uint32_t now = millis();
    if (g_hid && now - g_last_batt_ms > 60000) {
        g_last_batt_ms = now;
        g_hid->setBatteryLevel(battery_pct());
    }
}

}  // namespace blekbd
}  // namespace apps
