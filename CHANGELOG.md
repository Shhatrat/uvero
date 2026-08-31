# Changelog

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
