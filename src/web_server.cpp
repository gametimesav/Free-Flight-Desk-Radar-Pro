#include "web_server.h"
#include "settings.h"
#include "ui.h"
#include "display.h"
#include "radar.h"
#include "screenshot.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <time.h>
#include <stdlib.h>
#include <new>

namespace web {
namespace {

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dns;
bool dns_active = false;
bool ap_mode = false;

constexpr uint16_t DNS_PORT = 53;

bool serve_littlefs_file(AsyncWebServerRequest* req, const char* path,
                         const char* content_type) {
    String normalized = path ? String(path) : "";
    if (normalized.length() == 0 || normalized[0] != '/') {
        normalized = "/" + normalized;
    }
    const char* fs_path = normalized.c_str();

    if (!LittleFS.exists(fs_path)) {
        log_w("[http] missing asset %s", fs_path);
        req->send(404, "text/plain", "Missing asset");
        return false;
    }

    log_i("[http] GET %s -> %s", req->url().c_str(), fs_path);
    AsyncWebServerResponse* res = req->beginResponse(LittleFS, fs_path, content_type);
    if (!res) {
        log_e("[http] failed to build response for %s", fs_path);
        req->send(500, "text/plain", "Unable to read asset");
        return false;
    }
    res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    req->send(res);
    return true;
}

const char* kFallbackIndexHtml =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Sky Gauge Setup</title>"
    "<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
    "margin:24px;line-height:1.4}code{background:#f3f4f6;padding:2px 6px;"
    "border-radius:6px}a{color:#0b63ce}</style></head><body>"
    "<h2>Sky Gauge is online</h2>"
    "<p>If <code>esp-gauge.local</code> does not resolve, open this device by IP.</p>"
    "<p>Try <a href='/api/state'>/api/state</a> to verify API connectivity.</p>"
    "<p>Tip: upload LittleFS web assets to get the full settings UI.</p>"
    "</body></html>";

// NVS writes are debounced: a brightness-slider drag arrives as dozens of
// config patches per second, and each save() walks every key. Settings apply
// live immediately; the flash write happens after the burst settles.
bool     save_pending = false;
uint32_t save_request_ms = 0;
constexpr uint32_t SAVE_DEBOUNCE_MS = 1500;

void flush_save() {
    if (save_pending) {
        save_pending = false;
        settings::save();
    }
}

// LVGL is single-threaded and lives on core 1 (main loop). Config patches
// arrive on the AsyncTCP task, so touching ui::* from the handlers races the
// radar screen's 33 ms lv_timer — screen loads get clobbered and mode
// switches silently fail. Handlers only set this flag; loop_tick() (same
// thread as lv_timer_handler) applies the UI change.
volatile bool ui_refresh_pending = false;

bool safe_ws_text_all(const String& out, const char* tag) {
    // Avoid crashes from AsyncWebSocket internal allocations when heap is
    // tight (seen as std::bad_alloc -> terminate).
    if (ESP.getFreeHeap() < 45000) {
        log_w("[ws] drop %s frame (low heap: %u)", tag, (unsigned)ESP.getFreeHeap());
        return false;
    }
    try {
        ws.textAll(out);
        return true;
    } catch (const std::bad_alloc&) {
        log_e("[ws] OOM broadcasting %s (%u bytes)", tag, (unsigned)out.length());
        return false;
    } catch (...) {
        log_e("[ws] exception broadcasting %s", tag);
        return false;
    }
}

void send_state_to(AsyncWebSocketClient* client) {
    JsonDocument doc;
    doc["type"] = "state";
    JsonObject data = doc["data"].to<JsonObject>();
    settings::to_json(data, false);

    String out;
    serializeJson(doc, out);
    try {
        client->text(out);
    } catch (const std::bad_alloc&) {
        log_e("[ws] OOM sending state to client #%u", client->id());
    } catch (...) {
        log_e("[ws] exception sending state to client #%u", client->id());
    }
}

void broadcast_all_state_inline() {
    JsonDocument doc;
    doc["type"] = "state";
    JsonObject data = doc["data"].to<JsonObject>();
    settings::to_json(data, false);

    String out;
    serializeJson(doc, out);
    safe_ws_text_all(out, "state");
}

// Apply a config patch. Brightness-only patches (slider drags) skip the
// screen rebuild — they'd reset animation state 30×/second for no visual
// difference, since brightness is pure backlight PWM.
void apply_config_patch(JsonVariantConst patch) {
    char old_tz[sizeof(settings::state().timezone)];
    strlcpy(old_tz, settings::state().timezone, sizeof(old_tz));

    const bool changed = settings::apply_json(patch);
    if (!changed) {
        log_i("[cfg] patch applied with no state changes");
        return;
    }

    const JsonVariantConst radar = patch["radar"];
    if (!radar.isNull()) {
        const float lat = radar["lat"].as<float>();
        const float lon = radar["lon"].as<float>();
        log_i("[cfg] radar patch lat=%.4f lon=%.4f", lat, lon);
    }

    save_pending = true;
    save_request_ms = millis();
    display::set_brightness(settings::state().brightness);

    // Persist location changes immediately so the page can be reloaded
    // without losing the new coordinates while the debounced save window
    // is still pending.
    if (!radar.isNull()) {
        settings::save();
    }

    // Apply timezone changes immediately so the on-screen clock updates
    // without waiting for a reboot.
    if (strcmp(old_tz, settings::state().timezone) != 0) {
        // Persist TZ immediately: users often change it and then reboot soon
        // after; waiting for the generic debounce risks losing the update.
        save_pending = false;
        if (!settings::save_timezone_only()) {
            log_e("[time] failed to persist timezone");
        }
        setenv("TZ", settings::state().timezone, 1);
        tzset();
        if (WiFi.status() == WL_CONNECTED) {
            configTzTime(settings::state().timezone,
                         "pool.ntp.org", "time.nist.gov");
        }
        log_i("[time] timezone set to %s", settings::state().timezone);
    }

    JsonObjectConst o = patch.as<JsonObjectConst>();
    bool only_brightness = o.size() == 1 && !o["brightness"].isNull();
    if (!only_brightness) {
        ui_refresh_pending = true;   // applied on the LVGL thread in loop_tick()
    }
    broadcast_all_state_inline();
}

void handle_ws_message(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        log_w("[ws] bad JSON: %s", err.c_str());
        return;
    }

    const char* type = doc["type"] | "";

    if (strcmp(type, "config") == 0) {
        apply_config_patch(doc["patch"]);
        return;
    }

    if (strcmp(type, "hello") == 0) {
        send_state_to(client);
        return;
    }

    if (strcmp(type, "command") == 0) {
        const char* cmd = doc["cmd"] | "";
        if (strcmp(cmd, "reboot") == 0) {
            log_i("[ws] reboot requested");
            flush_save();   // don't lose a debounced config write
            delay(100);
            ESP.restart();
        } else if (strcmp(cmd, "factory_reset") == 0) {
            settings::reset_to_defaults();
            delay(100);
            ESP.restart();
        }
        return;
    }
}

void on_ws_event(AsyncWebSocket*, AsyncWebSocketClient* client, AwsEventType type,
                 void* arg, uint8_t* data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            log_i("[ws] client #%u connected from %s", client->id(),
                  client->remoteIP().toString().c_str());
            send_state_to(client);
            break;
        case WS_EVT_DISCONNECT:
            log_i("[ws] client #%u disconnected", client->id());
            break;
        case WS_EVT_DATA: {
            auto* info = static_cast<AwsFrameInfo*>(arg);
            if (info->final && info->index == 0 && info->len == len &&
                info->opcode == WS_TEXT) {
                handle_ws_message(client, data, len);
            }
            break;
        }
        default:
            break;
    }
}

void register_routes() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        log_i("[http] GET %s", req->url().c_str());
        if (LittleFS.exists("/index.html")) {
            serve_littlefs_file(req, "/index.html", "text/html; charset=utf-8");
            return;
        }
        req->send(200, "text/html; charset=utf-8", kFallbackIndexHtml);
    });

    server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        serve_littlefs_file(req, "/index.html", "text/html; charset=utf-8");
    });

    server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* req) {
        serve_littlefs_file(req, "/app.js", "application/javascript; charset=utf-8");
    });

    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* req) {
        serve_littlefs_file(req, "/style.css", "text/css; charset=utf-8");
    });

    // REST endpoints for the web UI (HTTP fallback to WS-driven flow).
    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        settings::to_json(root, false);
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    auto* patchHandler = new AsyncCallbackJsonWebHandler(
        "/api/state",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            apply_config_patch(json);
            JsonDocument doc;
            JsonObject root = doc.to<JsonObject>();
            settings::to_json(root, false);
            String out;
            serializeJson(doc, out);
            req->send(200, "application/json", out);
        });
    server.addHandler(patchHandler);

    // Screen capture: POST /api/shot requests one (taken on the LVGL thread
    // a few ms later); GET /shot.bmp serves the most recent capture.
    server.on("/api/shot", HTTP_POST, [](AsyncWebServerRequest* req) {
        bool ok = shot::request();
        req->send(ok ? 200 : 507, "application/json",
                  ok ? "{\"ok\":true}" : "{\"ok\":false}");
    });

    server.on("/shot.bmp", HTTP_GET, [](AsyncWebServerRequest* req) {
        size_t len = 0;
        const uint8_t* data = shot::bmp(len);
        if (!data) {
            req->send(404, "text/plain",
                      "No capture yet - POST /api/shot first, then retry.");
            return;
        }
        AsyncWebServerResponse* res =
            req->beginResponse(200, "image/bmp", data, len);
        res->addHeader("Cache-Control", "no-store");
        req->send(res);
    });

    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", "{\"ok\":true}");
        flush_save();
        delay(100);
        ESP.restart();
    });

    server.on("/api/factory_reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        settings::reset_to_defaults();
        req->send(200, "application/json", "{\"ok\":true}");
        delay(100);
        ESP.restart();
    });

    server.onNotFound([](AsyncWebServerRequest* req) {
        if (ap_mode) {
            req->redirect("/");
            return;
        }
        if (req->method() == HTTP_OPTIONS) { req->send(200); return; }
        req->send(404, "text/plain", "Not found");
    });
}

}  // namespace

void begin() {
    if (!LittleFS.begin(true)) {
        log_e("LittleFS mount failed");
    }

    if (!LittleFS.exists("/index.html")) {
        log_w("No /index.html in LittleFS; serving fallback setup page");
    }

    const wifi_mode_t mode = WiFi.getMode();
    ap_mode = (mode == WIFI_AP || mode == WIFI_AP_STA);

    ws.onEvent(on_ws_event);
    server.addHandler(&ws);
    register_routes();

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    server.begin();
    log_i("HTTP server started on :80");

    if (ap_mode) {
        dns.setErrorReplyCode(DNSReplyCode::NoError);
        dns.start(DNS_PORT, "*", WiFi.softAPIP());
        dns_active = true;
        log_i("Captive DNS active on %s", WiFi.softAPIP().toString().c_str());
    }
}

void loop_tick() {
    if (dns_active) {
        dns.processNextRequest();
    }

    ws.cleanupClients();

    if (ui_refresh_pending) {
        ui_refresh_pending = false;
        ui::apply_settings();   // rebuilds + loads the screen for the new mode
    }

    shot::loop_tick();          // pending screen captures (LVGL thread)

    if (save_pending && millis() - save_request_ms >= SAVE_DEBOUNCE_MS) {
        flush_save();
    }

    // In Radar/Auto mode, push the aircraft list to web clients every 2 s so
    // the browser can render its own scope (it dead-reckons between pushes).
    static uint32_t last_radar_ms = 0;
    bool radar_active = settings::state().mode == settings::Mode::Radar ||
                        settings::state().mode == settings::Mode::Auto;
    if (radar_active && ws.count() > 0) {
        uint32_t now = millis();
        if (now - last_radar_ms >= 2000) {
            last_radar_ms = now;
            radar::Aircraft ac[radar::MAX_AIRCRAFT];
            size_t n = radar::get_aircraft(ac, radar::MAX_AIRCRAFT);

            JsonDocument doc;
            doc["type"]  = "radar";
            doc["age"]   = radar::data_age_ms();
            doc["range"] = settings::state().radar.range_km;
            JsonArray arr = doc["ac"].to<JsonArray>();
            for (size_t i = 0; i < n; i++) {
                JsonObject o = arr.add<JsonObject>();
                o["cs"]  = ac[i].callsign;
                o["rt"]  = ac[i].route;
                o["x"]   = ac[i].x_km;
                o["y"]   = ac[i].y_km;
                o["alt"] = ac[i].alt_ft;
                o["gs"]  = ac[i].gs_kt;
                o["trk"] = ac[i].track_deg;
                o["vr"]  = ac[i].baro_rate;
                o["gnd"] = ac[i].on_ground;
                o["emg"] = ac[i].emergency;
            }
            String out;
            serializeJson(doc, out);
            safe_ws_text_all(out, "radar");
        }
    }
}

void broadcast_state() { broadcast_all_state_inline(); }

}  // namespace web
