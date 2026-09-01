# Changelog

## [1.2] — 2026-09-01
### Added
- NVS persistent storage: total lifetime distance and distance since last lubrication survive reboots and firmware updates
- Lubrication reminder at 150 km (configurable via `LUB_WARN_KM`) — dashboard card turns orange when service is due
- `/reset_lubrication` endpoint resets the lubrication counter after servicing
- `/json` now includes `total_dist_km`, `lub_dist_km`, `lub_warn_km`, `lub_needs_service`
- Dashboard: two new cards — total distance and lubrication distance
- Alert banner shown at top of dashboard when lubrication is due (translates with EN/PL switcher)

## [1.1] — 2026-08-31
### Added
- Dashboard language switcher (EN/PL), preference saved in `localStorage`
- Uptime displayed as `hh:mm:ss` instead of raw seconds
- Translations extracted to `esp32/src/translations.h`, served as `/i18n.js` — easy to add new languages

## [1.0] — 2026-08-28
### Added
- Initial release: UART parser, BLE RSC broadcaster, WiFi dashboard
- Speed lookup table (31 entries, 1.1–4.0 km/h)
- Reverse-engineering docs (EN + PL)
