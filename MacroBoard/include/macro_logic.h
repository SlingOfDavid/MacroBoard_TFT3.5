#ifndef MACRO_LOGIC_H
#define MACRO_LOGIC_H

void init_macro_logic();
void execute_macro(int index);
bool is_ble_connected();
void apply_settings();

// AFK, Theme and Debug logic
bool is_afk_enabled();
void set_afk_enabled(bool enabled);
void process_afk_logic();
int get_tui_color_index();
bool is_debug_enabled();
void set_debug_enabled(bool enabled);

#endif
