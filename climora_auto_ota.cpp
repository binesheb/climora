#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <FastLED.h>
#include <mbedtls/sha256.h>

#define NUM_LEDS 60
extern CRGB leds[NUM_LEDS];

namespace {
constexpr const char* OTA_API_URL = "https://api.github.com/repos/binesheb/climora/releases/latest";
constexpr const char* OTA_ASSET_NAME = "climora-firmware.bin";
constexpr const char* OTA_CHECKSUM_ASSET_NAME = "climora-firmware.bin.sha256";
constexpr const char* DEFAULT_VERSION = "5.5.0";
constexpr unsigned long OTA_FIRST_CHECK_DELAY_MS = 15000UL;
constexpr unsigned long OTA_CHECK_INTERVAL_MS = 21600000UL;
constexpr size_t OTA_JSON_CAPACITY = 8192;

// GitHub currently serves releases through DigiCert's public roots. Keep TLS
// verification enabled and fail closed if the pinned public roots are not accepted.
constexpr const char* OTA_ROOT_CA =
"-----BEGIN CERTIFICATE-----\n"
"MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
"MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
"b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
"9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
"2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
"1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
"q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
"tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
"vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
"BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
"5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
"1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
"NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
"Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
"8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
"pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
"MrY=\n"
"-----END CERTIFICATE-----\n";

enum OTAVisualState : uint8_t { OTA_VISUAL_IDLE, OTA_VISUAL_CHECKING, OTA_VISUAL_UPDATING };
volatile OTAVisualState otaVisualState = OTA_VISUAL_IDLE;
volatile int otaProgressPercent = 0;
Preferences otaPrefs;
TaskHandle_t otaTaskHandle = nullptr;

String storedVersion() {
  otaPrefs.begin("climora", true);
  String v = otaPrefs.getString("version", DEFAULT_VERSION);
  otaPrefs.end();
  return v;
}

void storeVersion(const String& v) {
  otaPrefs.begin("climora", false);
  otaPrefs.putString("version", v);
  otaPrefs.end();
}

int compareVersions(String a, String b) {
  if (a.startsWith("v") || a.startsWith("V")) a.remove(0, 1);
  if (b.startsWith("v") || b.startsWith("V")) b.remove(0, 1);
  int ai[3] = {0,0,0}, bi[3] = {0,0,0};
  sscanf(a.c_str(), "%d.%d.%d", &ai[0], &ai[1], &ai[2]);
  sscanf(b.c_str(), "%d.%d.%d", &bi[0], &bi[1], &bi[2]);
  for (int i = 0; i < 3; ++i) {
    if (ai[i] < bi[i]) return -1;
    if (ai[i] > bi[i]) return 1;
  }
  return 0;
}

void otaLog(const String& message) {
  Serial.println("[OTA-AUTO] " + message);
}

bool configureTLS(WiFiClientSecure& client) {
  client.setCACert(OTA_ROOT_CA);
  client.setTimeout(15000);
  return true;
}

bool fetchLatestRelease(String& version, String& assetUrl, size_t& assetSize, String& checksumUrl) {
  WiFiClientSecure client;
  configureTLS(client);
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2026-03-10");
  http.addHeader("User-Agent", "CLIMORA-AutoOTA/1.1");

  if (!http.begin(client, OTA_API_URL)) {
    otaLog("Unable to start GitHub API request");
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    otaLog("GitHub API HTTP=" + String(code));
    http.end();
    return false;
  }

  DynamicJsonDocument doc(OTA_JSON_CAPACITY);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    otaLog("Release JSON parse failed");
    return false;
  }

  version = doc["tag_name"] | "";
  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject asset : assets) {
    const char* name = asset["name"] | "";
    const char* url = asset["browser_download_url"] | "";
    size_t size = asset["size"] | 0UL;
    if (String(name) == OTA_ASSET_NAME) {
      assetUrl = url;
      assetSize = size;
    } else if (String(name) == OTA_CHECKSUM_ASSET_NAME) {
      checksumUrl = url;
    }
  }
  if (!version.length() || !assetUrl.length() || assetSize == 0 || !checksumUrl.length()) {
    otaLog("Release is missing firmware or checksum asset");
    return false;
  }
  return true;
}

bool fetchExpectedSha256(const String& checksumUrl, String& expected) {
  WiFiClientSecure client;
  configureTLS(client);
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CLIMORA-AutoOTA/1.1");
  if (!http.begin(client, checksumUrl)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    otaLog("Checksum download HTTP=" + String(code));
    http.end();
    return false;
  }
  expected = http.getString();
  http.end();
  expected.trim();
  int space = expected.indexOf(' ');
  if (space > 0) expected = expected.substring(0, space);
  expected.toLowerCase();
  if (expected.length() != 64) return false;
  for (size_t i = 0; i < expected.length(); ++i) {
    char c = expected[i];
    if (!isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

String sha256Hex(const uint8_t* digest, size_t length) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out += hex[(digest[i] >> 4) & 0x0F];
    out += hex[digest[i] & 0x0F];
  }
  return out;
}

bool performUpdate(const String& version, const String& assetUrl, size_t expectedSize, const String& expectedSha256) {
  otaVisualState = OTA_VISUAL_UPDATING;
  otaProgressPercent = 0;

  WiFiClientSecure client;
  configureTLS(client);
  HTTPClient http;
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CLIMORA-AutoOTA/1.1");

  otaLog("Downloading firmware " + version);
  if (!http.begin(client, assetUrl)) {
    otaLog("Unable to start firmware download");
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    otaLog("Firmware download HTTP=" + String(code));
    http.end();
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) contentLength = (int)expectedSize;
  if (expectedSize > 0 && (size_t)contentLength != expectedSize) {
    otaLog("Size mismatch: expected=" + String(expectedSize) + " received=" + String(contentLength));
    http.end();
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }
  if (!Update.begin((size_t)contentLength)) {
    otaLog("Update.begin failed, error=" + String(Update.getError()));
    http.end();
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  if (mbedtls_sha256_starts_ret(&sha, 0) != 0) {
    mbedtls_sha256_free(&sha);
    Update.abort();
    http.end();
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buffer[2048];
  unsigned long lastProgress = 0;

  while (http.connected() && (contentLength <= 0 || written < (size_t)contentLength)) {
    size_t available = stream->available();
    if (available) {
      size_t toRead = min(available, sizeof(buffer));
      int read = stream->readBytes(buffer, toRead);
      if (read <= 0) break;
      if (mbedtls_sha256_update_ret(&sha, buffer, (size_t)read) != 0) {
        mbedtls_sha256_free(&sha);
        Update.abort();
        http.end();
        otaVisualState = OTA_VISUAL_IDLE;
        return false;
      }
      size_t w = Update.write(buffer, (size_t)read);
      if (w != (size_t)read) {
        otaLog("Flash write failed, error=" + String(Update.getError()));
        mbedtls_sha256_free(&sha);
        Update.abort();
        http.end();
        otaVisualState = OTA_VISUAL_IDLE;
        return false;
      }
      written += w;
      if (contentLength > 0) otaProgressPercent = (int)((written * 100UL) / (size_t)contentLength);
      if (millis() - lastProgress >= 1000) {
        lastProgress = millis();
        otaLog("Progress=" + String(otaProgressPercent) + "% (" + String(written) + "/" + String(contentLength) + ")");
      }
    } else {
      delay(10);
    }
  }

  uint8_t digest[32];
  bool digestOk = mbedtls_sha256_finish_ret(&sha, digest) == 0;
  mbedtls_sha256_free(&sha);
  http.end();

  if (contentLength > 0 && written != (size_t)contentLength) {
    otaLog("Incomplete download: " + String(written) + "/" + String(contentLength));
    Update.abort();
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }
  if (!digestOk) {
    otaLog("SHA-256 calculation failed");
    Update.abort();
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }

  String actualSha256 = sha256Hex(digest, sizeof(digest));
  if (!actualSha256.equalsIgnoreCase(expectedSha256)) {
    otaLog("SHA-256 mismatch: expected=" + expectedSha256 + " actual=" + actualSha256);
    Update.abort();
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }

  if (!Update.end(true)) {
    otaLog("Update.end failed, error=" + String(Update.getError()));
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }
  if (!Update.isFinished()) {
    otaLog("Update did not finish");
    otaVisualState = OTA_VISUAL_IDLE;
    return false;
  }

  otaProgressPercent = 100;
  storeVersion(version);
  otaLog("Firmware " + version + " written and verified successfully");
  otaLog("Rebooting into new firmware");
  delay(500);
  ESP.restart();
  return true;
}

void checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    otaLog("WiFi unavailable; skipping check");
    otaVisualState = OTA_VISUAL_IDLE;
    return;
  }

  otaVisualState = OTA_VISUAL_CHECKING;
  otaProgressPercent = 0;

  String current = storedVersion();
  String latest, assetUrl, checksumUrl;
  size_t assetSize = 0;
  otaLog("Current firmware=" + current);
  otaLog("Checking GitHub Releases...");

  if (!fetchLatestRelease(latest, assetUrl, assetSize, checksumUrl)) {
    otaVisualState = OTA_VISUAL_IDLE;
    return;
  }

  otaLog("Latest release=" + latest);
  if (compareVersions(current, latest) >= 0) {
    otaLog("Firmware is up to date");
    otaVisualState = OTA_VISUAL_IDLE;
    return;
  }

  String expectedSha256;
  if (!fetchExpectedSha256(checksumUrl, expectedSha256)) {
    otaLog("Unable to obtain a valid firmware SHA-256; refusing update");
    otaVisualState = OTA_VISUAL_IDLE;
    return;
  }

  otaLog("Update available: " + current + " -> " + latest);
  otaLog("Asset size=" + String(assetSize) + " bytes");
  otaLog("Expected SHA-256=" + expectedSha256);
  performUpdate(latest, assetUrl, assetSize, expectedSha256);
}

void renderOTAVisual() {
  static uint16_t phase = 0;
  phase++;
  fill_solid(leds, NUM_LEDS, CRGB::Black);

  if (otaVisualState == OTA_VISUAL_CHECKING) {
    const int barWidth = 10;
    int center = (phase / 2) % NUM_LEDS;
    for (int offset = -barWidth / 2; offset <= barWidth / 2; ++offset) {
      int pos = center + offset;
      if (pos < 0) pos += NUM_LEDS;
      if (pos >= NUM_LEDS) pos -= NUM_LEDS;
      int distance = abs(offset);
      uint8_t brightness = 220 - distance * 28;
      leds[pos] = CRGB((uint8_t)(brightness * 0.70f),
                       (uint8_t)(brightness * 0.82f), brightness);
    }
    for (int i = 0; i < NUM_LEDS; ++i) {
      if (leds[i].getLuma() == 0) leds[i] = CRGB(8, 10, 14);
    }
  }
  else if (otaVisualState == OTA_VISUAL_UPDATING) {
    float breath = sin8(phase) / 255.0f;
    breath *= breath;
    uint8_t centerBrightness = 40 + (uint8_t)(205.0f * breath);
    int halfWidth = 11;
    int center = (NUM_LEDS - 1) / 2;

    for (int i = 0; i < NUM_LEDS; ++i) {
      float distance = abs(i - center);
      float falloff = 1.0f - distance / (float)(halfWidth + 1);
      if (falloff < 0.0f) falloff = 0.0f;
      uint8_t value = (uint8_t)(centerBrightness * falloff);
      leds[i] = CRGB(value, value, value);
    }

    int filled = (otaProgressPercent * NUM_LEDS) / 100;
    for (int i = 0; i < filled && i < NUM_LEDS; ++i) {
      uint8_t core = max<uint8_t>(leds[i].r, 70);
      leds[i] = CRGB(core, core, core);
    }
  }
}

void otaUpdateTask(void*) {
  delay(OTA_FIRST_CHECK_DELAY_MS);
  unsigned long lastCheck = 0;
  bool first = true;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && (first || millis() - lastCheck >= OTA_CHECK_INTERVAL_MS)) {
      first = false;
      lastCheck = millis();
      checkForUpdate();
    }
    delay(1000);
  }
}

void otaVisualTask(void*) {
  for (;;) {
    if (otaVisualState != OTA_VISUAL_IDLE) {
      renderOTAVisual();
      FastLED.show();
    }
    delay(25);
  }
}

struct AutoOTAStarter {
  AutoOTAStarter() {
    xTaskCreatePinnedToCore(otaUpdateTask, "climora_ota", 12288, nullptr, 1, &otaTaskHandle, 0);
    xTaskCreatePinnedToCore(otaVisualTask, "climora_ota_led", 4096, nullptr, 1, nullptr, 1);
  }
};

AutoOTAStarter autoOTAStarter;
}
