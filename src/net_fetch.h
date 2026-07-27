#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace net {

bool http_get_json(const String& url,
                   JsonDocument& doc,
                   JsonDocument* filter = nullptr,
                   const char* bearer = nullptr);

bool http_post_form_json(const String& url,
                         const String& formBody,
                         JsonDocument& doc);

}  // namespace net
