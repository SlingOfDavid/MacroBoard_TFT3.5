#ifndef MACRO_LOGIC_H
#define MACRO_LOGIC_H

#include <Arduino.h>
#include <vector>

// SneaKeyboard (LittleFS + PSRAM file storage and full-speed BLE typing)
struct SneakyFileInfo {
    String name;
    size_t size;
};

void init_macro_logic();
void execute_macro(int index);
bool is_ble_connected();
void pause_ble_for_wifi();
void resume_ble_after_wifi();
void apply_settings();

// AFK, Typer, Theme and Debug logic
bool is_afk_enabled();
void set_afk_enabled(bool enabled);
void process_afk_logic();

// Typer Mode and MicroSD logic
bool init_sd_card();
bool is_sd_card_mounted();
bool is_typer_enabled();
void set_typer_enabled(bool enabled);
void process_typer_logic();

// Hybrid Mode logic
bool is_hybrid_enabled();
void set_hybrid_enabled(bool enabled);
void process_hybrid_logic();

bool is_automated_mode_active();

int get_tui_color_index();
bool is_debug_enabled();
void set_debug_enabled(bool enabled);

// SneaKeyboard filesystem and playback API
bool init_sneaky_fs();
bool is_sneaky_fs_mounted();
void refresh_sneaky_fs();
size_t get_sneaky_fs_total_bytes();
size_t get_sneaky_fs_used_bytes();
std::vector<SneakyFileInfo> get_sneaky_files();
void notify_sneaky_http_activity(bool upload_active);
bool queue_sneaky_file_write(const String& filename, uint8_t* data, size_t len);
bool delete_sneaky_file(const String& filename);
void trigger_sneaky_paste_file(const String& filename);
void trigger_sneaky_paste_all();
void stop_sneaky_paste();
bool is_sneaky_paste_active();
void process_sneaky_paste_logic();

#endif


