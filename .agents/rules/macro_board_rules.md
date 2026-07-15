---
trigger: always_on
---

# MacroBoard Project Rules & Quirks

## Build Environment (PlatformIO)
- **Windows esptool bug**: Must use `platform_packages = tool-esptoolpy@~2.40900.0` in `platformio.ini` to avoid python execution errors on Windows with `pioarduino`.
- **Arduino Core V3**: Using ESP32 Core V3 breaks older `BleKeyboard` libraries. Specifically, `BLECharacteristic::setValue(std::string&)` no longer exists. Use `setValue(str.c_str())` instead.
- **LVGL Linker Collisions**: Never include the manufacturer's pre-compiled objects or duplicate `lvgl` folders. Ensure `LV_TICK_CUSTOM` matches across `lv_conf.h` and the platform code (currently `LV_TICK_CUSTOM 0`).

## Codebase Strategy
- **Minimalist Approach**: The manufacturer's `ui.c` and associated assets are bloated with a fake OS demo. We are stripping all of it in favor of a clean, single-screen macro board.
- **WiFi AP Issue**: Calling `WiFi.softAP()` silently fails if WiFi mode is not initialized. Must call `WiFi.mode(WIFI_AP)` (or APSTA) before `WiFi.softAP()`.

## Future Work
- Do work on separate branches.
- Keep macro logic separate from UI logic (`macro_logic.cpp`, `web_server.cpp`).

## General
- After a modification is done or plan is implemented do not create a walkthrough unless specifically asked to
