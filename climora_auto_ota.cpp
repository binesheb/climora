#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>

namespace {
constexpr const char* OTA_API_URL = "https://api.github.com/repos/binesheb/climora/releases/latest";
constexpr const char* OTA_ASSET_NAME = "climora-firmware.bin";
constexpr const char* DEFAULT_VERSION = "5.4.0";
constexpr unsigned long OTA_FIRST_CHECK_DELAY_MS = 15000UL;
constexpr unsigned long OTA_CHECK_INTERVAL_MS = 21600000UL;
constexpr size_t OTA_JSON_CAPACITY = 8192;

Preferences prefs;
TaskHandle_t otaTaskHandle = nullptr;

String storedVersion() {
  prefs.begin("climora", true);
  String v = prefs.getString("version", DEFAULT_VERSION);
  prefs.end();
  return v;
}

void storeVersion(const String& version) {
  prefs.begin("climora", false);
  prefs.putString("version", version);
  prefs.end();
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

void otaLog(const String& message) {
  Serial.println("[OTA-AUTO] " + message);
}

bool fetchLatestRelease(String& version, String& assetUrl, size_t& assetSize, String& digest) {
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
    if (String(name) == OTA_ASSET_NAME) {
      assetUrl = asset["browser_download_url"] | "";
      assetSize = asset["size"] | 0UL;
      digest = asset["digest"] | "";
      return version.length() > 0 && assetUrl.length() > 0 && assetSize > 0;
    }
  }

  otaLog("Latest release has no " + String(OTA_ASSET_NAME));
  return false;
}

bool performUpdate(const String& version, const String& assetUrl, size_t expectedSize, const String& expectedDigest) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CLIMORA-AutoOTA/1.0");

  otaLog("Downloading firmware " + version);
  if (!http.begin(client, assetUrl)) {
    otaLog("Unable to start firmware download");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    otaLog("Firmware download HTTP=" + String(code));
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) contentLength = (int)expectedSize;

  if (expectedSize > 0 && (size_t)contentLength != expectedSize) {
    otaLog("Size mismatch: expected=" + String(expectedSize) + " received=" + String(contentLength));
    http.end();
    return false;
  }

  if (!Update.begin((size_t)contentLength)) {
    otaLog("Update.begin failed, error=" + String(Update.getError()));
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buffer[2048];
  unsigned long lastProgress = 0;

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);

  while (http.connected() && (contentLength <= 0 || written < (size_t)contentLength)) {
    size_t available = stream->available();
    if (available) {
      size_t toRead = available;
      if (toRead > sizeof(buffer)) toRead = sizeof(buffer);
      int read = stream->readBytes(buffer, toRead);
      if (read <= 0) break;

      size_t w = Update.write(buffer, (size_t)read);
      if (w != (size_t)read) {
        otaLog("Flash write failed, error=" + String(Update.getError()));
        Update.abort();
        http.end();
        mbedtls_sha256_free(&sha);
        return false;
      }

      mbedtls_sha256_update_ret(&sha, buffer, (size_t)read);
      written += w;

      if (millis() - lastProgress >= 1000) {
        lastProgress = millis();
        int percent = contentLength > 0 ? (int)((written * 100UL) / (size_t)contentLength) : 0;
        otaLog("Progress=" + String(percent) + "% (" + String(written) + "/" + String(contentLength) + ")");
      }
    } else {
      delay(10);
    }
  }

  http.end();

  if (contentLength > 0 && written != (size_t)contentLength) {
    mbedtls_sha256_free(&sha);
    otaLog("Incomplete download: " + String(written) + "/" + String(contentLength));
    Update.abort();
    return false;
  }

  uint8_t hash[32];
  mbedtls_sha256_finish_ret(&sha, hash);
  mbedtls_sha256_free(&sha);

  String actualDigest = "sha256:";
  const char* hex = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(hash); ++i) {
    actualDigest += hex[(hash[i] >> 4) & 0x0F];
    actualDigest += hex[hash[i] & 0x0F];
  }

  if (expectedDigest.length() == 0 || !expectedDigest.equalsIgnoreCase(actualDigest)) {
    otaLog("SHA-256 verification failed");
    otaLog("Expected=" + expectedDigest);
    otaLog("Actual=" + actualDigest);
    Update.abort();
    return false;
  }

  otaLog("SHA-256 verified");

  if (!Update.end(true)) {
    otaLog("Update.end failed, error=" + String(Update.getError()));
    return false;
  }

  if (!Update.isFinished()) {
    otaLog("Update did not finish");
    return false;
  }

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
    return;
  }

  String current = storedVersion();
  String latest;
  String assetUrl;
  String digest;
  size_t assetSize = 0;

  otaLog("Current firmware=" + current);
  otaLog("Checking GitHub Releases...");

  if (!fetchLatestRelease(latest, assetUrl, assetSize, digest)) return;

  otaLog("Latest release=" + latest);

  if (compareVersions(current, latest) >= 0) {
    otaLog("Firmware is up to date");
    return;
  }

  otaLog("Update available: " + current + " -> " + latest);
  otaLog("Asset size=" + String(assetSize) + " bytes");
  otaLog("Release digest=" + digest);

  performUpdate(latest, assetUrl, assetSize, digest);
}

void otaTask(void*) {
  delay(OTA_FIRST_CHECK_DELAY_MS);

  unsigned long lastCheck = 0;
  bool first = true;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (first || millis() - lastCheck >= OTA_CHECK_INTERVAL_MS) {
        first = false;
        lastCheck = millis();
        checkForUpdate();
      }
    }
    delay(1000);
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
      0
    );
  }
};

AutoOTAStarter autoOTAStarter;
}
