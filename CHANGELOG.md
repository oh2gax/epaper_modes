# Changelog

All notable changes to the epaper_modes (APWS) sketch, most recent first.

## 2026-09-23

- **Added a warning icon for military-watchlist aircraft**: a boxed airscrew/propeller glyph, a simplified version of NATO APP-6's "fixed wing aircraft" symbol, shown for any aircraft where modes_logger's own `alert` field is `"mil"`. Reuses the same icon slot/size as the Russian star and squawk-alarm triangle. Military-flagged aircraft also now get the same priority-sort boost as squawk-alarm and Russian-flagged ones, so they're never the ones silently pushed into "+N more". If a card matches more than one of squawk-alarm/Russian/military at once, only one icon is shown, in that priority order — the others are still visible via their own flag letter in the card's text.
- **Replaced the Russian-aircraft warning triangle with a solid five-pointed star** (one point up), matching the familiar red-star symbol instead of a plain triangle. Same position, size, and vertical centering as the old triangle, so this is a pure icon-shape swap with no other layout change. The squawk-alarm icon (outlined triangle with "!") is unchanged.
- **Enlarged the Russian-flag/squawk-alarm icon** (outer radius 13px → 17px, ~30% bigger) after the star was confirmed visible on real hardware and found a bit small. The icon's right edge stays fixed against the panel margin, so it only grows toward the card text; still comfortably clear of both the worst-case line-3 text width and the card separator line below it.

## 2026-09-22

- **Added aircraft registration to the display.** Card line 1 now shows `REGISTRATION ICAO24` (e.g. `RA-73724 151FFC`), falling back to `ICAO24` alone when no registration is known. Required adding `registration` to the aircraft JSON filter and to the on-device aircraft record.
- **Moved to three-line cards, three cards per screen (was two lines, four cards).** The registration+ICAO24 pair didn't fit alongside the old single-line identity row, so cards now use: line 1 registration/ICAO24, line 2 callsign/squawk/type (moved down from line 1), line 3 the existing altitude/speed/track/flags line, unchanged. Font size stays at 12pt.
- **Added a squawk-alarm warning triangle.** A card with an active 7500/7600/7700 squawk now gets an outlined triangle with "!" in it on the card's right edge, alongside the existing full-width inverse-video banner. If a card is both squawk-alarm and Russian-flagged at once, only the squawk-alarm triangle is shown.
- **Fixed altitude rounding.** The 3-digit "hundreds of feet" altitude code (used for both current and selected/target altitude) was truncating instead of rounding, so a value like 36990ft could display as `369` instead of the intended `370`. Confirmed directly from modes_logger's source that it passes `selected_altitude` through unmodified — the jitter comes from the upstream ADS-B decode, not the server. Now rounds to the nearest hundred feet instead of truncating.
- Confirmed on real hardware with 3 simultaneous targets: the three-line cards and registration display both work as designed.
- **Added a climb/descend sign to the altitude field when there's no selected altitude.** Previously showed just `F180`/`A045` with no trend indicator in this case. Now shows `+F180`/`+A045` while climbing or `-F180`/`-A045` while descending (same ±150 ft/min deadband already used for the selected-altitude case's own sign), and still just `F180`/`A045` with no sign when level or when vertical rate is unavailable.

## 2026-09-21

- Confirmed on real hardware: boot-time IP address display, the tightened aircraft data card, and the Russian-aircraft warning triangle all tested with a live Russian target and working as designed.
- Added `README.md` and `CHANGELOG.md` ahead of publishing this sketch to GitHub as `epaper_modes`.

## 2026-09-20

### METAR/TAF scheduling and reliability
- Changed weather refresh from a rolling 30-minute timer to a fixed wall-clock grid aligned to `:00`/`:30`, so fetches always land shortly after EFHK's normal METAR publish times (`:20`/`:50`).
- Weather is now always fetched immediately when the display reverts from an aircraft-watch screen back to weather, regardless of the `:00`/`:30` grid, since a watch can run well past a grid boundary.
- Added a simple weather API health check: the on-screen timestamp now reads `YYYY-MM-DD HH:MM / API OK` or `/ API FAIL` depending on whether the most recent METAR fetch actually returned data, also logged to Serial and shown on the status page.
- **Fixed a bug where the displayed weather timestamp could read several minutes in the future.** The display was preferring AWC's `reportTime` field, which is rounded to the station's nominal reporting cycle rather than the real observation time (confirmed live: a `16:50Z` METAR reported a `reportTime` of `17:00:00Z`). Switched the primary timestamp source to `obsTime` (the actual observation epoch), keeping `reportTime` only as a fallback.

### Russian-aircraft warning triangle
- Added on-device detection of Russian-registered aircraft by ICAO24 allocation block (`0x100000`–`0x1FFFFF`), mirroring `modes_logger`'s own server-side check exactly, so the indicator works regardless of whether `modes_logger`'s own "eastern-red" toggle is enabled.
- A Russian-flagged aircraft now gets a solid warning triangle drawn on the right edge of its card.
- Added priority-tiered sorting of flagged aircraft (squawk alarm first, then Russian-flagged, then everything else) so these are never the ones silently pushed into "+N more" when more than 4 aircraft are flagged at once.
- Made the flagged-set-changed check order-independent, since cards can now be reordered by priority between polls without that alone counting as a "change" requiring an immediate redraw.

### Aircraft data card tuning
- Added `speed` and `track` to the aircraft card's second line (previously altitude only).
- Iteratively tightened the card format down to an ATC-shorthand style: `SPD` shortened to `S`, `FL` shortened to a single letter baked directly into the altitude code (`F` for flight levels, `A` for altitudes below the transition altitude), and the standalone altitude-field label removed entirely in favor of that embedded prefix.
- Final line-2 format: `<alt field> S<spd> T<track> <flags>`, e.g. `A045-020 S491 T040 M!`.
- Restored a single space before the trailing flag letters after noticing a track value could run straight into a following flag letter with nothing between them (e.g. `T254E` read as one ambiguous token); now reads `T254 E`.
- Resized the Russian-flag warning triangle larger (half-size 10px → 13px) to use the width freed up by the label removal.

### Startup
- Added a one-time display of the device's IP address under "APWS ONLINE" on the boot screen only (not shown on later idle-screen reverts), so the device's address is readable off the panel without needing a Serial connection.
