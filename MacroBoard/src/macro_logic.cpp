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
static uint32_t typo_original_codepoint = 0;
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

static uint32_t read_utf8_codepoint(File &f) {
    int c1 = f.read();
    if (c1 == -1) return 0;
    
    if ((c1 & 0x80) == 0) {
        return (uint32_t)c1;
    }
    
    if ((c1 & 0xE0) == 0xC0) {
        int c2 = f.read();
        if (c2 == -1) return (uint32_t)c1;
        return ((uint32_t)(c1 & 0x1F) << 6) | (uint32_t)(c2 & 0x3F);
    }
    
    if ((c1 & 0xF0) == 0xE0) {
        int c2 = f.read();
        int c3 = f.read();
        if (c2 == -1 || c3 == -1) return (uint32_t)c1;
        return ((uint32_t)(c1 & 0x0F) << 12) | ((uint32_t)(c2 & 0x3F) << 6) | (uint32_t)(c3 & 0x3F);
    }
    
    if ((c1 & 0xF8) == 0xF0) {
        int c2 = f.read();
        int c3 = f.read();
        int c4 = f.read();
        if (c2 == -1 || c3 == -1 || c4 == -1) return (uint32_t)c1;
        return ((uint32_t)(c1 & 0x07) << 18) | ((uint32_t)(c2 & 0x3F) << 12) | ((uint32_t)(c3 & 0x3F) << 6) | (uint32_t)(c4 & 0x3F);
    }
    
    return (uint32_t)c1;
}

static void type_utf8_codepoint(uint32_t cp) {
    if (cp <= 127) {
        bleKeyboard.write((uint8_t)cp);
        return;
    }
    
    preferences.begin("macro", true);
    int layout = preferences.getInt("layout", 0); // 0: en-gb, 1: en-us, 2: pt-br
    preferences.end();
    
    bool is_pt_br = (layout == 2);
    
    switch (cp) {
        // Small Acute: á, é, í, ó, ú
        case 0x00E1: // á
        case 0x00E9: // é
        case 0x00ED: // í
        case 0x00F3: // ó
        case 0x00FA: // ú
        {
            char base = 'a';
            if (cp == 0x00E9) base = 'e';
            else if (cp == 0x00ED) base = 'i';
            else if (cp == 0x00F3) base = 'o';
            else if (cp == 0x00FA) base = 'u';
            
            if (is_pt_br) {
                bleKeyboard.writeRaw(0x2F); // ABNT2 acute key ´
            } else {
                bleKeyboard.write('\'');
            }
            delay(15);
            bleKeyboard.write(base);
            break;
        }
        
        // Capital Acute: Á, É, Í, Ó, Ú
        case 0x00C1: // Á
        case 0x00C9: // É
        case 0x00CD: // Í
        case 0x00D3: // Ó
        case 0x00DA: // Ú
        {
            char base = 'A';
            if (cp == 0x00C9) base = 'E';
            else if (cp == 0x00CD) base = 'I';
            else if (cp == 0x00D3) base = 'O';
            else if (cp == 0x00DA) base = 'U';
            
            if (is_pt_br) {
                bleKeyboard.writeRaw(0x2F); // ABNT2 acute key ´
            } else {
                bleKeyboard.write('\'');
            }
            delay(15);
            bleKeyboard.write(base);
            break;
        }
        
        // Small Tilde: ã, õ
        case 0x00E3: // ã
        case 0x00F5: // õ
        {
            char base = (cp == 0x00E3) ? 'a' : 'o';
            bleKeyboard.write('~');
            delay(15);
            bleKeyboard.write(base);
            break;
        }
        
        // Capital Tilde: Ã, Õ
        case 0x00C3: // Ã
        case 0x00D5: // Õ
        {
            char base = (cp == 0x00C3) ? 'A' : 'O';
            bleKeyboard.write('~');
            delay(15);
            bleKeyboard.write(base);
            break;
        }
        
        // Small Circumflex: â, ê, ô
        case 0x00E2: // â
        case 0x00EA: // ê
        case 0x00F4: // ô
        {
            char base = 'a';
            if (cp == 0x00EA) base = 'e';
            else if (cp == 0x00F4) base = 'o';
            bleKeyboard.write('^');
            delay(15);
            bleKeyboard.write(base);
            break;
        }
        
        // Capital Circumflex: Â, Ê, Ô
        case 0x00C2: // Â
        case 0x00CA: // Ê
        case 0x00D4: // Ô
        {
            char base = 'A';
            if (cp == 0x00CA) base = 'E';
            else if (cp == 0x00D4) base = 'O';
            bleKeyboard.write('^');
            delay(15);
            bleKeyboard.write(base);
            break;
        }
        
        // Small Grave: à
        case 0x00E0: // à
        {
            bleKeyboard.write('`');
            delay(15);
            bleKeyboard.write('a');
            break;
        }
        
        // Capital Grave: À
        case 0x00C0: // À
        {
            bleKeyboard.write('`');
            delay(15);
            bleKeyboard.write('A');
            break;
        }
        
        // Cedilla: ç, Ç
        case 0x00E7: // ç
        {
            if (is_pt_br) {
                bleKeyboard.writeRaw(0x33); // ABNT2 ç key
            } else {
                bleKeyboard.write('\'');
                delay(15);
                bleKeyboard.write('c');
            }
            break;
        }
        
        case 0x00C7: // Ç
        {
            if (is_pt_br) {
                bleKeyboard.writeRaw(0x33 | 0x80); // ABNT2 Ç key (Shift+0x33)
            } else {
                bleKeyboard.write('\'');
                delay(15);
                bleKeyboard.write('C');
            }
            break;
        }
        
        default:
            if (cp <= 255) {
                bleKeyboard.write((uint8_t)cp);
            }
            break;
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
            type_utf8_codepoint(typo_original_codepoint);
            typo_in_progress = false;
            typo_state = 0;
            
            // Calculate delay for next regular char
            if (typo_original_codepoint == '\n') {
                next_typer_char_ms = now + random(800, 2001);
            } else if (typo_original_codepoint == '.' || typo_original_codepoint == ',' || typo_original_codepoint == ';' || typo_original_codepoint == '!' || typo_original_codepoint == '?') {
                next_typer_char_ms = now + random(200, 501);
            } else {
                next_typer_char_ms = now + random(40, 161);
            }
            return;
        }
    }

    // Open file if not currently open
    if (!current_typer_file) {
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
        next_typer_char_ms = now + 500;
        return;
    }

    uint32_t cp = read_utf8_codepoint(current_typer_file);
    if (cp == 0) { // End of File reached
        current_typer_file.close();
        // Rest delay between 45 seconds and 2 minutes (45,000ms to 120,000ms) before opening next random file
        next_typer_char_ms = now + random(45000, 120001);
        if (debug_enabled) Serial.println("Typer finished reading file. Resting for 45s-2m.");
        return;
    }

    // ~3% chance of typo on standard ASCII letters
    if (((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) && random(0, 100) < 3) {
        typo_in_progress = true;
        typo_original_codepoint = cp;
        typo_state = 1;

        // Type a wrong character adjacent or close
        char wrong_c = (cp == 'z' || cp == 'Z') ? (char)cp - 1 : (char)cp + 1;
        bleKeyboard.write(wrong_c);

        // Pause as if noticing typo
        next_typer_char_ms = now + random(200, 401);
        return;
    }

    // Type character directly
    type_utf8_codepoint(cp);

    // Apply human typing cadence intervals
    if (cp == '\n') {
        next_typer_char_ms = now + random(800, 2001);
    } else if (cp == '.' || cp == ',' || cp == ';' || cp == '!' || cp == '?') {
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
