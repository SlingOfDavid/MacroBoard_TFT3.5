# MacroBoard Refactor Plan

## Goal Description
Strip away all the bulky manufacturer "OS" UI code and assets from the `ui.c` files. Rebuild a clean, minimal UI using LVGL directly, focused entirely on the 8 macro buttons and the "Start AP" functionality. Fix the bug preventing the Access Point from actually starting.

## User Review Required
> [!WARNING]
> This plan will DELETE `ui.c`, all its image assets, and related auto-generated SquareLine files to save space and simplify the project. Do you approve wiping out the existing demo UI?

## Proposed Changes

### UI & Core
- `main.cpp`: Remove references to SquareLine UI `ui_init()` and instead initialize our custom Macro UI.
- `ui.c` / `ui.h` / `ui_events.c` / etc: DELETE.
- `ui_img_*.c` (assets): DELETE.

### Custom Macro UI
- Create `src/macro_ui.cpp` and `include/macro_ui.h` to hold a simple grid of 8 LVGL buttons.

### Web Server & WiFi
- `web_server.cpp`: Add `WiFi.mode(WIFI_AP);` before `WiFi.softAP()` to fix the AP not starting issue.

## Verification Plan
1. Compile via PlatformIO to ensure all deleted file references are removed from `CMakeLists.txt` or PIO build system.
2. Flash the device.
3. Verify 8 macro buttons appear.
4. Click "Start Web Config AP" and verify the `MacroBoard-Config` network appears.
