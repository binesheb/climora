/************************************************************
 * CLIMORA v5.4
 * Natural ambience engine – Single ambience bar (behind monitor)
 *
 * - Single strip (60 LEDs, pin 5)
 * - Weather is the primary visual layer
 * - Sunrise/Sunset are solar tint overlays (background layer)
 * - Morning & Night Stability Windows (brightness-safe, motion-soft)
 * - Strong smoothing (brightness, clouds, wind, POP, visibility)
 * - Effect hysteresis (no mode flapping)
 * - Mood governor (calm / neutral / energetic / dramatic)
 * - Solar-aware palettes (weather colors adapt to time of day)
 * - Non-blocking thunder, OTA-safe, weather-failure safe mode
 * - Fireflies only in natural calm night windows (clear, gentle)
 * - All major features from v5.2/v5.3 preserved and refined
 * - DIAGNOSTICS_MODE: R/G/B/W, wheel, all effects, natural fireflies
 ************************************************************/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <time.h>
#include <ArduinoOTA.h>

/************************************************************
 * 1. USER CONFIG
 ************************************************************/

// WiFi
const char *WIFI_SSID = "JSPL Directors";
const char *WIFI_PASSWORD = "Directors@jspl";

// OpenWeatherMap API key (Current Weather API)
const char *WEATHER_API_KEY = "c3aebea47c249bc5f4c3584abc35e2bf";

// Location (Kochi)
const float LOCATION_LAT = 10.003945359505757;
const float LOCATION_LON = 76.31380963757567;

// Timezone offset (IST, +5:30)
const long IST_OFFSET = 19800;

// Single strip config (ambience bar)
#define DATA_PIN 5
#define NUM_LEDS 60
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

CRGB leds[NUM_LEDS];

// Status LED
#define STATUS_LED 2

// Brightness
uint8_t BRIGHTNESS_DAY = 120;
uint8_t BRIGHTNESS_NIGHT = 35;

// Diagnostics
#define DIAGNOSTICS_MODE true          // true: R/G/B/W, wheel, all effects, fireflies
#define DIAGNOSTICS_SIMULATE_DAY false  // true: 24h looped simulation
const float SIM_SPEED = 240.0f;         // 1 sec = 4 sim minutes

/************************************************************
 * 2. GLOBAL STATE
 ************************************************************/

struct WeatherData {
  String condition;
  String description;

  float temp;
  float feelsLike;
  float tempMin;
  float tempMax;

  float humidity;
  float pressure;
  float visibility;
  float cloudCover;

  float windSpeed;
  float windGust;
  float windDir;
  float windSmooth;

  float rain1h;
  float snow1h;

  float dewPoint;
  float uvIndex;
  float pop;

  unsigned long sunrise;  // IST epoch for TODAY
  unsigned long sunset;   // IST epoch for TODAY
};

WeatherData weather;

// Smoothed weather fields
float smoothCloudCover = 0.0f;
float smoothWindSpeed = 0.0f;
float smoothPOP = 0.0f;
float smoothVisibility = 0.0f;

bool internetOK = false;

unsigned long lastWeatherFetch = 0;
const unsigned long WEATHER_INTERVAL = 300000;  // 5 minutes

unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 10000;

unsigned long statusLedTimer = 0;
bool statusLedState = false;

// Simulation
unsigned long simMidnight = 0;
unsigned long simStartMillis = 0;

// Smoothed brightness
uint8_t smoothedBrightness = 0;

/************************************************************
 * 3. EFFECT ENGINE STATE
 ************************************************************/

enum EffectMode {
  FX_CLEAR,
  FX_CLOUDS,
  FX_RAIN,
  FX_THUNDER,
  FX_FOG,
  FX_DEFAULT,
  FX_HEAVY_RAIN,

  FX_GUST,
  FX_HEATWAVE,
  FX_COLDWAVE,
  FX_DENSE_FOG,
  FX_HAZE,
  FX_UV_HIGH,
  FX_DEW_MORNING,
  FX_PRE_RAIN,
  FX_ALERT,

  FX_AURORA,
  FX_OCEAN,
  FX_SUNSET
};

EffectMode activeEffect = FX_DEFAULT;
EffectMode lastStableEffect = FX_DEFAULT;
EffectMode pendingEffect = FX_DEFAULT;
String activeEffectName = "Default";

unsigned long lastEffectChangeMillis = 0;

/************************************************************
 * 4. MOOD GOVERNOR
 ************************************************************/

enum MoodState {
  MOOD_CALM,
  MOOD_NEUTRAL,
  MOOD_ENERGETIC,
  MOOD_DRAMATIC
};

MoodState currentMood = MOOD_NEUTRAL;

// Mood parameters (derived)
float moodMotionScale = 1.0f;   // 0.5–1.5
float moodSaturation = 1.0f;    // 0.7–1.2
float moodOverlayScale = 1.0f;  // 0.7–1.3

/************************************************************
 * 5. DIAGNOSTICS EFFECT LIST
 ************************************************************/

EffectMode diagnosticsEffects[] = {
  FX_CLEAR,
  FX_CLOUDS,
  FX_RAIN,
  FX_HEAVY_RAIN,
  FX_THUNDER,
  FX_FOG,
  FX_DENSE_FOG,
  FX_HAZE,
  FX_PRE_RAIN,
  FX_UV_HIGH,
  FX_DEW_MORNING,
  FX_HEATWAVE,
  FX_COLDWAVE,
  FX_GUST,
  FX_ALERT,
  FX_AURORA,
  FX_OCEAN,
  FX_SUNSET,
  FX_DEFAULT
};
const int diagnosticsEffectCount = sizeof(diagnosticsEffects) / sizeof(diagnosticsEffects[0]);

/************************************************************
 * 6. UTILITIES
 ************************************************************/

String formatIST(unsigned long epoch) {
  if (epoch == 0) return "Unknown";
  time_t raw = epoch;
  struct tm *ti = localtime(&raw);
  char buf[32];
  strftime(buf, sizeof(buf), "%H:%M:%S IST", ti);
  return String(buf);
}

void logMsg(const char *tag, const String &msg) {
  Serial.printf("[%s] %s\n", tag, msg.c_str());
}

bool frameDueScaled(unsigned long &last, unsigned long baseIntervalMs) {
  float scale = 1.0f / moodMotionScale;
  unsigned long interval = (unsigned long)(baseIntervalMs * scale);
  unsigned long now = millis();
  if (now - last >= interval) {
    last = now;
    return true;
  }
  return false;
}

bool frameDue(unsigned long &last, unsigned long interval) {
  unsigned long now = millis();
  if (now - last >= interval) {
    last = now;
    return true;
  }
  return false;
}

/************************************************************
 * 7. HOSTNAME & STATUS LED
 ************************************************************/

String getClimoraHostname() {
  uint64_t mac = ESP.getEfuseMac();
  char suffix[5];
  sprintf(suffix, "%04X", (uint16_t)(mac & 0xFFFF));
  return String("CLIMORA-") + suffix;
}

void blinkStatusLED(unsigned long interval) {
  if (millis() - statusLedTimer > interval) {
    statusLedTimer = millis();
    statusLedState = !statusLedState;
    digitalWrite(STATUS_LED, statusLedState);
  }
}

void updateStatusLED() {
  if (internetOK) {
    digitalWrite(STATUS_LED, HIGH);
  } else if (WiFi.status() == WL_CONNECTED) {
    blinkStatusLED(200);
  } else {
    blinkStatusLED(600);
  }
}

/************************************************************
 * 8. WIFI + TIME + OTA
 ************************************************************/

void initWiFi() {
  WiFi.setHostname(getClimoraHostname().c_str());
  logMsg("WIFI", "Connecting to SSID: " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    blinkStatusLED(500);
    logMsg("WIFI", "Waiting for WiFi...");
    delay(500);
  }

  logMsg("WIFI", "Connected");
  logMsg("WIFI", "IP: " + WiFi.localIP().toString());
}

void maintainWiFi() {
  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL) return;
  lastWiFiCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    logMsg("WIFI", "Lost connection, reconnecting...");
    WiFi.reconnect();
  }
}

void initTime() {
  logMsg("TIME", "Requesting NTP time");
  configTime(IST_OFFSET, 0, "pool.ntp.org");

  time_t now = 0;
  int retries = 0;
  while (now < 1577836800 && retries < 20) {
    delay(500);
    time(&now);
    retries++;
  }

  logMsg("TIME", "Local time: " + formatIST(now));
}

void initOTA() {
  ArduinoOTA.setHostname(getClimoraHostname().c_str());

  ArduinoOTA.onStart([]() {
    logMsg("OTA", "Start updating, entering safe visual state");
    FastLED.clear(true);
    FastLED.setBrightness(40);
    FastLED.show();
  });

  ArduinoOTA.onEnd([]() {
    logMsg("OTA", "Update finished");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    logMsg("OTA", "Error: " + String(error));
  });

  ArduinoOTA.begin();
  logMsg("OTA", "Ready for OTA updates");
}

/************************************************************
 * 9. LED STRIP (SINGLE AMBIENCE BAR)
 ************************************************************/

void initLEDStrip() {
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 2000);
  FastLED.setMaxRefreshRate(60);
  FastLED.clear();
  FastLED.setBrightness(BRIGHTNESS_DAY);
  FastLED.show();
  smoothedBrightness = BRIGHTNESS_DAY;
  logMsg("LED", "LED driver ready (single ambience bar)");
}

/************************************************************
 * 10. APPROXIMATION HELPERS
 ************************************************************/

float approximateDewPoint(float tempC, float humidity) {
  if (humidity <= 0) return tempC - 10.0f;
  float a = 17.27f;
  float b = 237.7f;
  float alpha = ((a * tempC) / (b + tempC)) + log(humidity / 100.0f);
  float dp = (b * alpha) / (a - alpha);
  return dp;
}

float approximateUVIndex() {
  time_t now;
  time(&now);
  struct tm ti;
  if (!localtime_r(&now, &ti)) return 5.0f;

  int hour = ti.tm_hour;
  float baseUV = 0.0f;

  if (hour >= 11 && hour <= 14) baseUV = 9.0f;
  else if (hour >= 9 && hour <= 16) baseUV = 6.0f;
  else if (hour >= 7 && hour <= 17) baseUV = 3.0f;
  else baseUV = 0.5f;

  float cloudFactor = weather.cloudCover / 100.0f;
  float uv = baseUV * (1.0f - 0.6f * cloudFactor);
  if (uv < 0.0f) uv = 0.0f;
  return uv;
}

float approximatePOP() {
  if (weather.rain1h > 0.0f) return 0.9f;
  if (weather.cloudCover > 90.0f) return 0.7f;
  if (weather.cloudCover > 70.0f) return 0.5f;
  if (weather.cloudCover > 50.0f) return 0.3f;
  return 0.1f;
}

void approximateMinMax() {
  weather.tempMin = weather.temp - 2.0f;
  weather.tempMax = weather.temp + 2.0f;
}

/************************************************************
 * 11. SUNRISE/SUNSET (C1, FIXED)
 ************************************************************/

unsigned long computeTodayISTMidnight() {
  time_t now;
  time(&now);
  struct tm ti;
  localtime_r(&now, &ti);
  ti.tm_hour = 0;
  ti.tm_min = 0;
  ti.tm_sec = 0;
  time_t midnight = mktime(&ti);
  return (unsigned long)midnight;
}

void applyC1SunEvents(unsigned long sunriseUTC, unsigned long sunsetUTC) {
  if (sunriseUTC == 0 || sunsetUTC == 0) {
    weather.sunrise = 0;
    weather.sunset = 0;
    return;
  }

  time_t srUTC = (time_t)sunriseUTC;
  time_t ssUTC = (time_t)sunsetUTC;

  struct tm srTmUTC;
  struct tm ssTmUTC;

  gmtime_r(&srUTC, &srTmUTC);
  gmtime_r(&ssUTC, &ssTmUTC);

  int srSecUTC = srTmUTC.tm_hour * 3600 + srTmUTC.tm_min * 60 + srTmUTC.tm_sec;
  int ssSecUTC = ssTmUTC.tm_hour * 3600 + ssTmUTC.tm_min * 60 + ssTmUTC.tm_sec;

  int srSecIST = (srSecUTC + IST_OFFSET) % 86400;
  int ssSecIST = (ssSecUTC + IST_OFFSET) % 86400;
  if (srSecIST < 0) srSecIST += 86400;
  if (ssSecIST < 0) ssSecIST += 86400;

  unsigned long todayMidnightIST = computeTodayISTMidnight();

  weather.sunrise = todayMidnightIST + srSecIST;
  weather.sunset = todayMidnightIST + ssSecIST;

  logMsg("TIME", "Sunrise (IST, C1): " + formatIST(weather.sunrise));
  logMsg("TIME", "Sunset  (IST, C1): " + formatIST(weather.sunset));
}

/************************************************************
 * 12. WEATHER SERVICE
 ************************************************************/

bool weatherUpdateDue() {
  return (millis() - lastWeatherFetch > WEATHER_INTERVAL);
}

void smoothWeatherFields() {
  static bool first = true;
  if (first) {
    smoothCloudCover = weather.cloudCover;
    smoothWindSpeed = weather.windSpeed;
    smoothPOP = weather.pop;
    smoothVisibility = weather.visibility;
    first = false;
  }

  float alpha = 0.1f;

  smoothCloudCover = smoothCloudCover * (1.0f - alpha) + weather.cloudCover * alpha;
  smoothWindSpeed = smoothWindSpeed * (1.0f - alpha) + weather.windSpeed * alpha;
  smoothPOP = smoothPOP * (1.0f - alpha) + weather.pop * alpha;
  smoothVisibility = smoothVisibility * (1.0f - alpha) + weather.visibility * alpha;
}

void fetchWeather() {
  logMsg("WEATHER", "Fetching (Current Weather API, lat/lon)...");

  if (WiFi.status() != WL_CONNECTED) {
    internetOK = false;
    logMsg("WEATHER", "WiFi disconnected, skipping");
    return;
  }

  HTTPClient http;
  String url =
    "https://api.openweathermap.org/data/2.5/weather?lat=" + String(LOCATION_LAT, 6) + "&lon=" + String(LOCATION_LON, 6) + "&units=metric&appid=" + WEATHER_API_KEY;

  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();

  if (code != 200) {
    internetOK = false;
    logMsg("WEATHER", "HTTP error: " + String(code));
    http.end();
    return;
  }

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  if (err) {
    internetOK = false;
    logMsg("WEATHER", "JSON parse error");
    http.end();
    return;
  }

  weather.condition = doc["weather"][0]["main"].as<String>();
  weather.description = doc["weather"][0]["description"].as<String>();

  JsonObject main = doc["main"];
  weather.temp = main["temp"] | 0.0f;
  weather.feelsLike = main["feels_like"] | weather.temp;
  weather.humidity = main["humidity"] | 0.0f;
  weather.pressure = main["pressure"] | 0.0f;

  weather.visibility = doc["visibility"] | 10000.0f;
  weather.cloudCover = doc["clouds"]["all"] | 0.0f;

  weather.windSpeed = doc["wind"]["speed"] | 0.0f;
  weather.windGust = doc["wind"]["gust"] | weather.windSpeed;
  weather.windDir = doc["wind"]["deg"] | 0.0f;

  weather.rain1h = doc["rain"]["1h"] | 0.0f;
  weather.snow1h = doc["snow"]["1h"] | 0.0f;

  unsigned long sunriseUTC = doc["sys"]["sunrise"] | 0UL;
  unsigned long sunsetUTC = doc["sys"]["sunset"] | 0UL;

  applyC1SunEvents(sunriseUTC, sunsetUTC);

  weather.dewPoint = approximateDewPoint(weather.temp, weather.humidity);
  weather.uvIndex = approximateUVIndex();
  weather.pop = approximatePOP();
  approximateMinMax();

  if (weather.windSmooth == 0.0f) weather.windSmooth = weather.windSpeed;
  weather.windSmooth = weather.windSmooth * 0.85f + weather.windSpeed * 0.15f;

  smoothWeatherFields();

  internetOK = true;

  logMsg("WEATHER", "Condition: " + weather.condition + " (" + weather.description + ")");
  logMsg("WEATHER", "Temp: " + String(weather.temp) + "°C, Feels: " + String(weather.feelsLike) + "°C");
  logMsg("WEATHER", "Humidity: " + String(weather.humidity) + "%, Clouds: " + String(weather.cloudCover) + "%");
  logMsg("WEATHER", "Wind: " + String(weather.windSpeed) + " m/s, Gust: " + String(weather.windGust) + " m/s");
  logMsg("WEATHER", "Rain1h: " + String(weather.rain1h) + " mm, POP≈ " + String(weather.pop * 100.0f) + "%");
  logMsg("WEATHER", "DewPoint≈ " + String(weather.dewPoint) + "°C, UV≈ " + String(weather.uvIndex));
  http.end();
}

/************************************************************
 * 13. TIME & NIGHT LOGIC
 ************************************************************/

unsigned long getSimulatedTime() {
  if (!DIAGNOSTICS_SIMULATE_DAY) return 0;

  unsigned long nowMs = millis();
  unsigned long elapsedMs = nowMs - simStartMillis;
  unsigned long simSeconds = (unsigned long)(elapsedMs / 1000.0f * SIM_SPEED);
  simSeconds %= 86400UL;
  return simMidnight + simSeconds;
}

unsigned long getNowEpoch() {
  if (DIAGNOSTICS_SIMULATE_DAY) {
    return getSimulatedTime();
  } else {
    time_t now;
    time(&now);
    return (unsigned long)now;
  }
}

bool isNightFallback(unsigned long now) {
  time_t t = (time_t)now;
  struct tm ti;
  if (!localtime_r(&t, &ti)) return false;
  int h = ti.tm_hour;
  return (h < 6 || h >= 19);
}

bool isNight() {
  unsigned long now = getNowEpoch();

  if (weather.sunrise == 0 || weather.sunset == 0) {
    return isNightFallback(now);
  }

  unsigned long sr = weather.sunrise;
  unsigned long ss = weather.sunset;

  if (sr < ss) {
    if (now >= sr && now <= ss) return false;
    return true;
  }

  if (now >= sr || now <= ss) return false;
  return true;
}

float solarProgress(unsigned long eventTime, unsigned long windowSeconds) {
  if (eventTime == 0) return 0.0f;

  unsigned long now = getNowEpoch();
  unsigned long start = eventTime - windowSeconds;
  unsigned long end = eventTime + windowSeconds;

  if (now <= start || now >= end) return 0.0f;
  return float(now - start) / float(2 * windowSeconds);
}

void getLocalHourMinute(int &hour, int &minute) {
  unsigned long now = getNowEpoch();
  time_t t = (time_t)now;
  struct tm ti;
  localtime_r(&t, &ti);
  hour = ti.tm_hour;
  minute = ti.tm_min;
}

/************************************************************
 * 14. STABILITY WINDOWS
 ************************************************************/

bool isMorningStabilityWindow() {
  if (weather.sunrise == 0) {
    int h, m;
    getLocalHourMinute(h, m);
    int mins = h * 60 + m;
    return (mins >= 6 * 60 && mins <= 9 * 60);
  }

  unsigned long now = getNowEpoch();
  long diff = (long)now - (long)weather.sunrise;

  return (diff >= -45 * 60 && diff <= 90 * 60);
}

bool isNightStabilityWindow() {
  if (weather.sunset == 0) {
    int h, m;
    getLocalHourMinute(h, m);
    int mins = h * 60 + m;
    return (mins >= 18 * 60 && mins <= 21 * 60);
  }

  unsigned long now = getNowEpoch();
  long diff = (long)now - (long)weather.sunset;

  return (diff >= -30 * 60 && diff <= 60 * 60);
}

bool isAnyStabilityWindow() {
  return isMorningStabilityWindow() || isNightStabilityWindow();
}

/************************************************************
 * 15. MOOD GOVERNOR
 ************************************************************/

void updateMood() {
  bool night = isNight();
  float clouds = smoothCloudCover;
  float wind = smoothWindSpeed;
  float pop = smoothPOP;
  float vis = smoothVisibility;

  MoodState mood = MOOD_NEUTRAL;

  if (weather.rain1h > 3.0f || wind > 11.0f || pop > 0.8f) {
    mood = MOOD_DRAMATIC;
  }

  if (!night && clouds < 50.0f && wind > 3.0f && wind < 9.0f && pop < 0.5f) {
    mood = MOOD_ENERGETIC;
  }

  if (night && wind < 4.0f && pop < 0.4f && vis > 4000.0f) {
    mood = MOOD_CALM;
  }

  currentMood = mood;

  switch (currentMood) {
    case MOOD_CALM:
      moodMotionScale = 0.6f;
      moodSaturation = 0.85f;
      moodOverlayScale = 1.1f;
      break;
    case MOOD_NEUTRAL:
      moodMotionScale = 1.0f;
      moodSaturation = 1.0f;
      moodOverlayScale = 1.0f;
      break;
    case MOOD_ENERGETIC:
      moodMotionScale = 1.3f;
      moodSaturation = 1.15f;
      moodOverlayScale = 0.9f;
      break;
    case MOOD_DRAMATIC:
      moodMotionScale = 1.1f;
      moodSaturation = 1.2f;
      moodOverlayScale = 1.2f;
      break;
  }
}

/************************************************************
 * 16. SOLAR-AWARE COLOR HELPERS
 ************************************************************/

float solarDayFactor() {
  int hour, minute;
  getLocalHourMinute(hour, minute);
  float h = hour + minute / 60.0f;

  if (h < 6.0f || h > 19.0f) return 0.0f;
  if (h <= 12.0f) {
    return (h - 6.0f) / 6.0f;
  } else {
    return (19.0f - h) / 7.0f;
  }
}

CRGB temperatureColor(float t) {
  if (t <= 18) return CRGB(80, 140, 255);
  if (t >= 32) return CRGB(255, 120, 40);
  float k = (t - 18.0f) / 14.0f;
  return blend(CRGB(80, 140, 255), CRGB(255, 120, 40), uint8_t(k * 255));
}

CRGB applyMoodSaturation(const CRGB &c) {
CHSV hsv = rgb2hsv_approximate(c);
  hsv.s = uint8_t(constrain(hsv.s * moodSaturation, 0, 255));
  return hsv;
}

/************************************************************
 * 17. EFFECT SELECTION (HYSTERESIS)
 ************************************************************/

EffectMode classifyEffectFromWeather() {
  String c = weather.condition;

  float clouds = smoothCloudCover;
  float wind = smoothWindSpeed;
  float pop = smoothPOP;
  float vis = smoothVisibility;

  if (weather.rain1h > 5.0f || (pop > 0.85f && clouds > 90.0f)) {
    return FX_HEAVY_RAIN;
  }

  if (c == "Thunderstorm" || (weather.rain1h > 2.0f && wind > 9.0f && clouds > 85.0f)) {
    return FX_THUNDER;
  }

  if (vis < 500.0f) {
    return FX_DENSE_FOG;
  }

  if ((c == "Haze" || c == "Smoke") || (weather.humidity > 85.0f && clouds > 60.0f && vis < 3000.0f)) {
    return FX_HAZE;
  }

  if (weather.windGust > 13.0f || wind > 11.0f) {
    return FX_GUST;
  }

  if (weather.temp > 35.0f || weather.feelsLike > 40.0f) {
    return FX_HEATWAVE;
  }

  if (weather.temp < 15.0f || weather.feelsLike < 12.0f) {
    return FX_COLDWAVE;
  }

  if (weather.uvIndex > 7.0f && !isNight()) {
    return FX_UV_HIGH;
  }

  if (weather.dewPoint < 18.0f && weather.temp < 22.0f && !isNight()) {
    return FX_DEW_MORNING;
  }

  if (pop > 0.65f && weather.rain1h == 0.0f) {
    return FX_PRE_RAIN;
  }

  if (c == "Clear") {
    return FX_CLEAR;
  }

  if (c == "Clouds") {
    unsigned long nowEpoch = getNowEpoch();
    long diff = (long)weather.sunset - (long)nowEpoch;
    if (!isNight() && diff > -3600 && diff < 0) {
      return FX_SUNSET;
    } else {
      return FX_CLOUDS;
    }
  }

  if (c == "Rain" || c == "Drizzle") {
    return FX_RAIN;
  }

  if (c == "Fog" || c == "Mist") {
    return FX_FOG;
  }

  if (weather.temp > 24.0f && weather.temp < 30.0f && weather.humidity > 70.0f && wind > 3.0f && wind < 8.0f) {
    return FX_OCEAN;
  }

  if (isNight() && clouds < 30.0f && weather.uvIndex < 3.0f) {
    return FX_AURORA;
  }

  if (wind > 12.0f && clouds > 80.0f && weather.rain1h > 0.0f) {
    return FX_ALERT;
  }

  return FX_DEFAULT;
}

void updateEffectName() {
  switch (activeEffect) {
    case FX_CLEAR: activeEffectName = "Clear"; break;
    case FX_CLOUDS: activeEffectName = "Clouds"; break;
    case FX_RAIN: activeEffectName = "Rain"; break;
    case FX_THUNDER: activeEffectName = "Thunderstorm"; break;
    case FX_FOG: activeEffectName = "Fog/Mist"; break;
    case FX_DEFAULT: activeEffectName = "Default"; break;
    case FX_HEAVY_RAIN: activeEffectName = "Heavy Rain"; break;
    case FX_GUST: activeEffectName = "Gust"; break;
    case FX_HEATWAVE: activeEffectName = "Heatwave"; break;
    case FX_COLDWAVE: activeEffectName = "Coldwave"; break;
    case FX_DENSE_FOG: activeEffectName = "Dense Fog"; break;
    case FX_HAZE: activeEffectName = "Haze/Smog"; break;
    case FX_UV_HIGH: activeEffectName = "UV High"; break;
    case FX_DEW_MORNING: activeEffectName = "Dew Morning"; break;
    case FX_PRE_RAIN: activeEffectName = "Pre-Rain"; break;
    case FX_ALERT: activeEffectName = "Alert-like Storm"; break;
    case FX_AURORA: activeEffectName = "Aurora"; break;
    case FX_OCEAN: activeEffectName = "Ocean"; break;
    case FX_SUNSET: activeEffectName = "Sunset Glow"; break;
  }
}

void applyEffectHysteresis() {
  EffectMode classified = classifyEffectFromWeather();

  if (classified != pendingEffect) {
    pendingEffect = classified;
    lastEffectChangeMillis = millis();
    return;
  }

  if (millis() - lastEffectChangeMillis > 2 * WEATHER_INTERVAL) {
    if (classified != lastStableEffect) {
      lastStableEffect = classified;
      activeEffect = classified;
      updateEffectName();
      logMsg("EFFECT", "Effect stabilized: " + activeEffectName);
    }
  }
}

/************************************************************
 * 18. SUNRISE/SUNSET OVERLAY
 ************************************************************/

float easeInOut(float x) {
  x = constrain(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

float computeOverlayStrength() {
  float cloud = smoothCloudCover / 100.0f;
  float base;
  if (cloud < 0.2f) base = 0.75f;
  else if (cloud < 0.6f) base = 0.50f;
  else base = 0.25f;

  base *= moodOverlayScale;
  return constrain(base, 0.2f, 0.9f);
}

CRGB sunriseOverlayColor(float k) {
  float cloud = smoothCloudCover / 100.0f;
  k = easeInOut(k);

  CRGB c1_clear = CRGB(255, 90, 20);
  CRGB c2_clear = CRGB(255, 190, 60);
  CRGB c3_clear = CRGB(255, 255, 210);

  CRGB c1_cloudy = CRGB(255, 140, 80);
  CRGB c2_cloudy = CRGB(255, 210, 150);
  CRGB c3_cloudy = CRGB(240, 240, 220);

  CRGB c1 = blend(c1_clear, c1_cloudy, uint8_t(cloud * 255));
  CRGB c2 = blend(c2_clear, c2_cloudy, uint8_t(cloud * 255));
  CRGB c3 = blend(c3_clear, c3_cloudy, uint8_t(cloud * 255));

  CRGB mix1 = blend(c1, c2, uint8_t(k * 255));
  CRGB mix2 = blend(c2, c3, uint8_t(k * 255));

  return blend(mix1, mix2, 128);
}

CRGB sunsetOverlayColor(float k) {
  float cloud = smoothCloudCover / 100.0f;
  k = easeInOut(k);

  CRGB c1_clear = CRGB(255, 130, 40);
  CRGB c2_clear = CRGB(255, 80, 140);
  CRGB c3_clear = CRGB(130, 50, 140);

  CRGB c1_cloudy = CRGB(255, 160, 90);
  CRGB c2_cloudy = CRGB(240, 140, 160);
  CRGB c3_cloudy = CRGB(180, 120, 170);

  CRGB c1 = blend(c1_clear, c1_cloudy, uint8_t(cloud * 255));
  CRGB c2 = blend(c2_clear, c2_cloudy, uint8_t(cloud * 255));
  CRGB c3 = blend(c3_clear, c3_cloudy, uint8_t(cloud * 255));

  CRGB mix1 = blend(c1, c2, uint8_t(k * 255));
  CRGB mix2 = blend(c2, c3, uint8_t(k * 255));

  return blend(mix1, mix2, 128);
}

void applySunOverlay(float sunriseFactor, float sunsetFactor) {
  if (sunriseFactor <= 0.0f && sunsetFactor <= 0.0f) return;

  float overlayStrength = computeOverlayStrength();

  for (int i = 0; i < NUM_LEDS; i++) {
    float t = float(i) / (NUM_LEDS - 1);
    CRGB base = leds[i];

    if (sunriseFactor > 0.0f && sunsetFactor <= 0.0f) {
      CRGB c = sunriseOverlayColor(sunriseFactor);
      CRGB overlay = blend(c, CRGB::White, uint8_t(t * 40));
      float k = sunriseFactor * overlayStrength;
      leds[i] = blend(base, overlay, uint8_t(k * 255));
    } else if (sunsetFactor > 0.0f && sunriseFactor <= 0.0f) {
      CRGB c = sunsetOverlayColor(sunsetFactor);
      CRGB overlay = blend(c, CRGB::Black, uint8_t(t * 60));
      float k = sunsetFactor * overlayStrength;
      leds[i] = blend(base, overlay, uint8_t(k * 255));
    } else {
      CRGB c1 = sunriseOverlayColor(sunriseFactor);
      CRGB c2 = sunsetOverlayColor(sunsetFactor);
      CRGB mix = blend(c1, c2, 128);
      float k = max(sunriseFactor, sunsetFactor) * (overlayStrength * 0.7f);
      leds[i] = blend(base, mix, uint8_t(k * 255));
    }
  }
}

/************************************************************
 * 19. LIGHTING LOGIC
 ************************************************************/

void applyLightingLogic() {
  float sunriseFactor = solarProgress(weather.sunrise, 60 * 60);
  float sunsetFactor = solarProgress(weather.sunset, 60 * 60);

  uint8_t targetBrightness = isNight() ? BRIGHTNESS_NIGHT : BRIGHTNESS_DAY;

  if (sunriseFactor > 0.0f) {
    float b = BRIGHTNESS_NIGHT + sunriseFactor * (BRIGHTNESS_DAY - BRIGHTNESS_NIGHT);
    targetBrightness = (uint8_t)b;
  }

  if (sunsetFactor > 0.0f) {
    float delta = sunsetFactor * 25.0f;
    int b = (int)targetBrightness - (int)delta;
    targetBrightness = (uint8_t)constrain(b, 5, BRIGHTNESS_DAY);
  }

  float cloudFactor = smoothCloudCover / 100.0f;
  targetBrightness = (uint8_t)(targetBrightness * (1.0f - 0.2f * cloudFactor));
  targetBrightness = constrain(targetBrightness, 5, BRIGHTNESS_DAY);

  float alpha = isAnyStabilityWindow() ? 0.05f : 0.15f;
  smoothedBrightness = (uint8_t)(smoothedBrightness * (1.0f - alpha) + targetBrightness * alpha);
  FastLED.setBrightness(smoothedBrightness);

  if (!internetOK) {
    activeEffect = FX_DEFAULT;
    updateEffectName();
  } else {
    applyEffectHysteresis();
  }

  updateMood();

  if (DIAGNOSTICS_SIMULATE_DAY) {
    unsigned long t = getSimulatedTime();
    String simTimeStr = formatIST(t);

    logMsg("SIM",
           simTimeStr + " | Brightness=" + String(smoothedBrightness) + " | Effect=" + activeEffectName + " | Night=" + String(isNight()) + " | Temp=" + String(weather.temp) + " | Clouds=" + String(smoothCloudCover) + " | Wind=" + String(smoothWindSpeed) + " | POP=" + String(smoothPOP * 100.0f) + "%" + " | UV=" + String(weather.uvIndex) + " | Dew=" + String(weather.dewPoint) + " | Mood=" + String((int)currentMood) + " | SRF=" + String(sunriseFactor, 2) + " | SSF=" + String(sunsetFactor, 2));
  } else {
    logMsg("LIGHT",
           "Brightness=" + String(smoothedBrightness) + ", Mode=" + activeEffectName + ", Temp=" + String(weather.temp) + ", Wind=" + String(smoothWindSpeed) + ", Clouds=" + String(smoothCloudCover) + ", Mood=" + String((int)currentMood) + ", MorningStable=" + String(isMorningStabilityWindow()) + ", NightStable=" + String(isNightStabilityWindow()));
  }
}

/************************************************************
 * 20. EFFECTS
 ************************************************************/

bool effectBrightnessAllowed() {
  return !isAnyStabilityWindow();
}

/******** CLEAR ********/
void effectClear() {
  static uint8_t phase = 0;
  static unsigned long last = 0;
  if (!frameDueScaled(last, 40)) return;
  phase++;

  float dayFactor = solarDayFactor();

  for (int i = 0; i < NUM_LEDS; i++) {
    float t = float(i) / (NUM_LEDS - 1);

    CRGB base = temperatureColor(weather.temp);
    CRGB sky = blend(CRGB(120, 180, 255), CRGB(180, 220, 255), uint8_t(dayFactor * 255));
    CRGB mix = blend(base, sky, uint8_t((0.3f + 0.4f * dayFactor) * 255));

    mix = applyMoodSaturation(mix);

    if (effectBrightnessAllowed()) {
      uint8_t v = 140 + sin8(phase + i * 3) / 12;
      leds[i] = mix;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = mix;
      leds[i].fadeLightBy(uint8_t(t * 30));
    }
  }
}

/******** CLOUDS ********/
void effectClouds() {
  static uint8_t shift = 0;
  static unsigned long last = 0;
  if (!frameDueScaled(last, 45)) return;
  shift++;

  float dayFactor = solarDayFactor();

  for (int i = 0; i < NUM_LEDS; i++) {
    float t = float(i) / (NUM_LEDS - 1);

    CRGB cool = CRGB(170, 190, 210);
    CRGB warm = CRGB(210, 210, 230);
    CRGB base = blend(cool, warm, uint8_t(dayFactor * 180));

    base = applyMoodSaturation(base);

    if (effectBrightnessAllowed()) {
      uint8_t n1 = inoise8(i * 20, shift * 3);
      uint8_t n2 = inoise8(i * 40, shift * 2);
      uint8_t v = 100 + (n1 / 5) + (n2 / 8);
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(uint8_t(t * 20));
    }
  }
}

/******** RAIN ********/
void effectRain()
{
  static unsigned long last = 0;
  static uint16_t skyPhase = 0;

  struct Ripple
  {
    bool active;
    float center;
    float radius;
    float speed;
  };

  const int MAX_RIPPLES = 2;
  static Ripple ripples[MAX_RIPPLES];

  if (!frameDueScaled(last, 40))
    return;

  skyPhase++;

  // ---------- SKY ----------
  for (int i = 0; i < NUM_LEDS; i++)
  {
    uint8_t n = inoise8(i * 4, skyPhase);

    CRGB sky =
        blend(
            CRGB(90, 105, 120),
            CRGB(120, 135, 150),
            n);

    leds[i] = sky;
  }

  // ---------- NEW DROP ----------
  if (random8() < 3)
  {
    for (int r = 0; r < MAX_RIPPLES; r++)
    {
      if (!ripples[r].active)
      {
        ripples[r].active = true;
        ripples[r].center = random(NUM_LEDS);
        ripples[r].radius = 0;
        ripples[r].speed = 0.35f;
        break;
      }
    }
  }

  // ---------- RIPPLE ENGINE ----------
  for (int r = 0; r < MAX_RIPPLES; r++)
  {
    if (!ripples[r].active)
      continue;

    ripples[r].radius += ripples[r].speed;

    for (int i = 0; i < NUM_LEDS; i++)
    {
      float d = abs(i - ripples[r].center);

      float ring = abs(d - ripples[r].radius);

      if (ring < 0.8f)
      {
        leds[i] += CRGB(90, 90, 90);
      }
      else if (ring < 1.5f)
      {
        leds[i] += CRGB(40, 40, 40);
      }
    }

    if (ripples[r].radius > NUM_LEDS)
    {
      ripples[r].active = false;
    }
  }
}

/******** THUNDER ********/
void effectThunder()
{
  effectHeavyRain();

  static unsigned long nextFlash = 0;
  static bool flashActive = false;
  static unsigned long flashStart = 0;
  static uint8_t flashType = 0;

  unsigned long now = millis();

  if (nextFlash == 0)
  {
    nextFlash = now + random(180000, 420000);
  }

  if (!flashActive && now >= nextFlash)
  {
    flashActive = true;
    flashStart = now;

    flashType = (random8() < 20) ? 2 : 1;

    nextFlash = now + random(180000, 420000);

    logMsg("THUNDER", "Lightning");
  }

  if (!flashActive)
    return;

  unsigned long elapsed = now - flashStart;

  if (flashType == 1)
  {
    if (elapsed < 120)
    {
      for (int i = 0; i < NUM_LEDS; i++)
      {
        leds[i] += CRGB(80, 90, 110);
      }
    }
    else
    {
      flashActive = false;
    }
  }
  else
  {
    if (elapsed < 80)
    {
      for (int i = 0; i < NUM_LEDS; i++)
      {
        leds[i] += CRGB(120, 130, 150);
      }
    }
    else if (elapsed > 180 && elapsed < 260)
    {
      for (int i = 0; i < NUM_LEDS; i++)
      {
        leds[i] += CRGB(160, 170, 200);
      }
    }
    else if (elapsed > 350)
    {
      flashActive = false;
    }
  }
}

/******** FOG ********/
void effectFog() {
  static uint8_t shift = 0;
  static unsigned long last = 0;
  if (!frameDueScaled(last, 60)) return;
  shift++;

  float dayFactor = solarDayFactor();

  for (int i = 0; i < NUM_LEDS; i++) {
    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 30, shift * 2);
      uint8_t v = 90 + n / 5;
      CRGB base = CHSV(0, 0, v);
      CRGB warm = CRGB(255, 230, 200);
      base = blend(base, warm, uint8_t(dayFactor * 80));
      base = applyMoodSaturation(base);
      leds[i] = base;
    } else {
      CRGB base = CHSV(0, 0, 110);
      CRGB warm = CRGB(255, 230, 200);
      base = blend(base, warm, uint8_t(dayFactor * 80));
      base = applyMoodSaturation(base);
      leds[i] = base;
    }
  }
}

/******** DEFAULT ********/
void effectDefault() {
  static unsigned long last = 0;
  if (!frameDueScaled(last, 70)) return;

  float dayFactor = solarDayFactor();

  for (int i = 0; i < NUM_LEDS; i++) {
    float t = float(i) / (NUM_LEDS - 1);
    CRGB c = temperatureColor(weather.temp);
    CRGB sky = blend(CRGB(120, 160, 220), CRGB(200, 220, 255), uint8_t(dayFactor * 255));
    c = blend(c, sky, uint8_t(0.3f * 255));
    c = applyMoodSaturation(c);
    leds[i] = c;
    leds[i].fadeLightBy(uint8_t(t * 40));
  }
}

/******** HEAVY RAIN ********/
void effectHeavyRain()
{
  static unsigned long last = 0;
  static uint16_t skyPhase = 0;

  struct Ripple
  {
    bool active;
    float center;
    float radius;
    float speed;
  };

  const int MAX_RIPPLES = 8;
  static Ripple ripples[MAX_RIPPLES];

  if (!frameDueScaled(last, 35))
    return;

  skyPhase += 3;

  // ---------- SKY ----------
  for (int i = 0; i < NUM_LEDS; i++)
  {
    uint8_t n = inoise8(i * 3, skyPhase);

    CRGB sky =
        blend(
            CRGB(65, 80, 95),
            CRGB(100, 115, 130),
            n);

    leds[i] = sky;
  }

  // ---------- RAIN INTENSITY ----------
  float rainRate = max(weather.rain1h, 4.0f);

  uint8_t spawnChance =
      constrain(
          map((int)(rainRate * 10), 40, 150, 10, 35),
          10,
          35);

  // ---------- NEW DROPS ----------
  if (random8() < spawnChance)
  {
    for (int r = 0; r < MAX_RIPPLES; r++)
    {
      if (!ripples[r].active)
      {
        ripples[r].active = true;
        ripples[r].center = random(NUM_LEDS);
        ripples[r].radius = 0;
        ripples[r].speed = random(30, 55) / 100.0f;
        break;
      }
    }
  }

  // ---------- RIPPLE ENGINE ----------
  for (int r = 0; r < MAX_RIPPLES; r++)
  {
    if (!ripples[r].active)
      continue;

    ripples[r].radius += ripples[r].speed;

    for (int i = 0; i < NUM_LEDS; i++)
    {
      float d = abs(i - ripples[r].center);

      float ring = abs(d - ripples[r].radius);

      if (ring < 0.8f)
      {
        leds[i] += CRGB(120, 120, 120);
      }
      else if (ring < 1.8f)
      {
        leds[i] += CRGB(60, 60, 60);
      }
    }

    if (ripples[r].radius > NUM_LEDS)
    {
      ripples[r].active = false;
    }
  }
}

/******** GUST ********/
void effectGust() {
  static unsigned long last = 0;
  static uint8_t offset = 0;
  if (!frameDueScaled(last, 45)) return;
  offset++;

  for (int i = 0; i < NUM_LEDS; i++) {
    CRGB base = temperatureColor(weather.temp);
    base = applyMoodSaturation(base);
    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 40, offset * 4);
      uint8_t v = 120 + n / 7;
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(40);
    }
  }
}

/******** HEATWAVE ********/
void effectHeatwave() {
  static unsigned long last = 0;
  static uint8_t phase = 0;
  if (!frameDueScaled(last, 45)) return;
  phase++;

  for (int i = 0; i < NUM_LEDS; i++) {
    CRGB base = CRGB(255, 120, 40);
    base = applyMoodSaturation(base);
    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 30, phase * 4);
      uint8_t v = 150 + n / 7;
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(30);
    }
  }
}

/******** COLDWAVE ********/
void effectColdwave() {
  static unsigned long last = 0;
  static uint8_t phase = 0;
  if (!frameDueScaled(last, 50)) return;
  phase++;

  for (int i = 0; i < NUM_LEDS; i++) {
    CRGB base = CRGB(80, 140, 255);
    base = applyMoodSaturation(base);
    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 30, phase * 3);
      uint8_t v = 140 + n / 7;
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(30);
    }
  }
}

/******** DENSE FOG ********/
void effectDenseFog() {
  static unsigned long last = 0;
  static uint8_t shift = 0;
  if (!frameDueScaled(last, 60)) return;
  shift++;

  float dayFactor = solarDayFactor();

  for (int i = 0; i < NUM_LEDS; i++) {
    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 25, shift * 2);
      uint8_t v = 100 + n / 6;
      CRGB base = CHSV(0, 0, v);
      CRGB warm = CRGB(255, 230, 210);
      base = blend(base, warm, uint8_t(dayFactor * 100));
      base = applyMoodSaturation(base);
      leds[i] = base;
    } else {
      CRGB base = CHSV(0, 0, 120);
      CRGB warm = CRGB(255, 230, 210);
      base = blend(base, warm, uint8_t(dayFactor * 100));
      base = applyMoodSaturation(base);
      leds[i] = base;
    }
  }
}

/******** HAZE ********/
void effectHaze() {
  static unsigned long last = 0;
  static uint8_t shift = 0;
  if (!frameDueScaled(last, 55)) return;
  shift++;

  for (int i = 0; i < NUM_LEDS; i++) {
    CRGB base = CRGB(200, 180, 80);
    base = applyMoodSaturation(base);
    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 35, shift * 3);
      uint8_t v = 130 + n / 7;
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(40);
    }
  }
}

/******** UV HIGH ********/
void effectUVHigh() {
  static unsigned long last = 0;
  static uint8_t phase = 0;
  if (!frameDueScaled(last, 50)) return;
  phase++;

  for (int i = 0; i < NUM_LEDS; i++) {
    float t = float(i) / (NUM_LEDS - 1);
    CRGB base = blend(CRGB(255, 220, 120), CRGB(255, 150, 80), uint8_t(t * 255));
    base = applyMoodSaturation(base);
    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 25, phase * 4);
      uint8_t v = 170 + n / 9;
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(30);
    }
  }
}

/******** DEW MORNING ********/
void effectDewMorning() {
  static unsigned long last = 0;
  static uint8_t phase = 0;
  if (!frameDueScaled(last, 55)) return;
  phase++;

  for (int i = 0; i < NUM_LEDS; i++) {
    float t = float(i) / (NUM_LEDS - 1);
    CRGB base = blend(CRGB(120, 180, 255), CRGB(180, 230, 255), uint8_t(t * 255));
    base = applyMoodSaturation(base);
    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 30, phase * 3);
      uint8_t v = 140 + n / 9;
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(20);
    }
  }

  if (effectBrightnessAllowed() && random8() < 15) {
    int pos = random(NUM_LEDS);
    leds[pos] += CRGB(255, 255, 255);
  }
}

/******** PRE-RAIN ********/
void effectPreRain() {
  static unsigned long last = 0;
  static uint8_t phase = 0;
  if (!frameDueScaled(last, 55)) return;
  phase++;

  for (int i = 0; i < NUM_LEDS; i++) {
    CRGB base = CRGB(80, 120, 200);
    base = applyMoodSaturation(base);
    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 30, phase * 3);
      uint8_t v = 130 + n / 9;
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(30);
    }
  }

  if (effectBrightnessAllowed() && random8() < 12) {
    int pos = random(NUM_LEDS);
    leds[pos] += CRGB(90, 130, 255);
  }
}

/******** ALERT ********/
void effectAlert() {
  static unsigned long last = 0;
  static uint8_t phase = 0;
  if (!frameDueScaled(last, 75)) return;
  phase++;

  for (int i = 0; i < NUM_LEDS; i++) {
    CRGB base = CRGB(40, 0, 0);
    base = applyMoodSaturation(base);
    if (effectBrightnessAllowed()) {
      uint8_t v = 80 + sin8(phase) / 5;
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(60);
    }
  }

  if (effectBrightnessAllowed() && phase % 50 < 5) {
    for (int i = 0; i < NUM_LEDS; i++) {
      leds[i] += CRGB(120, 0, 0);
    }
  }
}

/******** AURORA ********/
void effectAurora() {
  static unsigned long last = 0;
  static uint8_t hueBase = 90;
  static uint8_t offset = 0;
  if (!frameDueScaled(last, 55)) return;
  offset++;
  hueBase++;

  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t n = inoise8(i * 20, offset * 3);
    uint8_t hue = hueBase + n / 8;
    uint8_t val = effectBrightnessAllowed() ? (110 + n / 7) : 120;
    CRGB c = CHSV(hue, 180, val);
    c = applyMoodSaturation(c);
    leds[i] = c;
  }
}

/******** OCEAN ********/
void effectOcean() {
  static unsigned long last = 0;
  static uint8_t offset = 0;
  if (!frameDueScaled(last, 55)) return;
  offset++;

  for (int i = 0; i < NUM_LEDS; i++) {
    CRGB base = CRGB(0, 80, 180);
    base = applyMoodSaturation(base);
    if (effectBrightnessAllowed()) {
      uint8_t w1 = inoise8(i * 20, offset * 3);
      uint8_t w2 = inoise8(i * 40, offset * 2);
      uint8_t v = 90 + w1 / 4 + w2 / 7;
      leds[i] = base;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = base;
      leds[i].fadeLightBy(40);
    }
  }
}

/******** SUNSET ********/
void effectSunset() {
  static unsigned long last = 0;
  static uint8_t phase = 0;
  if (!frameDueScaled(last, 60)) return;
  phase++;

  for (int i = 0; i < NUM_LEDS; i++) {
    float t = float(i) / (NUM_LEDS - 1);
    CRGB start = CRGB(255, 120, 40);
    CRGB end = CRGB(120, 40, 120);
    CRGB mix = blend(start, end, uint8_t(t * 255));
    mix = applyMoodSaturation(mix);

    if (effectBrightnessAllowed()) {
      uint8_t n = inoise8(i * 25, phase * 3);
      uint8_t v = 140 + n / 9;
      leds[i] = mix;
      leds[i].fadeLightBy(255 - v);
    } else {
      leds[i] = mix;
      leds[i].fadeLightBy(30);
    }
  }
}

/************************************************************
 * 21. FIREFLIES (NATURAL, NIGHT, CALM WINDOWS)
 ************************************************************/

// Smooth bioluminescent glow curve
float smoothPulse(float x) {
  x = constrain(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

bool isFireflyEveningWindow() {
  if (!isNight()) return false;
  if (isAnyStabilityWindow()) return false;
  if (currentMood != MOOD_CALM) return false;

  int hour, minute;
  getLocalHourMinute(hour, minute);

  int currentMinutes = hour * 60 + minute;
  int startMinutes = 19 * 60 + 30;
  int endMinutes = 22 * 60 + 30;

  if (currentMinutes < startMinutes || currentMinutes > endMinutes) return false;

  float srF = solarProgress(weather.sunrise, 60 * 60);
  float ssF = solarProgress(weather.sunset, 60 * 60);
  if (srF > 0.05f || ssF > 0.05f) return false;

  if (smoothWindSpeed > 4.0f) return false;
  if (weather.rain1h > 0.2f) return false;
  if (smoothVisibility < 3000.0f) return false;
  if (smoothCloudCover > 50.0f) return false;

  return true;
}

void overlayFirefliesLayered() {
  if (!isFireflyEveningWindow()) return;

  if (activeEffect == FX_HEAVY_RAIN || activeEffect == FX_THUNDER || activeEffect == FX_DENSE_FOG || activeEffect == FX_HAZE || activeEffect == FX_ALERT) {
    return;
  }

  static const int FIREFLY_COUNT = 6;

  struct Firefly {
    float pos;
    float phase;
    float duration;
    unsigned long startTime;
    unsigned long nextGlowAt;
    bool active;
    bool paired;
    float speed;
    int dir;
    uint8_t baseHue;
  };

  static Firefly ff[FIREFLY_COUNT];
  static bool init = false;

  if (!init) {
    init = true;
    unsigned long now = millis();
    for (int i = 0; i < FIREFLY_COUNT; i++) {
      ff[i].pos = random(NUM_LEDS);
      ff[i].phase = 0.0f;
      ff[i].duration = 0.6f + random(0, 7) * 0.1f;  // 0.6–1.2s
      ff[i].startTime = now + random(1000, 6000);
      ff[i].nextGlowAt = ff[i].startTime;
      ff[i].active = false;
      ff[i].paired = (random8() < 25);
      ff[i].speed = 0.01f + random(0, 6) * 0.003f;
      ff[i].dir = random(0, 2) ? 1 : -1;
      ff[i].baseHue = 80 + random8(10);  // yellow-green band
    }
  }

  static unsigned long lastUpdate = 0;
  if (!frameDue(lastUpdate, 30)) return;

  unsigned long now = millis();

  for (int i = 0; i < FIREFLY_COUNT; i++) {
    Firefly &f = ff[i];

    f.pos += f.dir * f.speed;
    if (f.pos < 0) f.pos += NUM_LEDS;
    if (f.pos >= NUM_LEDS) f.pos -= NUM_LEDS;

    if (!f.active && now >= f.nextGlowAt) {
      f.active = true;
      f.startTime = now;
      f.duration = 0.4f + random(0, 9) * 0.1f;  // 0.4–1.3s
      f.phase = 0.0f;

      f.nextGlowAt = now + random(3000, 8000);
      if (random8() < 20) {
        f.nextGlowAt = now + random(800, 1600);
      }
    }

    if (!f.active) continue;

    float elapsed = (now - f.startTime) / 1000.0f;
    float x = elapsed / f.duration;

    if (x >= 1.0f) {
      f.active = false;
      continue;
    }

    float pulse = smoothPulse(x);

    CHSV hsv(f.baseHue, 200, 255);
    CRGB c;
    hsv2rgb_rainbow(hsv, c);
    c = applyMoodSaturation(c);

    uint8_t centerV = (uint8_t)(pulse * 255);

    int center = (int)f.pos;
    int left1 = (center - 1 + NUM_LEDS) % NUM_LEDS;
    int right1 = (center + 1) % NUM_LEDS;
    int left2 = (center - 2 + NUM_LEDS) % NUM_LEDS;
    int right2 = (center + 2) % NUM_LEDS;

    leds[center] += c.nscale8(centerV);
    leds[left1] += c.nscale8(centerV * 0.4f);
    leds[right1] += c.nscale8(centerV * 0.4f);
    leds[left2] += c.nscale8(centerV * 0.15f);
    leds[right2] += c.nscale8(centerV * 0.15f);

    if (f.paired && pulse > 0.6f && random8() < 10) {
      int p2 = (center + (random(0, 2) ? 3 : -3) + NUM_LEDS) % NUM_LEDS;
      leds[p2] += c.nscale8(centerV * 0.5f);
    }
  }
}

/************************************************************
 * 22. EFFECT DISPATCH
 ************************************************************/

void runActiveEffect() {
  switch (activeEffect) {
    case FX_CLEAR: effectClear(); break;
    case FX_CLOUDS: effectClouds(); break;
    case FX_RAIN: effectRain(); break;
    case FX_THUNDER: effectThunder(); break;
    case FX_FOG: effectFog(); break;
    case FX_DEFAULT: effectDefault(); break;

    case FX_HEAVY_RAIN: effectHeavyRain(); break;
    case FX_GUST: effectGust(); break;
    case FX_HEATWAVE: effectHeatwave(); break;
    case FX_COLDWAVE: effectColdwave(); break;
    case FX_DENSE_FOG: effectDenseFog(); break;
    case FX_HAZE: effectHaze(); break;
    case FX_UV_HIGH: effectUVHigh(); break;
    case FX_DEW_MORNING: effectDewMorning(); break;
    case FX_PRE_RAIN: effectPreRain(); break;
    case FX_ALERT: effectAlert(); break;

    case FX_AURORA: effectAurora(); break;
    case FX_OCEAN: effectOcean(); break;
    case FX_SUNSET: effectSunset(); break;
  }
}

/************************************************************
 * 23. DIAGNOSTICS: FIREFLY DEMO (NATURAL)
 ************************************************************/

void diagnosticsFireflies(unsigned long durationMs) {
  logMsg("DIAG", "Firefly pass (natural, forest-like)");

  const int FIREFLY_COUNT = 3;

  struct Firefly {
    float pos;
    float phase;
    float duration;
    unsigned long startTime;
    unsigned long nextGlowAt;
    bool active;
    bool paired;
    float speed;
    int dir;
    uint8_t baseHue;
  };

  Firefly ff[FIREFLY_COUNT];
  unsigned long now = millis();

  for (int i = 0; i < FIREFLY_COUNT; i++) {
    ff[i].pos = random(NUM_LEDS);
    ff[i].phase = 0.0f;
    ff[i].duration = 0.6f + random(0, 7) * 0.1f;
    ff[i].startTime = now + random(500, 4000);
    ff[i].nextGlowAt = ff[i].startTime;
    ff[i].active = false;
    ff[i].paired = (random8() < 25);
    ff[i].speed = 0.01f + random(0, 6) * 0.003f;
    ff[i].dir = random(0, 2) ? 1 : -1;
    ff[i].baseHue = 80 + random8(10);
  }

  unsigned long start = millis();
  unsigned long lastUpdate = 0;

  while (millis() - start < durationMs) {
    if (millis() - lastUpdate < 30) {
      delay(5);
      continue;
    }
    lastUpdate = millis();

    fadeToBlackBy(leds, NUM_LEDS, 40);

    unsigned long t = millis();

    for (int i = 0; i < FIREFLY_COUNT; i++) {
      Firefly &f = ff[i];

      f.pos += f.dir * f.speed;
      if (f.pos < 0) f.pos += NUM_LEDS;
      if (f.pos >= NUM_LEDS) f.pos -= NUM_LEDS;

      if (!f.active && t >= f.nextGlowAt) {
        f.active = true;
        f.startTime = t;
        f.duration = 0.4f + random(0, 9) * 0.1f;
        f.phase = 0.0f;
        f.nextGlowAt = t + random(3000, 8000);
        if (random8() < 20) {
          f.nextGlowAt = t + random(800, 1600);
        }
      }

      if (!f.active) continue;

      float elapsed = (t - f.startTime) / 1000.0f;
      float x = elapsed / f.duration;

      if (x >= 1.0f) {
        f.active = false;
        continue;
      }

      float pulse = smoothPulse(x);

      CHSV hsv(f.baseHue, 200, 255);
      CRGB c;
      hsv2rgb_rainbow(hsv, c);

      uint8_t centerV = (uint8_t)(pulse * 255);

      int center = (int)f.pos;
      int left1 = (center - 1 + NUM_LEDS) % NUM_LEDS;
      int right1 = (center + 1) % NUM_LEDS;
      int left2 = (center - 2 + NUM_LEDS) % NUM_LEDS;
      int right2 = (center + 2) % NUM_LEDS;

      leds[center] += c.nscale8(centerV);
      leds[left1] += c.nscale8(centerV * 0.4f);
      leds[right1] += c.nscale8(centerV * 0.4f);
      leds[left2] += c.nscale8(centerV * 0.15f);
      leds[right2] += c.nscale8(centerV * 0.15f);

      if (f.paired && pulse > 0.6f && random8() < 10) {
        int p2 = (center + (random(0, 2) ? 3 : -3) + NUM_LEDS) % NUM_LEDS;
        leds[p2] += c.nscale8(centerV * 0.5f);
      }
    }

    FastLED.show();
  }
}

/************************************************************
 * 24. DIAGNOSTICS RUNNER
 ************************************************************/

void runDiagnosticsOnce() {
  logMsg("DIAG", "Starting diagnostics mode");

  // 1) R, G, B, W – 3s each
  CRGB testColors[4] = { CRGB::Red, CRGB::Green, CRGB::Blue, CRGB::White };
  for (int c = 0; c < 4; c++) {
    fill_solid(leds, NUM_LEDS, testColors[c]);
    FastLED.show();
    delay(3000);
  }

  // 2) Flowing colour wheel – 10s
  unsigned long startWheel = millis();
  while (millis() - startWheel < 10000) {
    uint8_t shift = (millis() / 10) & 0xFF;
    for (int i = 0; i < NUM_LEDS; i++) {
      leds[i] = CHSV((i * 8 + shift) & 0xFF, 255, 255);
    }
    FastLED.show();
    delay(20);
  }

  // 3) All effects – 15s each (raw, no overlays, no mood)
  for (int e = 0; e < diagnosticsEffectCount; e++) {
    activeEffect = diagnosticsEffects[e];
    updateEffectName();
    logMsg("DIAG", "Effect: " + activeEffectName + " running");
    unsigned long start = millis();
    while (millis() - start < 10000) {
      runActiveEffect();
      FastLED.show();
      delay(20);
    }
  }

  // 4) Dedicated firefly pass – 600s
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  diagnosticsFireflies(600000);

  logMsg("DIAG", "Diagnostics complete, holding last frame");
}

/************************************************************
 * 25. BOOT ANIMATION
 ************************************************************/

void bootAnimation() {
  logMsg("BOOT", "Running boot animation");
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::White;
    FastLED.show();
    delay(10);
  }
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Black;
    FastLED.show();
    delay(5);
  }
}

/************************************************************
 * 26. SETUP & LOOP
 ************************************************************/

void setup() {
  Serial.begin(115200);
  delay(1000);

  logMsg("BOOT", "Power on");
  logMsg("BOOT", "Climora v5.4 (Natural ambience, single bar, C1, Stability, Overlay, Mood, Natural Fireflies)");

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  initLEDStrip();
  bootAnimation();
  initWiFi();
  initTime();
  initOTA();

  fetchWeather();
  applyLightingLogic();

  if (DIAGNOSTICS_SIMULATE_DAY) {
    simMidnight = computeTodayISTMidnight();
    simStartMillis = millis();

    logMsg("SIM", "Continuous 24-hour simulation active (looping)");
    logMsg("SIM", "Sim Midnight: " + formatIST(simMidnight));
    logMsg("SIM", "Sim Sunrise : " + formatIST(weather.sunrise));
    logMsg("SIM", "Sim Sunset  : " + formatIST(weather.sunset));
  }

  logMsg("STATE", "System active");
}

void loop() {
  if (DIAGNOSTICS_MODE) {
    runDiagnosticsOnce();
    while (true) {
      delay(1000);
    }
  }

  ArduinoOTA.handle();
  updateStatusLED();
  maintainWiFi();

  if (!DIAGNOSTICS_SIMULATE_DAY && weatherUpdateDue()) {
    lastWeatherFetch = millis();
    fetchWeather();
    applyLightingLogic();
  }

  if (DIAGNOSTICS_SIMULATE_DAY) {
    applyLightingLogic();
  }

  runActiveEffect();

  float sunriseFactor = solarProgress(weather.sunrise, 60 * 60);
  float sunsetFactor = solarProgress(weather.sunset, 60 * 60);
  applySunOverlay(sunriseFactor, sunsetFactor);

  overlayFirefliesLayered();

  FastLED.show();
}