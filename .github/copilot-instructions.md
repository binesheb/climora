# Climora v5.4: AI Coding Agent Guidelines

## Project Overview
**Climora** is a real-time ambient LED lighting engine for Arduino ESP32 that translates live weather data into dynamic visual effects. A single 30-LED WS2812B strip behind a monitor displays a continuously evolving ambience driven by OpenWeatherMap API, sunrise/sunset cycles, and solar tint overlays.

## Architecture: Multi-Layer Composition

The system operates as **4 independent visual layers** that blend together each frame:

1. **Weather Effect Layer** (Primary) – 19 distinct effect modes (Clear, Rain, Thunder, Haze, etc.) selected by `classifyEffectFromWeather()` with 10-minute hysteresis
2. **Solar Overlay Layer** – Sunrise/sunset color tints applied via `applySunOverlay()` (warm oranges, purples) over 1-hour windows
3. **Firefly Layer** – Rare bioluminescent accents overlay only in calm night conditions (wind < 4 m/s, clear skies, MOOD_CALM)
4. **Brightness Layer** – Smooth (0.05s stability windows, 0.15s normal) day/night transitions responsive to cloud cover

**No effects directly modify `leds[]` during rendering**—they fade/blend/overlay. Always use `FastLED.show()` at end of loop.

## Key Patterns & Conventions

### Weather State & Smoothing
- Raw weather fields stored in `WeatherData` struct (temp, humidity, windSpeed, cloudCover, etc.)
- **Always use smoothed versions** in effect logic: `smoothCloudCover`, `smoothWindSpeed`, `smoothPOP`, `smoothVisibility` (exponential moving average, alpha=0.1)
- Effects respond to smoothed data; raw data drives effect classification

### Time & Solar Events
- **IST (UTC+5:30) throughout**. All times converted via `applyC1SunEvents()` from UTC sunrise/sunset
- `computeTodayISTMidnight()` anchors daily cycles
- `isNight()` checks actual sunrise/sunset; fallback to 6–19 hour window if weather data missing
- `solarProgress(eventTime, windowSeconds)` returns 0–1 fade factor for sunrise/sunset overlays

### Effect Loop Pattern
Every effect follows this structure (see `effectRain()`, `effectThunder()` etc.):
```cpp
void effectXXX() {
  static uint8_t last = 0;
  if (!frameDueScaled(last, baseIntervalMs)) return;  // Motion-scaled frame gating
  
  // 1. Check effectBrightnessAllowed() to apply stability window (bright animation vs. steady glow)
  // 2. Use smoothed weather, solarDayFactor(), moodSaturation to parameterize colors
  // 3. Never fill leds[] = pure color; use blend(), fadeLightBy(), or += for layering
  // 4. Apply applyMoodSaturation() to all colors
}
```

### Stability Windows (Motion Suppression)
- **Morning Window**: 45 min before sunrise to 90 min after (smooth transitions for waking)
- **Night Window**: 30 min before sunset to 60 min after (evening stabilization)
- `isMorningStabilityWindow()`, `isNightStabilityWindow()` check real solar times or fallback clocks
- **Effect behavior during windows**: `effectBrightnessAllowed()` returns false → effects display static glow (no perlin noise, no temporal animation)

### Mood Governor (Affective Tuning)
Updates every main loop via `updateMood()` based on conditions:
- **MOOD_CALM**: Night + low wind (< 4 m/s) + clear (vis > 4000 m) → `moodMotionScale = 0.6` (slower), `moodSaturation = 0.85`, `moodOverlayScale = 1.1`
- **MOOD_ENERGETIC**: Day + clouds < 50% + wind 3–9 m/s → `moodMotionScale = 1.3` (faster), `moodSaturation = 1.15`
- **MOOD_DRAMATIC**: Rain > 3 mm or wind > 11 m/s → `moodSaturation = 1.2`, `moodOverlayScale = 1.2`
- **MOOD_NEUTRAL**: Default, all scales = 1.0

All effect animations scaled by `moodMotionScale` via `frameDueScaled()`.

### Effect Hysteresis (Mode Stability)
`applyEffectHysteresis()` prevents flickering:
1. Classify new effect via `classifyEffectFromWeather()`
2. Pending effect enters buffer until stable for **10 minutes** (`2 * WEATHER_INTERVAL`)
3. Only then commit to `activeEffect` and log change

**Do not modify effect selection logic directly**—add classification conditions to `classifyEffectFromWeather()` only.

## Critical Developer Workflows

### Testing & Simulation
```cpp
#define DIAGNOSTICS_MODE          true   // Static diagnostic tests (R/G/B, wheel, all 19 effects)
#define DIAGNOSTICS_SIMULATE_DAY  true   // Continuous 24-hour sim (looping, 240x speedup)
const float SIM_SPEED = 240.0f;           // 1 real second = 4 simulated minutes
```
- Enable simulation to verify solar overlays, stability windows, mood transitions without waiting 24 hours
- `runDiagnosticsOnce()` cycles through all effects for 15s each—use for regression testing

### OTA & Connectivity
- WiFi/OTA handled automatically; effects gracefully degrade if internet lost (`internetOK = false` → FX_DEFAULT)
- `updateStatusLED()` blinks pattern indicates state (solid=online, 200ms blink=WiFi-only, 600ms=disconnected)
- Weather fetch blocks loop only ~200ms; non-blocking by design

### Adding New Effects
1. Create `void effectMyEffect()` following the loop pattern above
2. Add case to `EffectMode` enum and `runActiveEffect()` dispatcher
3. Add classification rule to `classifyEffectFromWeather()` (must return enum value)
4. Add string name to `updateEffectName()` for logging
5. Add to `diagnosticsEffects[]` array for diagnostics pass

## Integration Points & Dependencies

| Component | Library | Notes |
|-----------|---------|-------|
| LED Control | FastLED 3.x | WS2812B on pin 5; max 2A, 60 fps limit |
| Weather | OpenWeatherMap API (Current Weather) | lat/lon query, 5-min fetch interval, ~4KB JSON response |
| Time Sync | NTP (pool.ntp.org) | IST offset +5:30, 20-retry boot handshake |
| OTA Updates | ArduinoOTA | Non-blocking in loop, `FastLED.clear()` on start |
| JSON | ArduinoJson 6.x | 4096-byte StaticJsonDocument for weather parse |

## Common Pitfalls & Anti-Patterns

❌ **Direct `leds[i] = CRGB(r,g,b)`** in rendering—overwrites prior layers  
✅ Use `leds[i] = blend(existing, new, alpha)` or `leds[i] += color` for compositing

❌ **Ignoring smoothed weather in effects** (using raw `weather.cloudCover` directly)  
✅ Always use `smoothCloudCover` for animations; raw only for classification thresholds

❌ **Hardcoding times** (6 AM, 7 PM) instead of sunrise/sunset  
✅ Use `solarProgress()`, `isNight()` to adapt to actual location & season

❌ **Adding high-frequency animation during stability windows** (breaks UX)  
✅ Check `effectBrightnessAllowed()` → reduce perlin shifts, increase static hold

❌ **Modifying `leds[]` after `FastLED.show()` or before final overlay pass**  
✅ All edits must complete before `applySunOverlay()` and firefly layering; `show()` only at loop end

## Key Files & Reference Lines

- [climora_effects.ino](climora_effects.ino#L25-L75): Config, global state, effect enum
- [classifyEffectFromWeather()](climora_effects.ino#L800-L833): Effect selection logic
- [effectRain()](climora_effects.ino#L1083-L1120): Template pattern for procedural effects
- [applyLightingLogic()](climora_effects.ino#L1210-L1280): Main brightness & mood driver
- [overlayFirefliesLayered()](climora_effects.ino#L1675-L1800): Rare calm-window enhancement

