# ESP32-S3 MacroBoard (JC3248W535C)

This repository contains the source code for the BLE Macro Board using the 3.5-inch Capacitive Touch Screen.

## Critical Hardware Discoveries & Documentation

Through extensive debugging, we discovered that the "Cheap Yellow Display" (CYD) 3.5-inch board comes in two vastly different hardware variants. The initial SDK provided was for the older ESP32 WROOM, which caused instant boot-loops (`SPI_FAST_FLASH_BOOT`) when compiled for the newer S3.

This codebase is specifically tailored for the **ESP32-S3 Variant (JC3248W535C)**. 

### Hardware Specifications
*   **Microcontroller:** ESP32-S3
*   **Flash Memory:** 16MB (Must be configured as **DIO** or **QIO 80MHz** depending on eFuses)
*   **PSRAM:** 8MB Embedded (**OPI PSRAM** is absolutely required. Using QSPI will crash the bootloader).
*   **Display Driver:** **AXS15231B** over QSPI (This chip combines both the display controller and the capacitive touch controller).
*   **Touch Controller:** AXS15231B over I2C (SCL=8, SDA=4).

### Software Architecture (Why LovyanGFX failed)
The ESP32-S3 variant of this board utilizes a high-speed QSPI bus for the display. The popular `LovyanGFX` and `TFT_eSPI` libraries do not natively support the AXS15231B QSPI driver out of the box. 

Instead, this project uses the native **ESP-IDF LCD framework (`esp_lcd`)** provided by Espressif. The manufacturer provided a Board Support Package (`esp_bsp.c`) that initializes the screen and touch drivers and passes them directly to LVGL.

### PlatformIO Configuration Requirements
Because the `esp_lcd` QSPI and touch drivers rely on ESP-IDF v5.1 APIs, this project **REQUIRES Arduino Core V3.0+**. 
Since PlatformIO officially defaults to Arduino Core V2.0.x, our `platformio.ini` uses the community `pioarduino` fork to pull in the V3 framework:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.11/platform-espressif32.zip
```

### SDK Source
The correct SDK and datasheet for the ESP32-S3 JC3248W535C board were obtained from the manufacturer's provided link on the AliExpress product page:
`http://pan.jczn1688.com/directlink/1/HMI%20display/JC3248W535EN.zip`

---

## Project Structure
*   **`src/esp_bsp.c` / `.h`**: Manufacturer's hardware initialization wrapper (SPI bus, display IO, touch interrupt).
*   **`src/esp_lcd_axs15231b.c`**: Native ESP-IDF driver for the AXS15231B display controller.
*   **`src/lv_port.c`**: Connects the `esp_lcd` panel to LVGL, handles flush callbacks and touch inputs.
*   **`src/main.cpp`**: Bootstraps the hardware, initializes the UI, and starts the Web Server and Bluetooth Macro logic.
*   **`src/macro_logic.cpp`**: Emulates a BLE HID Keyboard to trigger PC macros.
*   **`src/web_server.cpp`**: Hosts a captive portal AP for configuring Wi-Fi and the 8 Macro slots.
*   **`src/macro_screen.cpp`**: Binds the SquareLine Studio UI to our C++ execution callbacks.

## Setup & Flashing
1. Open this project in VSCode + PlatformIO.
2. Connect the board via the **USB** port (not the UART/COM port if separate, though USB CDC handles both flashing and serial).
3. Wait for PlatformIO to download the `pioarduino` framework.
4. Click **Upload**.

---

## Macro Configuration & Chords

The MacroBoard supports simulated simultaneous key combinations (Chords). When configuring slot actions, separate individual keys with a `+` symbol (e.g. `Ctrl + Shift + a`).

### Supported Modifiers
- `Ctrl` / `Control`
- `Shift`
- `Alt`
- `Gui` / `Win` / `Cmd`

### Supported Special Keys
- `Enter` / `Return`
- `Esc` / `Escape`
- `Tab`
- `Backspace`
- `Delete` / `Del`
- `Insert` / `Ins`
- `Home`
- `End`
- `PageUp` / `PgUp`
- `PageDown` / `PgDn`
- `Up` / `Down` / `Left` / `Right`
- `F1` to `F24`

### Language Layouts & Screensaver
- **Layouts:** Use the settings dropdown (on screen or web page) to select the correct layout corresponding to your PC OS keyboard layout: `en-gb` (UK English), `en-us` (US English), or `pt-br` (Brazilian ABNT2).
- **Screensaver:** If enabled (1, 5, or 10 minutes), the screen transitions to a blinking typewriter terminal cursor screensaver. Tap the screen at any time to wake the device.

