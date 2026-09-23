# epaper_modes

Aircraft Proximity Warning System (APWS) — an ESP32 + e-paper display that shows aircraft currently flagged by a [modes_logger](https://github.com/oh2gax/modes_logger) ADS-B receiver, and falls back to a local METAR/TAF weather display when nothing is flagged.

The device has no aircraft list of its own. `modes_logger` decides what's worth showing — a match on one of its own mil/gov/civ watchlists, or a live emergency squawk (7500/7600/7700) — and reports it per-aircraft over a small JSON API. The display just polls that API and shows whatever comes back flagged.

## Overview

- Polls `modes_logger`'s `/api/liveflights` endpoint every 10 seconds.
- When one or more aircraft are flagged, draws them as compact three-line "cards" (registration, ICAO24, callsign, squawk, type, altitude, speed, track, and flag letters).
- A Russian-registered aircraft (by ICAO24 allocation block) additionally gets a solid five-pointed star icon, detected independently on-device so it doesn't depend on any server-side toggle.
- An aircraft matched by modes_logger's own military watchlist gets a boxed airscrew icon (a simplified NATO-style "fixed wing aircraft" symbol).
- An active emergency squawk (7500/7600/7700) draws a full-width inverse-video banner, forces an immediate redraw, and also puts an outlined warning-triangle-with-"!" icon on that aircraft's own card.
- With nothing flagged, shows the current EFHK METAR/TAF (03:00–21:00 UTC) or a plain "APWS ONLINE" idle screen at night, refreshed on a fixed :00/:30 wall-clock grid.
- Runs a small read-only web status page (WiFi signal, free heap, last-fetch status, weather API health) plus a `/restart` endpoint.
- Built for long unattended uptime: task watchdog with self-restart, WiFi auto-reconnect, explicit HTTP timeouts, and a single reused JSON buffer to keep heap fragmentation down over weeks/months.

## Hardware

- **MCU:** ESP32 Dev Board (Arduino core, WiFi built in).
- **Display:** 4.2" monochrome e-paper panel, 400×300px, driven via [GxEPD2](https://github.com/ZinggJM/GxEPD2) (`GxEPD2_420`).
- **Wiring** (as configured in the sketch):

  | Signal | ESP32 pin |
  |--------|-----------|
  | CS     | 22        |
  | DC     | 15        |
  | RST    | 13        |
  | BUSY   | 34        |

- No other peripherals — WiFi is the only connection to the outside world (to the `modes_logger` host and to `aviationweather.gov` for METAR/TAF).

## The display, in use

- **Boot:** "APWS ONLINE" full-screen, with the device's IP address shown underneath for a few seconds (once, at startup only — handy for finding it on the LAN without opening Serial).
- **Idle / daytime, nothing flagged:** current EFHK METAR and TAF, word-wrapped, with a timestamp line at the bottom reading `YYYY-MM-DD HH:MM / API OK` (or `API FAIL` if the last weather fetch attempt didn't get data back). The timestamp is the METAR's real observation time, not a rounded reporting-cycle label.
- **Idle / nighttime (21:00–03:00 UTC), nothing flagged:** plain "APWS ONLINE" screen — no weather is fetched outside the daytime window.
- **One or more aircraft flagged:** each aircraft gets a three-line card:
  - line 1 — `REGISTRATION ICAO24` (e.g. `RA-73724 151FFC`), or just `ICAO24` if no registration is known
  - line 2 — `CALLSIGN SQUAWK TYPE`
  - line 3 — altitude/speed/track/flags in ATC shorthand, e.g. `A045-020 S491 T040 M!`
    - the altitude field is prefixed `F` (flight level) at/above 5000ft or `A` (altitude) below it, followed by `+`/`-`/`=` for climb/descend/level and the selected/target altitude if the aircraft has one dialed in (rounded to the nearest hundred feet)
    - `S<speed>` in knots, `T<track>` as a 3-digit heading
    - trailing flag letters: `M`/`G`/`C`/`E` for a mil/gov/civ/eastern watchlist match, `!` for an active emergency squawk
  - a Russian-registered aircraft gets a solid five-pointed star on the right edge of its card, a military-watchlist match gets a boxed airscrew icon, and a squawk-alarm aircraft gets an outlined triangle with "!" instead — only one icon per card if more than one applies, in that priority order (squawk alarm, then Russian, then military); the others are still visible via their own flag letter in the card's text
  - up to 3 cards fit on screen at once; anything beyond that is summarized as "+N more" — squawk-alarm, Russian-flagged, and military-flagged aircraft are always sorted to the top first, in that order, so they're never the ones left out
  - a squawk alarm draws an inverse-video "!! SQUAWK ALARM !!" banner across the top instead of the normal header
  - after the last flagged aircraft has been gone for 60 seconds, the display reverts to the weather or idle screen

## modes_logger data source

Repo: https://github.com/oh2gax/modes_logger

```
GET http://<your-modes_logger-host>:5000/api/liveflights
```

Returns the current snapshot of all tracked aircraft; the sketch filters the response down to just the fields it uses (`icao24`, `registration`, `callsign`, `squawk`, `type`, `altitude`, `speed`, `vertical_rate`, `selected_altitude`, `track`, `alert`, `squawk_alarm`). To watch a specific aircraft that isn't already on an official watchlist, add it through `modes_logger`'s own `/admin` page — there's nothing to configure on the display side.

## Building and flashing

**Board:** any ESP32 dev board, Arduino core (tested with arduino-esp32 3.x / ESP-IDF v5, also supports 2.x / IDF4).

**Libraries** (install via Arduino Library Manager unless noted):
- [GxEPD2](https://github.com/ZinggJM/GxEPD2)
- [ArduinoJson](https://arduinojson.org/) (v6)
- [NTPClient](https://github.com/arduino-libraries/NTPClient)
- `WiFi`, `WiFiClientSecure`, `WiFiUdp`, `HTTPClient`, `WebServer`, `esp_task_wdt` — bundled with the ESP32 Arduino core

**Repo layout:**
- `epaper_modes.ino` — the sketch
- `secrets.h.example` — template for your WiFi credentials; copy to `secrets.h` (gitignored) and fill in your own values
- `.gitignore` — keeps `secrets.h` and build artifacts out of version control

**WiFi credentials:** kept out of the sketch and out of git. Copy `secrets.h.example` to `secrets.h` in the same sketch folder and fill in your own SSID/password:

```cpp
#define SECRET_WIFI_SSID     "your-wifi-ssid"
#define SECRET_WIFI_PASSWORD "your-wifi-password"
```

`secrets.h` is listed in `.gitignore`, so it stays local — only `secrets.h.example` is committed. The sketch includes it and reads `SECRET_WIFI_SSID` / `SECRET_WIFI_PASSWORD` at the top of the file.

**Other configuration** — edit these near the top of `epaper_modes.ino` before flashing:
- `AIRCRAFT_API_URL` — your `modes_logger` instance's `/api/liveflights` URL
- `DAYTIME_START_HOUR` / `DAYTIME_END_HOUR` — UTC window for showing weather instead of the idle screen (defaults suit EFHK's traffic hours)
- METAR/TAF station is hardcoded to EFHK in `fetchWeatherData()` — change the `ids=EFHK` query parameter to use a different station

Flash normally from the Arduino IDE (Tools → Board → your ESP32 board, then Sketch → Upload). On first boot the device connects to WiFi, starts NTP, and shows its IP address on screen.

## Status page

While running, the device serves a minimal status page at `http://<device-ip>/`:
- WiFi RSSI, free heap (current + minimum seen)
- number of aircraft currently flagged
- time since last successful aircraft-feed fetch, and consecutive-failure count
- weather API health (`OK`/`FAIL`) and the last fetch attempt's time slot
- a `/restart` button

## Stability design

- ESP32 task watchdog (30s) — resets the device if `loop()` ever stalls
- self-restart after ~5 minutes of consecutive aircraft-feed fetch failures
- WiFi auto-reconnect with backoff, `WiFi.setSleep(false)` to avoid modem-sleep induced latency
- explicit connect/read timeouts on every HTTP request
- aircraft JSON is streamed directly from the HTTP response into a single reused `DynamicJsonDocument`, rather than buffered into a `String` and re-parsed each poll, to avoid heap fragmentation over long (weeks/months) uptime
- periodic free-heap logging to Serial for field diagnostics

## Credits

- Aircraft data: [modes_logger](https://github.com/oh2gax/modes_logger) by OH2GAX
- Weather data: [aviationweather.gov Data API](https://aviationweather.gov/data/api/)
- E-paper driver: [GxEPD2](https://github.com/ZinggJM/GxEPD2)
