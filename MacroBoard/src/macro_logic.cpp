#include "macro_logic.h"
#include <BleKeyboard.h>
#include <Preferences.h>
#include <Arduino.h>
#include <esp_mac.h>

BleKeyboard bleKeyboard("MacroBoard ESP32", "Espressif", 100);
Preferences preferences;

#include "macro_screen.h"

// We will store MAC address here to use as XOR key
uint8_t mac_addr[6];

static bool afk_enabled = false;
static uint32_t next_afk_trigger_ms = 0;
static int tui_color_index = 0;
static bool debug_enabled = false; // Default: false (Disabled)

bool is_ble_connected() {
    return bleKeyboard.isConnected();
}

bool is_afk_enabled() {
    return afk_enabled;
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
        // ponytail: schedule first trigger between 1000ms and 28000ms from now
        next_afk_trigger_ms = millis() + random(1000, 28001);
        if (debug_enabled) Serial.println("AFK Mode ENABLED");
    } else {
        if (debug_enabled) Serial.println("AFK Mode DISABLED");
    }
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
