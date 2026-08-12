#include <Arduino.h>
#include <lvgl.h>
#include <nvs_flash.h>
#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"

#include "macro_screen.h"
#include "macro_logic.h"
#include "web_server.h"

#define LVGL_PORT_ROTATION_DEGREE (90)

void setup()
{
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0); // Prevent USB CDC from blocking threads if the host port is busy
    if (Serial) Serial.println("MacroBoard starting...");

    // Initialize NVS for settings storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (Serial) Serial.println("Initialize panel device");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192; // Increase stack size to 8KB to prevent stack overflow
    port_cfg.task_affinity = 1; // Pin display UI task to Core 1 (keeps Core 0 free for BLE/WiFi)

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = port_cfg,
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

    if (Serial) Serial.println("Create UI");
    /* Lock the mutex due to the LVGL APIs are not thread-safe */
    bsp_display_lock(0);

    // Setup our custom macro buttons
    setup_macro_screen();

    /* Release the mutex */
    bsp_display_unlock();

    if (Serial) Serial.println("Initializing Macro Logic");
    init_macro_logic();

    if (Serial) Serial.println("Initializing MicroSD Card");
    init_sd_card();

    if (Serial) Serial.println("MacroBoard initialized successfully!");
}

extern bool should_reboot;

void loop()
{
    process_afk_logic();
    process_typer_logic();
    process_hybrid_logic();
    delay(50);
    if (should_reboot) {

        delay(1000); // Allow HTTP response to finish transmitting
        ESP.restart();
    }
}

