#ifndef MACRO_SCREEN_H
#define MACRO_SCREEN_H

#include <lvgl.h>

void setup_macro_screen();
void execute_macro(int index);
void set_screensaver_timeout(uint32_t timeout_ms);
// Signal the LVGL UI loop to rebuild the SneaKeyboard file list
void request_sneaky_ui_refresh();

#endif // MACRO_SCREEN_H
