// Entry point — orchestrates settings, display, UI, WiFi, web server and the
// two data pollers (flight radar + weather).
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <time.h>
#include <stdlib.h>
#include <WiFiManager.h>
#include <XPT2046_Touchscreen.h>

#include "board_config.h"
#include "settings.h"
#include "display.h"
#include "ui.h"
#include "web_server.h"
#include "radar.h"
#include "weather.h"
#include "homeassistant.h"
#include "net_lock.h"

namespace {

constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
constexpr int TOUCH_MIN_Z = 120;
constexpr int TOUCH_RAW_MIN_X = 180;
constexpr int TOUCH_RAW_MAX_X = 3920;
constexpr int TOUCH_RAW_MIN_Y = 180;
constexpr int TOUCH_RAW_MAX_Y = 3920;
constexpr bool TOUCH_SWAP_XY = true;
constexpr bool TOUCH_INVERT_X = false;
constexpr bool TOUCH_INVERT_Y = true;

SPIClass touch_spi(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS);

void init_touch() {
    static bool initialized = false;
    if (initialized) return;
    touch_spi.begin(board::TOUCH_SCLK, board::TOUCH_MISO, board::TOUCH_MOSI, board::TOUCH_CS_PIN);
    touch.begin(touch_spi);
    touch.setRotation(0);
    initialized = true;
}

bool is_touch_pressed() {
    if (touch.touched()) return true;
    TS_Point p = touch.getPoint();
    return p.z >= TOUCH_MIN_Z;
}

bool read_touch_xy(int& sx, int& sy) {
    TS_Point p = touch.getPoint();
    if (p.z < TOUCH_MIN_Z) return false;

    int tx = p.x;
    int ty = p.y;
    if (TOUCH_SWAP_XY) {
        int t = tx;
        tx = ty;
        ty = t;
    }

    tx = constrain(tx, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X);
    ty = constrain(ty, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y);
    sx = map(tx, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X, 0, board::LCD_WIDTH - 1);
    sy = map(ty, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y, 0, board::LCD_HEIGHT - 1);
    if (TOUCH_INVERT_X) sx = (board::LCD_WIDTH - 1) - sx;
    if (TOUCH_INVERT_Y) sy = (board::LCD_HEIGHT - 1) - sy;
    return true;
}

bool should_enter_touch_setup() {
    if (strlen(settings::state().wifi_ssid) == 0) return true;

    init_touch();
    ui::update_status("Hold touch to setup", "(2 seconds)");
    uint32_t hold_start = 0;
    const uint32_t window_start = millis();

    while (millis() - window_start < 4500) {
        display::tick();
        if (is_touch_pressed()) {
            if (hold_start == 0) hold_start = millis();
            if (millis() - hold_start >= 2000) return true;
        } else {
            hold_start = 0;
        }
        delay(20);
    }
    return false;
}

void run_touch_setup_portal() {
    WiFiManager wm;
    wm.setDebugOutput(true);
    wm.setConfigPortalTimeout(180);
    wm.setBreakAfterConfig(true);

    char host_buf[33];
    strlcpy(host_buf, settings::state().hostname, sizeof(host_buf));
    WiFiManagerParameter host_param("hostname", "Hostname", host_buf, sizeof(host_buf) - 1);
    wm.addParameter(&host_param);

    char ap_name[24];
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(ap_name, sizeof(ap_name), "SkyGauge-%02X%02X", mac[4], mac[5]);

    ui::show_status();
    ui::update_status("Setup AP running", ap_name);
    display::tick();
    bool ok = wm.startConfigPortal(ap_name);

    if (ok && WiFi.status() == WL_CONNECTED) {
        auto& s = settings::state();
        strlcpy(s.wifi_ssid, WiFi.SSID().c_str(), sizeof(s.wifi_ssid));
        strlcpy(s.wifi_password, WiFi.psk().c_str(), sizeof(s.wifi_password));
        strlcpy(s.hostname, host_param.getValue(), sizeof(s.hostname));
        settings::save();
        ui::update_status("Saved WiFi", WiFi.SSID().c_str());
        log_i("WiFi setup saved for SSID '%s'", s.wifi_ssid);
    } else {
        ui::update_status("Setup not completed", "Using fallback network mode");
        log_w("WiFi setup portal ended without active connection");
    }
}

// AP fallback identity ───────────────────────────────────────────────────────
String ap_ssid() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[24];
    snprintf(buf, sizeof(buf), "ESP-Gauge-%02X%02X", mac[4], mac[5]);
    return String(buf);
}

bool try_sta(const char* ssid, const char* pwd) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(settings::state().hostname);
    // Disable WiFi modem-sleep (power save). It's on by default and is a
    // notorious cause of "associated but unresponsive / dropping connections"
    // flakiness — which this unit suffered repeatedly. This is a mains/USB
    // powered desk device, so the extra draw is irrelevant.
    WiFi.setSleep(false);
    WiFi.begin(ssid, pwd);
    log_i("WiFi STA → SSID='%s'", ssid);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);
        return false;
    }
    log_i("WiFi connected, IP = %s", WiFi.localIP().toString().c_str());
    return true;
}

void start_ap_mode() {
    WiFi.mode(WIFI_AP);
    String ssid = ap_ssid();
    WiFi.softAP(ssid.c_str(), nullptr);
    log_i("AP mode '%s' on %s", ssid.c_str(), WiFi.softAPIP().toString().c_str());
    log_i("Open setup at http://%s", WiFi.softAPIP().toString().c_str());
    char l1[40];
    snprintf(l1, sizeof(l1), "AP %s", ssid.c_str());
    ui::update_status(l1, WiFi.softAPIP().toString().c_str());
}

void connect_or_ap() {
    const auto& s = settings::state();
    bool connected = false;
    if (strlen(s.wifi_ssid) > 0) {
        ui::update_status("Connecting WiFi...", s.wifi_ssid);
        connected = try_sta(s.wifi_ssid, s.wifi_password);
    }
    if (!connected) {
        start_ap_mode();
    } else {
        ui::update_status("WiFi connected", WiFi.localIP().toString().c_str());
        log_i("Open setup at http://%s", WiFi.localIP().toString().c_str());
    }

    // mDNS works in either mode — but skip if AP, just in case.
    if (connected) {
        if (MDNS.begin(s.hostname)) {
            MDNS.addService("http", "tcp", 80);
            log_i("mDNS as %s.local", s.hostname);
            log_i("Try http://%s.local", s.hostname);
        } else {
            log_w("mDNS start failed; use IP URL instead");
        }
    }

    if (connected) {
        // Start SNTP with the selected POSIX TZ so localtime() tracks
        // timezone and DST rules correctly.
        configTzTime(settings::state().timezone, "pool.ntp.org", "time.nist.gov");
    }
}

// OTA firmware + LittleFS updates over WiFi (no USB cable needed):
//   pio run -t upload   --upload-protocol espota --upload-port esp-gauge.local
//   pio run -t uploadfs --upload-protocol espota --upload-port esp-gauge.local
// The net lock is held for the whole transfer so the pollers' TLS traffic
// can't compete with the update for heap/bandwidth.
bool ota_holds_netlock = false;

void setup_ota() {
    ArduinoOTA.setHostname(settings::state().hostname);
    ArduinoOTA.onStart([]() {
        ota_holds_netlock =
            xSemaphoreTake(netlock::handle(), pdMS_TO_TICKS(15000)) == pdTRUE;
        ui::show_status();
        ui::update_status("OTA update", "starting...");
        display::tick();
        log_i("OTA started (%s)",
              ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static uint8_t last_pct = 255;
        uint8_t pct = done * 100 / total;
        if (pct == last_pct) return;
        last_pct = pct;
        char buf[12];
        snprintf(buf, sizeof(buf), "%u%%", pct);
        ui::update_status("OTA update", buf);
        display::tick();
    });
    ArduinoOTA.onEnd([]() {
        ui::update_status("OTA done", "rebooting...");
        display::tick();
        if (ota_holds_netlock) xSemaphoreGive(netlock::handle());
    });
    ArduinoOTA.onError([](ota_error_t err) {
        log_e("OTA error %u", err);
        if (ota_holds_netlock) xSemaphoreGive(netlock::handle());
        ota_holds_netlock = false;
        ui::update_status("OTA failed", "");
        display::tick();
    });
    ArduinoOTA.begin();
    log_i("OTA ready on port 3232");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(150);
    log_i("Booting Sky Gauge (ESP32 + ILI9341)");

    settings::begin();
    display::begin();

    ui::begin();

    // Show the status screen during boot so the user can read network messages,
    // then switch to their saved mode once the network is up.
    ui::show_status();
    ui::update_status("Booting...", "");
    display::tick();

    init_touch();
    if (should_enter_touch_setup()) {
        run_touch_setup_portal();
    }

    connect_or_ap();
    web::begin();
    setup_ota();
    radar::begin();     // pollers idle until their mode is active
    weather::begin();
    homeassistant::begin();

    // Let the user see the IP for a moment before switching to their mode.
    uint32_t t0 = millis();
    while (millis() - t0 < 2500) display::tick();

    const uint8_t boot_brightness = settings::state().brightness < 16
                                        ? 128
                                        : settings::state().brightness;
    display::set_brightness(boot_brightness);
    ui::set_mode(settings::state().mode);
}

void loop() {
    display::tick();
    web::loop_tick();
    ArduinoOTA.handle();

    static bool touch_down = false;
    static uint32_t touch_down_ms = 0;
    static int touch_x = 0;
    static int touch_y = 0;
    int sx = 0;
    int sy = 0;
    bool pressed = read_touch_xy(sx, sy);
    if (pressed && !touch_down) {
        touch_down = true;
        touch_down_ms = millis();
        touch_x = sx;
        touch_y = sy;
    } else if (!pressed && touch_down) {
        // Treat short press-release as a tap for selecting aircraft blips.
        if (millis() - touch_down_ms <= 700) {
            ui::on_touch_tap(touch_x, touch_y);
        }
        touch_down = false;
    }

    // STA association can drop and stay down silently (observed in the wild:
    // device alive, WiFi gone, nothing reconnecting). Nudge it every 30 s.
    static uint32_t last_wifi_ok_ms = 0;
    uint32_t now = millis();
    if (WiFi.getMode() == WIFI_STA) {
        if (WiFi.status() == WL_CONNECTED) {
            last_wifi_ok_ms = now;
        } else if (now - last_wifi_ok_ms > 30000) {
            last_wifi_ok_ms = now;   // rate-limit attempts
            log_w("WiFi down >30 s — reconnecting");
            WiFi.reconnect();
        }
    }
    // Yield briefly instead of busy-spinning core 1 — LVGL timers are 33 ms+
    // granular, so a 1 ms sleep costs nothing and lets the idle task run.
    delay(1);
}
