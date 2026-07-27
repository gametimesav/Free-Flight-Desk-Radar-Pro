#include "net_fetch.h"
#include "net_lock.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace net {

bool http_get_json(const String& url,
                   JsonDocument& doc,
                   JsonDocument* filter,
                   const char* bearer) {
  netlock::Guard guard;
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) {
    return false;
  }

  if (bearer && bearer[0]) {
    String auth = "Bearer ";
    auth += bearer;
    http.addHeader("Authorization", auth);
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  DeserializationError err;
  if (filter) {
    err = deserializeJson(doc, *stream, DeserializationOption::Filter(*filter));
  } else {
    err = deserializeJson(doc, *stream);
  }

  http.end();
  return !err;
}

bool http_post_form_json(const String& url,
                         const String& formBody,
                         JsonDocument& doc) {
  netlock::Guard guard;
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) {
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST(formBody);
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  return !err;
}

}  // namespace net
