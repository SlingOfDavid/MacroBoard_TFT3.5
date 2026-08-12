#include "macro_logic.h"
#include <BleKeyboard.h>
#include <Preferences.h>
#include <Arduino.h>
#include <esp_mac.h>
#include <SD_MMC.h>
#include <FS.h>
#include <vector>

#define SD_MMC_CLK 12
#define SD_MMC_CMD 11
#define SD_MMC_D0 13

BleKeyboard bleKeyboard("MacroBoard ESP32", "Espressif", 100);
Preferences preferences;

#include "macro_screen.h"


// We will store MAC address here to use as XOR key
uint8_t mac_addr[6];

static bool afk_enabled = false;
static uint32_t next_afk_trigger_ms = 0;
static int tui_color_index = 0;
static bool debug_enabled = false; // Default: false (Disabled)

static bool sd_mounted = false;
static bool typer_enabled = false;
static uint32_t next_typer_char_ms = 0;
static std::vector<String> typer_files;
static File current_typer_file;

// Typo state machine for Typer Mode
static bool typo_in_progress = false;
static char typo_original_char = 0;
static int typo_state = 0; // 1: sent wrong char, 2: sent backspace

bool is_ble_connected() {
    return bleKeyboard.isConnected();
}

bool is_afk_enabled() {
    return afk_enabled;
}

bool is_typer_enabled() {
    return typer_enabled;
}

bool is_sd_card_mounted() {
    return sd_mounted;
}

bool is_debug_enabled() {
    return debug_enabled;
}

void set_debug_enabled(bool enabled) {
    debug_enabled = enabled;
}

void set_afk_enabled(bool enabled) {
    afk_enabled = enabled;
    if (afk_enabled) {
        if (typer_enabled) {
            typer_enabled = false;
        }
        // ponytail: schedule first trigger between 1000ms and 28000ms from now
        next_afk_trigger_ms = millis() + random(1000, 28001);
        if (debug_enabled) Serial.println("AFK Mode ENABLED");
    } else {
        if (debug_enabled) Serial.println("AFK Mode DISABLED");
    }
}

static void scan_typer_dir() {
    typer_files.clear();
    if (!sd_mounted) return;

    // Create /typer directory if it does not exist to avoid VFS open error logs
    if (!SD_MMC.exists("/typer")) {
        SD_MMC.mkdir("/typer");
    }

    File dir = SD_MMC.open("/typer");
    if (!dir || !dir.isDirectory()) {
        dir = SD_MMC.open("/");
    }
    if (!dir || !dir.isDirectory()) return;

    File file = dir.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String name = file.path();
            if (name.endsWith(".txt") || name.endsWith(".TXT")) {
                typer_files.push_back(name);
            }
        }
        file.close();
        file = dir.openNextFile();
    }

    dir.close();
}


void set_typer_enabled(bool enabled) {
    typer_enabled = enabled;
    if (typer_enabled) {
        if (afk_enabled) {
            afk_enabled = false;
        }
        if (current_typer_file) {
            current_typer_file.close();
        }
        typo_in_progress = false;
        scan_typer_dir();
        next_typer_char_ms = millis() + 500;
        if (debug_enabled) Serial.println("Typer Mode ENABLED");
    } else {
        if (current_typer_file) {
            current_typer_file.close();
        }
        typo_in_progress = false;
        if (debug_enabled) Serial.println("Typer Mode DISABLED");
    }
}

bool init_sd_card() {
    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    // Minimal single boot log to avoid serial buffer overflow
    if (!SD_MMC.begin("/sdmmc", true, false, 20000)) {
        sd_mounted = false;
        Serial.println("[SD] Card Mount Failed");
        return false;
    }

    sd_mounted = true;
    scan_typer_dir();
    uint64_t total_mb = SD_MMC.cardSize() / (1024 * 1024);
    Serial.printf("[SD] Mounted OK. Type: %d, Size: %lluMB, Typer Files: %u\n",
                  (int)SD_MMC.cardType(), (unsigned long long)total_mb, (unsigned int)typer_files.size());
    return true;
}


int get_tui_color_index() {
    return tui_color_index;
}

void process_afk_logic() {
    // ponytail: list of safe, non-destructive keys (Shift, Ctrl, Alt, Arrows, Page Up/Down)
    static const uint8_t non_destructive_keys[] = {
        KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT,
        KEY_LEFT_CTRL,  KEY_LEFT_ALT,
        KEY_UP_ARROW,   KEY_DOWN_ARROW,
        KEY_LEFT_ARROW, KEY_RIGHT_ARROW,
        KEY_PAGE_UP,    KEY_PAGE_DOWN
    };

    if (!afk_enabled || !bleKeyboard.isConnected()) return;

    uint32_t now = millis();
    if (now >= next_afk_trigger_ms) {
        // Schedule next keypress randomly between 1 and 28 seconds
        next_afk_trigger_ms = now + random(1000, 28001);

        int key_idx = random(0, sizeof(non_destructive_keys) / sizeof(non_destructive_keys[0]));
        uint8_t key = non_destructive_keys[key_idx];
        bleKeyboard.press(key);
        delay(20);
        bleKeyboard.releaseAll();

        if (debug_enabled) Serial.printf("AFK key press 0x%02X sent. Next in %u ms\n", key, (unsigned int)(next_afk_trigger_ms - now));
    }
}

void process_typer_logic() {
    if (!typer_enabled || !bleKeyboard.isConnected() || !sd_mounted) return;

    uint32_t now = millis();
    if (now < next_typer_char_ms) return;

    // Handle typo state machine if typo in progress
    if (typo_in_progress) {
        if (typo_state == 1) {
            // Sent wrong character previously, now send backspace
            bleKeyboard.write(KEY_BACKSPACE);
            typo_state = 2;
            next_typer_char_ms = now + random(100, 251); // short pause before typing correct char
            return;
        } else if (typo_state == 2) {
            // Typed backspace, now type correct character
            bleKeyboard.write(typo_original_char);
            typo_in_progress = false;
            typo_state = 0;
            
            // Calculate delay for next regular char
            if (typo_original_char == '\n') {
                next_typer_char_ms = now + random(800, 2001);
            } else if (typo_original_char == '.' || typo_original_char == ',' || typo_original_char == ';' || typo_original_char == '!' || typo_original_char == '?') {
                next_typer_char_ms = now + random(200, 501);
            } else {
                next_typer_char_ms = now + random(40, 161);
            }
            return;
        }
    }

    // Open file if not currently open or at EOF
    if (!current_typer_file || !current_typer_file.available()) {
        if (current_typer_file) {
            current_typer_file.close();
        }

        if (typer_files.empty()) {
            scan_typer_dir();
        }

        if (typer_files.empty()) {
            // No files found, retry in 5 seconds
            next_typer_char_ms = now + 5000;
            return;
        }

        int file_idx = random(0, typer_files.size());
        current_typer_file = SD_MMC.open(typer_files[file_idx].c_str(), "r");
        if (!current_typer_file) {
            if (debug_enabled) Serial.printf("Failed to open typer file %s\n", typer_files[file_idx].c_str());
            next_typer_char_ms = now + 3000;
            return;
        }
        if (debug_enabled) Serial.printf("Typer file selected: %s\n", typer_files[file_idx].c_str());
    }

    int b = current_typer_file.read();
    if (b == -1) { // End of File reached
        current_typer_file.close();
        // Rest delay between 5 to 15 seconds before opening next random file
        next_typer_char_ms = now + random(5000, 15001);
        if (debug_enabled) Serial.println("Typer finished reading file, resting.");
        return;
    }

    char c = (char)b;

    // ~3% chance of typo on standard ASCII letters
    if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) && random(0, 100) < 3) {
        typo_in_progress = true;
        typo_original_char = c;
        typo_state = 1;

        // Type a wrong character adjacent or close
        char wrong_c = (c == 'z' || c == 'Z') ? c - 1 : c + 1;
        bleKeyboard.write(wrong_c);

        // Pause as if noticing typo
        next_typer_char_ms = now + random(200, 401);
        return;
    }

    // Type character directly
    bleKeyboard.write(c);

    // Apply human typing cadence intervals
    if (c == '\n') {
        next_typer_char_ms = now + random(800, 2001);
    } else if (c == '.' || c == ',' || c == ';' || c == '!' || c == '?') {
        next_typer_char_ms = now + random(200, 501);
    } else {
        next_typer_char_ms = now + random(40, 161);
    }
}


void apply_settings() {
    preferences.begin("macro", true);
    int layout = preferences.getInt("layout", 0);
    int saver_opt = preferences.getInt("saver", 0);
    tui_color_index = preferences.getInt("color", 0);
    debug_enabled = (preferences.getInt("debug", 0) == 1);
    preferences.end();

    if (debug_enabled) Serial.printf("Applying settings: Layout=%d, SaverOpt=%d, Color=%d, Debug=%d\n", layout, saver_opt, tui_color_index, debug_enabled);

    bleKeyboard.setLayout(layout);

    // Convert saver option: 0: disabled, 1: 1 min, 2: 5 min, 3: 10 min
    uint32_t timeout_ms = 0;
    if (saver_opt == 1) timeout_ms = 60000;
    else if (saver_opt == 2) timeout_ms = 300000;
    else if (saver_opt == 3) timeout_ms = 600000;

    set_screensaver_timeout(timeout_ms);
}

void init_macro_logic() {
    bleKeyboard.begin();
    bleKeyboard.setDelay(15);
    esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
    apply_settings();
    if (debug_enabled) Serial.println("Bluetooth Keyboard initialized.");
}

static void execute_chord(String value) {
    value.toLowerCase();
    
    int start = 0;
    int end = value.indexOf('+');
    
    while (true) {
        String token = "";
        if (end == -1) {
            token = value.substring(start);
        } else {
            token = value.substring(start, end);
        }
        token.trim();
        
        if (token.length() > 0) {
            if (token == "ctrl" || token == "control") {
                bleKeyboard.press(KEY_LEFT_CTRL);
            } else if (token == "shift") {
                bleKeyboard.press(KEY_LEFT_SHIFT);
            } else if (token == "alt") {
                bleKeyboard.press(KEY_LEFT_ALT);
            } else if (token == "gui" || token == "win" || token == "cmd") {
                bleKeyboard.press(KEY_LEFT_GUI);
            } else if (token == "enter" || token == "return") {
                bleKeyboard.press(KEY_RETURN);
            } else if (token == "esc" || token == "escape") {
                bleKeyboard.press(KEY_ESC);
            } else if (token == "tab") {
                bleKeyboard.press(KEY_TAB);
            } else if (token == "backspace") {
                bleKeyboard.press(KEY_BACKSPACE);
            } else if (token == "delete" || token == "del") {
                bleKeyboard.press(KEY_DELETE);
            } else if (token == "insert" || token == "ins") {
                bleKeyboard.press(KEY_INSERT);
            } else if (token == "home") {
                bleKeyboard.press(KEY_HOME);
            } else if (token == "end") {
                bleKeyboard.press(KEY_END);
            } else if (token == "pageup" || token == "pgup") {
                bleKeyboard.press(KEY_PAGE_UP);
            } else if (token == "pagedown" || token == "pgdn") {
                bleKeyboard.press(KEY_PAGE_DOWN);
            } else if (token == "up" || token == "uparrow") {
                bleKeyboard.press(KEY_UP_ARROW);
            } else if (token == "down" || token == "downarrow") {
                bleKeyboard.press(KEY_DOWN_ARROW);
            } else if (token == "left" || token == "leftarrow") {
                bleKeyboard.press(KEY_LEFT_ARROW);
            } else if (token == "right" || token == "rightarrow") {
                bleKeyboard.press(KEY_RIGHT_ARROW);
            } else if (token.startsWith("f") && token.length() > 1) {
                int f_num = token.substring(1).toInt();
                if (f_num >= 1 && f_num <= 24) {
                    bleKeyboard.press(KEY_F1 + (f_num - 1));
                }
            } else if (token.length() == 1) {
                bleKeyboard.press(token[0]);
            }
        }
        
        if (end == -1) break;
        start = end + 1;
        end = value.indexOf('+', start);
    }
    
    delay(50);
    bleKeyboard.releaseAll();
}

void execute_macro(int index) {
    if(!bleKeyboard.isConnected()) {
        if (debug_enabled) Serial.println("BLE not connected.");
        return;
    }
    
    preferences.begin("macro", true); // read-only
    String key_type = "type_" + String(index);
    String key_val = "val_" + String(index);
    
    int type = preferences.getInt(key_type.c_str(), 0); 
    String value = preferences.getString(key_val.c_str(), "");
    preferences.end();
    
    if(value == "") {
        if (debug_enabled) Serial.println("Macro empty.");
        return;
    }

    if (type == 0) {
        bleKeyboard.print(value);
    } 
    else if (type == 1) {
        String plain = "";
        for(size_t i = 0; i < value.length(); i++) {
            plain += (char)(value[i] ^ mac_addr[i % 6]);
        }
        bleKeyboard.print(plain);
    }
    else if (type == 2) {
        execute_chord(value);
    }
}
