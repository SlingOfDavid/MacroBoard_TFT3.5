#include <Arduino.h>
#include <lvgl.h>
#include <nvs_flash.h>
#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"

// Our Macro logic headers
#include "ui.h"
#include "macro_screen.h"
#include "macro_logic.h"
#include "web_server.h"

#define LVGL_PORT_ROTATION_DEGREE (90)

void setup()
{
    Serial.begin(115200);
    Serial.println("MacroBoard starting...");

    // Initialize NVS for settings storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    Serial.println("Initialize panel device");
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
#if LVGL_PORT_ROTATION_DEGREE == 90
        .rotate = LV_DISP_ROT_90,
#elif LVGL_PORT_ROTATION_DEGREE == 270
        .rotate = LV_DISP_ROT_270,
#elif LVGL_PORT_ROTATION_DEGREE == 180
        .rotate = LV_DISP_ROT_180,
#elif LVGL_PORT_ROTATION_DEGREE == 0
        .rotate = LV_DISP_ROT_NONE,
#endif
    };

    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    Serial.println("Create UI");
    /* Lock the mutex due to the LVGL APIs are not thread-safe */
    bsp_display_lock(0);

    // Initialize the SquareLine Studio UI
    ui_init();
    
    // Setup our custom macro buttons over the UI
    setup_macro_screen();

    /* Release the mutex */
    bsp_display_unlock();

    Serial.println("Initializing Macro Logic & Web Server");
    init_macro_logic();
    start_web_server();

    Serial.println("MacroBoard initialized successfully!");
}

void loop()
{
    // The ESP-BSP port handles lv_timer_handler in a background FreeRTOS task.
    // ESPAsyncWebServer runs asynchronously.
    // BLE Keyboard runs asynchronously.
    delay(1000);
}
