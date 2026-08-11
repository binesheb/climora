/************************************************************
 * CLIMORA AUTOMATIC GITHUB RELEASE OTA
 *
 * - Check once on every boot after Wi-Fi is available
 * - Check again every 6 hours
 * - GitHub Releases is the firmware source
 * - No SHA-256 validation for now (intentionally simplified)
 * - OTA checking has a temporary progress-bar visual
 * - Firmware download/update has a breathing white-bar visual
 ************************************************************/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <FastLED.h>

#define NUM_LEDS 60
extern CRGB leds[NUM_LEDS];

namespace {

constexpr const char *OTA_API_URL =
    "https://api.github.com/repos/binesheb/climora/releases/latest";
constexpr const char *OTA_ASSET_NAME = "climora-firmware.bin";
constexpr const char *DEFAULT_VERSION = "5.5.0";

constexpr unsigned long OTA_FIRST_CHECK_DELAY_MS = 15000UL;
constexpr unsigned long OTA_CHECK_INTERVAL_MS = 21600000UL; // 6 hours
constexpr size_t OTA_JSON_CAPACITY = 8192;

enum OTAVIsualState : uint8_t {
  OTA_VISUAL_IDLE = 0,
  OTA_VISUAL_CHECKING,
  OTA_VISUAL_UPDATING
};

volatile OTAVIsualState otaVisualState = OTA_VISUAL_IDLE;
volatile int otaProgressPercent = 0;
volatile bool otaTaskRunning = false;

Preferences otaPrefs;
TaskHandle_t otaTaskHandle = nullptr;

String storedVersion() {
  otaPrefs.begin("climora", true);
  String version = otaPrefs.getString("version", DEFAULT_VERSION);
  otaPrefs.end();
  return version;
}

void storeVersion(const String &version) {
  otaPrefs.begin("climora", false);
  otaPrefs.putString("version", version);
  otaPrefs.end();
}

int compareVersions(String a, String b) {
  if (a.startsWith("v") || a.startsWith("V")) a.remove(0, 1);
  if (b.startsWith("v") || b.startsWith("V")) b.remove(0, 1);

  int ai[3] = {0, 0, 0};
  int bi[3] = {0, 0, 0};

  sscanf(a.c_str(), "%d.%d.%d", &ai[0], &ai[1], &ai[2]);
  sscanf(b.c_str(), "%d.%d.%d", &bi[0], &bi[1], &bi[2]);

  for (int i = 0; i < 3; ++i) {
    if (ai[i] < bi[i]) return -1;
    if (ai[i] > bi[i]) return 1;
  }

  return 0;
}

void otaLog(const String &message) {
  Serial.println("[OTA-AUTO] " + message);
}

bool fetchLatestRelease(String &version, String &assetUrl, size_t &assetSize) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2026-03-10");
  http.addHeader("User-Agent", "CLIMORA-AutoOTA/1.0");

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
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error) {
    otaLog("Release JSON parse failed");
    return false;
  }

  version = doc["tag_name"] | "";
  JsonArray assets = doc["assets"].as<JsonArray>();

  for (JsonObject asset : assets) {
    const char *name = asset["name"] | "";

    if (String(name) == OTA_ASSET_NAME) {
      assetUrl = asset["browser_download_url"] | "";
      assetSize = asset["size"] | 0UL;

      return version.length() > 0 &&
             assetUrl.length() > 0 &&
             assetSize > 0;
    }
  }

  otaLog("Latest release has no " + String(OTA_ASSET_NAME));
  return false;
}

bool performUpdate(const String &version,
                   const String &assetUrl,
                   size_t expectedSize) {
  otaVisualState = OTA_VISUAL_UPDATING;
  otaProgressPercent = 0;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CLIMORA-AutoOTA/1.0");

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
    otaLog("Size mismatch: expected=" + String(expectedSize) +
           " received=" + String(contentLength));
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

  WiFiClient *stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buffer[2048];
  unsigned long lastProgress = 0;

  while (http.connected() &&
         (contentLength <= 0 || written < (size_t)contentLength)) {
    size_t available = stream->available();

    if (available) {
      size_t toRead = available;
      if (toRead > sizeof(buffer)) toRead = sizeof(buffer);

      int read = stream->readBytes(buffer, toRead);
      if (read <= 0) break;

      size_t writtenNow = Update.write(buffer, (size_t)read);

      if (writtenNow != (size_t)read) {
        otaLog("Flash write failed, error=" + String(Update.getError()));
        Update.abort();
        http.end();
        otaVisualState = OTA_VISUAL_IDLE;
        return false;
      }

      written += writtenNow;

      if (contentLength > 0) {
        otaProgressPercent =
            (int)((written * 100UL) / (size_t)contentLength);
      }

      if (millis() - lastProgress >= 1000) {
        lastProgress = millis();

        otaLog("Progress=" + String(otaProgressPercent) +
               "% (" + String(written) +
               "/" + String(contentLength) + ")");
      }
    } else {
      delay(10);
    }
  }

  http.end();

  if (contentLength > 0 && written != (size_t)contentLength) {
    otaLog("Incomplete download: " + String(written) +
           "/" + String(contentLength));
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

  otaLog("Firmware " + version + " written successfully");
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
  String latest;
  String assetUrl;
  size_t assetSize = 0;

  otaLog("Current firmware=" + current);
  otaLog("Checking GitHub Releases...");

  bool found = fetchLatestRelease(latest, assetUrl, assetSize);

  if (!found) {
    otaVisualState = OTA_VISUAL_IDLE;
    return;
  }

  otaProgressPercent = 100;
  otaLog("Latest release=" + latest);

  if (compareVersions(current, latest) >= 0) {
    otaLog("Firmware is up to date");
    otaVisualState = OTA_VISUAL_IDLE;
    return;
  }

  otaLog("Update available: " + current + " -> " + latest);
  otaLog("Asset size=" + String(assetSize) + " bytes");

  performUpdate(latest, assetUrl, assetSize);
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
      leds[pos] = CRGB(brightness * 0.70f,
                       brightness * 0.82f,
                       brightness);
    }

    for (int i = 0; i < NUM_LEDS; ++i) {
      if (leds[i].getLuma() == 0) {
        leds[i] = CRGB(8, 10, 14);
      }
    }
  }
  else if (otaVisualState == OTA_VISUAL_UPDATING) {
    float breath = (sin8(phase) / 255.0f);
    breath = breath * breath;

    uint8_t centerBrightness = 40 + (uint8_t)(205.0f * breath);
    int halfWidth = 11;
    int center = (NUM_LEDS - 1) / 2;

    for (int i = 0; i < NUM_LEDS; ++i) {
      float distance = abs(i - center);
      float falloff = 1.0f - (distance / (float)(halfWidth + 1));
      if (falloff < 0.0f) falloff = 0.0f;

      uint8_t value = (uint8_t)(centerBrightness * falloff);
      leds[i] = CRGB(value, value, value);
    }

    if (otaProgressPercent > 0) {
      int filled =
          (otaProgressPercent * NUM_LEDS) / 100;

      for (int i = 0; i < filled && i < NUM_LEDS; ++i) {
        uint8_t core = max<uint8_t>(leds[i].r, 70);
        leds[i] = CRGB(core, core, core);
      }
    }
  }
}

void otaTask(void *) {
  delay(OTA_FIRST_CHECK_DELAY_MS);

  unsigned long lastCheck = 0;
  bool first = true;
  unsigned long lastVisual = 0;

  for (;;) {
    if (otaVisualState != OTA_VISUAL_IDLE &&
        millis() - lastVisual >= 25) {
      lastVisual = millis();
      renderOTAVisual();
      FastLED.show();
    }

    if (WiFi.status() == WL_CONNECTED) {
      if (first || millis() - lastCheck >= OTA_CHECK_INTERVAL_MS) {
        first = false;
        lastCheck = millis();
        otaTaskRunning = true;
        checkForUpdate();
        otaTaskRunning = false;
      }
    }

    delay(5);
  }
}

struct AutoOTAStarter {
  AutoOTAStarter() {
    xTaskCreatePinnedToCore(
        otaTask,
        "climora_ota",
        12288,
        nullptr,
        1,
        &otaTaskHandle,
        0);
  }
};

AutoOTAStarter autoOTAStarter;

} // namespace
