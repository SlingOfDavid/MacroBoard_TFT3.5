#ifndef MACRO_SCREEN_H
#define MACRO_SCREEN_H

#include <lvgl.h>

void setup_macro_screen();
void execute_macro(int index);
void set_screensaver_timeout(uint32_t timeout_ms);

#endif // MACRO_SCREEN_H
