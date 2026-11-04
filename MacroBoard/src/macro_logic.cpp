#include "macro_logic.h"
#include <BleKeyboard.h>
#include <BLEDevice.h>
#include <Preferences.h>
#include <Arduino.h>
#include <esp_mac.h>
#include <SD_MMC.h>
#include <LittleFS.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <vector>
#include <algorithm>

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
static bool hybrid_enabled = false;
static uint32_t next_typer_char_ms = 0;
static std::vector<String> typer_files;
static File current_typer_file;

// Hybrid mode state
static uint8_t hybrid_state = 0; // 0: Typing, 1: AFK rest interval, 2: ESC sequence
static uint32_t hybrid_afk_end_ms = 0;
static uint8_t hybrid_esc_count = 0;
static uint32_t next_hybrid_esc_ms = 0;

// Typo state machine for Typer Mode
static bool typo_in_progress = false;
static uint32_t typo_original_codepoint = 0;
static int typo_state = 0; // 1: sent wrong char, 2: sent backspace

// SneaKeyboard state and PSRAM-backed file cache
struct PendingSneakyOp {
    bool is_delete;
    String name;
    uint8_t* data;
    size_t len;
};

struct SneakyRamFile {
    String name;
    size_t size;
    uint8_t* data;
};

static int cached_layout = 0;
static bool sneaky_fs_mounted = false;
static SemaphoreHandle_t sneaky_mutex = nullptr;
static SemaphoreHandle_t sneaky_step_mutex = nullptr;
static size_t cached_fs_total = 0;
static size_t cached_fs_used = 0;
static std::vector<SneakyFileInfo> cached_sneaky_files;
static std::vector<SneakyRamFile> cached_ram_files;
static std::vector<PendingSneakyOp> pending_sneaky_ops;
static volatile uint32_t last_http_activity_ms = 0;
static volatile bool sneaky_upload_in_progress = false;
static volatile bool sneaky_active = false;
static bool sneaky_multi_file_format = false;
static std::vector<SneakyRamFile> sneaky_play_queue;
static size_t sneaky_queue_idx = 0;
static size_t sneaky_file_pos = 0;
static bool sneaky_file_started = false;
static String sneaky_header_buf = "";
static size_t sneaky_header_pos = 0;

bool is_ble_connected() {
    return bleKeyboard.isConnected();
}

void pause_ble_for_wifi() {
    stop_sneaky_paste();
    set_afk_enabled(false);
    set_typer_enabled(false);
    set_hybrid_enabled(false);
    bleKeyboard.releaseAll();
    if (BLEDevice::getInitialized()) {
        BLEDevice::stopAdvertising();
    }
}

void resume_ble_after_wifi() {
    if (BLEDevice::getInitialized() && !bleKeyboard.isConnected()) {
        BLEDevice::startAdvertising();
    }
}

bool is_afk_enabled() {
    return afk_enabled;
}

bool is_typer_enabled() {
    return typer_enabled;
}

bool is_hybrid_enabled() {
    return hybrid_enabled;
}

bool is_automated_mode_active() {
    return afk_enabled || typer_enabled || hybrid_enabled;
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
        if (typer_enabled) typer_enabled = false;
        if (hybrid_enabled) hybrid_enabled = false;
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
        if (afk_enabled) afk_enabled = false;
        if (hybrid_enabled) hybrid_enabled = false;
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

void set_hybrid_enabled(bool enabled) {
    hybrid_enabled = enabled;
    if (hybrid_enabled) {
        if (afk_enabled) afk_enabled = false;
        if (typer_enabled) typer_enabled = false;
        if (current_typer_file) {
            current_typer_file.close();
        }
        typo_in_progress = false;
        scan_typer_dir();
        hybrid_state = 0; // Start with typing
        next_typer_char_ms = millis() + 500;
        if (debug_enabled) Serial.println("Hybrid Mode ENABLED");
    } else {
        if (current_typer_file) {
            current_typer_file.close();
        }
        typo_in_progress = false;
        hybrid_state = 0;
        if (debug_enabled) Serial.println("Hybrid Mode DISABLED");
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
    if (cp == '\r') return;
    if (cp <= 127) {
        bleKeyboard.write((uint8_t)cp);
        // Resolve dead keys (` on all layouts, ~ and ^ on pt-br) with a trailing space
        // so the literal ASCII character is emitted without accenting the next character.
        if (cp == '`' || (cached_layout == 2 && (cp == '~' || cp == '^'))) {
            bleKeyboard.write(' ');
        }
        return;
    }
    
    bool is_pt_br = (cached_layout == 2);
    
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

void process_hybrid_logic() {
    if (!hybrid_enabled || !bleKeyboard.isConnected() || !sd_mounted) return;

    uint32_t now = millis();

    // State 0: Typing text file
    if (hybrid_state == 0) {
        if (!current_typer_file) {
            if (now < next_typer_char_ms) return;
            
            if (typer_files.empty()) scan_typer_dir();
            if (typer_files.empty()) {
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
            if (debug_enabled) Serial.printf("[Hybrid] Opened file %s\n", typer_files[file_idx].c_str());
            next_typer_char_ms = now + 500;
            return;
        }

        if (now < next_typer_char_ms) return;

        // Handle typo state machine
        if (typo_in_progress) {
            if (typo_state == 1) {
                bleKeyboard.write(KEY_BACKSPACE);
                typo_state = 2;
                next_typer_char_ms = now + random(100, 251);
                return;
            } else if (typo_state == 2) {
                type_utf8_codepoint(typo_original_codepoint);
                typo_in_progress = false;
                typo_state = 0;
                if (typo_original_codepoint == '\n') next_typer_char_ms = now + random(800, 2001);
                else if (typo_original_codepoint == '.' || typo_original_codepoint == ',' || typo_original_codepoint == ';' || typo_original_codepoint == '!' || typo_original_codepoint == '?') next_typer_char_ms = now + random(200, 501);
                else next_typer_char_ms = now + random(40, 161);
                return;
            }
        }

        uint32_t cp = read_utf8_codepoint(current_typer_file);
        if (cp == 0) { // EOF reached in Hybrid Mode!
            current_typer_file.close();
            // Transition to State 1 (AFK Interval for 45s to 2min)
            hybrid_state = 1;
            uint32_t interval = random(45000, 120001);
            hybrid_afk_end_ms = now + interval;
            next_afk_trigger_ms = now + random(1000, 5000);
            if (debug_enabled) Serial.printf("[Hybrid] EOF reached. Switching to AFK mode for %u ms\n", (unsigned int)interval);
            return;
        }

        // ~3% chance of typo
        if (((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) && random(0, 100) < 3) {
            typo_in_progress = true;
            typo_original_codepoint = cp;
            typo_state = 1;
            char wrong_c = (cp == 'z' || cp == 'Z') ? (char)cp - 1 : (char)cp + 1;
            bleKeyboard.write(wrong_c);
            next_typer_char_ms = now + random(200, 401);
            return;
        }

        type_utf8_codepoint(cp);
        if (cp == '\n') next_typer_char_ms = now + random(800, 2001);
        else if (cp == '.' || cp == ',' || cp == ';' || cp == '!' || cp == '?') next_typer_char_ms = now + random(200, 501);
        else next_typer_char_ms = now + random(40, 161);
    }
    // State 1: AFK mode during rest interval
    else if (hybrid_state == 1) {
        if (now >= hybrid_afk_end_ms) {
            // AFK interval finished! Transition to State 2 (Send Escape twice with 500ms interval)
            hybrid_state = 2;
            hybrid_esc_count = 0;
            next_hybrid_esc_ms = now;
            if (debug_enabled) Serial.println("[Hybrid] AFK interval finished. Sending double ESC sequence before resuming Typer.");
            return;
        }

        // Execute AFK key routine periodically
        if (now >= next_afk_trigger_ms) {
            static const uint8_t non_destructive_keys[] = {
                KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT,
                KEY_LEFT_CTRL,  KEY_LEFT_ALT,
                KEY_UP_ARROW,   KEY_DOWN_ARROW,
                KEY_LEFT_ARROW, KEY_RIGHT_ARROW,
                KEY_PAGE_UP,    KEY_PAGE_DOWN
            };
            next_afk_trigger_ms = now + random(1000, 28001);
            int key_idx = random(0, sizeof(non_destructive_keys) / sizeof(non_destructive_keys[0]));
            uint8_t key = non_destructive_keys[key_idx];
            bleKeyboard.press(key);
            delay(20);
            bleKeyboard.releaseAll();
            if (debug_enabled) Serial.printf("[Hybrid AFK] Key 0x%02X sent.\n", key);
        }
    }
    // State 2: Send ESC key twice with 500ms interval before returning to Typer Mode
    else if (hybrid_state == 2) {
        if (now >= next_hybrid_esc_ms) {
            bleKeyboard.write(KEY_ESC);
            hybrid_esc_count++;
            if (debug_enabled) Serial.printf("[Hybrid] ESC %d sent.\n", hybrid_esc_count);

            if (hybrid_esc_count >= 2) {
                // Done sending double ESC! Return to State 0 (Typer Mode)
                hybrid_state = 0;
                next_typer_char_ms = now + 500;
                if (debug_enabled) Serial.println("[Hybrid] Resuming Typer mode with next file selection.");
            } else {
                next_hybrid_esc_ms = now + 500; // 500ms delay between ESC keys
            }
        }
    }
}



void apply_settings() {
    preferences.begin("macro", true);
    int layout = preferences.getInt("layout", 0);
    int saver_opt = preferences.getInt("saver", 0);
    tui_color_index = preferences.getInt("color", 0);
    debug_enabled = (preferences.getInt("debug", 0) == 1);
    preferences.end();

    cached_layout = layout;

    if (debug_enabled) Serial.printf("Applying settings: Layout=%d, SaverOpt=%d, Color=%d, Debug=%d\n", layout, saver_opt, tui_color_index, debug_enabled);

    bleKeyboard.setLayout(layout);

    // Convert saver option: 0: disabled, 1: 1 min, 2: 5 min, 3: 10 min
    uint32_t timeout_ms = 0;
    if (saver_opt == 1) timeout_ms = 60000;
    else if (saver_opt == 2) timeout_ms = 300000;
    else if (saver_opt == 3) timeout_ms = 600000;

    set_screensaver_timeout(timeout_ms);
}

static uint32_t read_utf8_from_string(const String &s, size_t &pos) {
    if (pos >= s.length()) return 0;
    uint8_t c1 = (uint8_t)s[pos++];
    if ((c1 & 0x80) == 0) return (uint32_t)c1;
    if ((c1 & 0xE0) == 0xC0) {
        if (pos >= s.length()) return (uint32_t)c1;
        uint8_t c2 = (uint8_t)s[pos++];
        return ((uint32_t)(c1 & 0x1F) << 6) | (uint32_t)(c2 & 0x3F);
    }
    if ((c1 & 0xF0) == 0xE0) {
        if (pos + 1 >= s.length()) return (uint32_t)c1;
        uint8_t c2 = (uint8_t)s[pos++];
        uint8_t c3 = (uint8_t)s[pos++];
        return ((uint32_t)(c1 & 0x0F) << 12) | ((uint32_t)(c2 & 0x3F) << 6) | (uint32_t)(c3 & 0x3F);
    }
    if ((c1 & 0xF8) == 0xF0) {
        if (pos + 2 >= s.length()) return (uint32_t)c1;
        uint8_t c2 = (uint8_t)s[pos++];
        uint8_t c3 = (uint8_t)s[pos++];
        uint8_t c4 = (uint8_t)s[pos++];
        return ((uint32_t)(c1 & 0x07) << 18) | ((uint32_t)(c2 & 0x3F) << 12) | ((uint32_t)(c3 & 0x3F) << 6) | (uint32_t)(c4 & 0x3F);
    }
    return (uint32_t)c1;
}

static uint32_t read_utf8_from_buf(const uint8_t *buf, size_t len, size_t &pos) {
    if (!buf || pos >= len) return 0;
    uint8_t c1 = buf[pos++];
    if ((c1 & 0x80) == 0) return (uint32_t)c1;
    if ((c1 & 0xE0) == 0xC0) {
        if (pos >= len) return (uint32_t)c1;
        uint8_t c2 = buf[pos++];
        return ((uint32_t)(c1 & 0x1F) << 6) | (uint32_t)(c2 & 0x3F);
    }
    if ((c1 & 0xF0) == 0xE0) {
        if (pos + 1 >= len) return (uint32_t)c1;
        uint8_t c2 = buf[pos++];
        uint8_t c3 = buf[pos++];
        return ((uint32_t)(c1 & 0x0F) << 12) | ((uint32_t)(c2 & 0x3F) << 6) | (uint32_t)(c3 & 0x3F);
    }
    if ((c1 & 0xF8) == 0xF0) {
        if (pos + 2 >= len) return (uint32_t)c1;
        uint8_t c2 = buf[pos++];
        uint8_t c3 = buf[pos++];
        uint8_t c4 = buf[pos++];
        return ((uint32_t)(c1 & 0x07) << 18) | ((uint32_t)(c2 & 0x3F) << 12) | ((uint32_t)(c3 & 0x3F) << 6) | (uint32_t)(c4 & 0x3F);
    }
    return (uint32_t)c1;
}

static String clean_sneaky_filename(const String &filename) {
    String clean = filename;
    int slashIdx = clean.lastIndexOf('/');
    if (slashIdx >= 0) clean = clean.substring(slashIdx + 1);
    int bslashIdx = clean.lastIndexOf('\\');
    if (bslashIdx >= 0) clean = clean.substring(bslashIdx + 1);
    clean.trim();
    return clean;
}

static void refresh_sneaky_cache() {
    std::vector<SneakyFileInfo> disk_files;
    std::vector<SneakyRamFile> ram_files;
    size_t total = 0;
    size_t used = 0;

    uint8_t *dram_buf = (uint8_t *)heap_caps_malloc(256, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (sneaky_fs_mounted) {
        total = LittleFS.totalBytes();
        used = LittleFS.usedBytes();

        File root = LittleFS.open("/");
        if (root && root.isDirectory()) {
            File file = root.openNextFile();
            while (file) {
                if (!file.isDirectory()) {
                    String name = String(file.name());
                    if (name.startsWith("/")) name = name.substring(1);
                    if (!name.startsWith("config/") && name.length() > 0) {
                        size_t fsize = file.size();
                        SneakyFileInfo info;
                        info.name = name;
                        info.size = fsize;
                        disk_files.push_back(info);

                        uint8_t *pbuf = (uint8_t *)heap_caps_malloc(fsize > 0 ? fsize : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if (pbuf && dram_buf && fsize > 0) {
                            size_t rd = 0;
                            while (rd < fsize) {
                                size_t chunk = (fsize - rd > 256) ? 256 : (fsize - rd);
                                int got = file.read(dram_buf, chunk);
                                if (got <= 0) break;
                                memcpy(pbuf + rd, dram_buf, (size_t)got);
                                rd += (size_t)got;
                            }
                        }
                        SneakyRamFile rf;
                        rf.name = name;
                        rf.size = fsize;
                        rf.data = pbuf;
                        ram_files.push_back(rf);
                    }
                }
                file.close();
                file = root.openNextFile();
            }
            root.close();
        }
    }

    if (dram_buf) heap_caps_free(dram_buf);

    std::sort(disk_files.begin(), disk_files.end(), [](const SneakyFileInfo &a, const SneakyFileInfo &b) {
        return a.name.compareTo(b.name) < 0;
    });
    std::sort(ram_files.begin(), ram_files.end(), [](const SneakyRamFile &a, const SneakyRamFile &b) {
        return a.name.compareTo(b.name) < 0;
    });

    if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
    for (auto &old_rf : cached_ram_files) {
        if (old_rf.data) heap_caps_free(old_rf.data);
    }
    cached_fs_total = total;
    cached_fs_used = used;
    cached_sneaky_files = std::move(disk_files);
    cached_ram_files = std::move(ram_files);
    if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);
}

static void flush_pending_sneaky_ops() {
    if (!sneaky_fs_mounted) return;
    uint8_t *dram_buf = (uint8_t *)heap_caps_malloc(256, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!dram_buf) return;

    bool did_work = false;
    while (true) {
        PendingSneakyOp op;
        bool has_op = false;
        if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
        if (!pending_sneaky_ops.empty()) {
            op = pending_sneaky_ops.front();
            pending_sneaky_ops.erase(pending_sneaky_ops.begin());
            has_op = true;
        }
        if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);

        if (!has_op) break;
        did_work = true;

        String path = "/" + op.name;
        if (op.is_delete) {
            if (LittleFS.exists(path)) {
                LittleFS.remove(path);
            }
            if (debug_enabled) Serial.printf("[LittleFS] Deleted %s\n", path.c_str());
        } else {
            File f = LittleFS.open(path, "w");
            if (f) {
                if (op.data && op.len > 0) {
                    size_t written = 0;
                    while (written < op.len) {
                        size_t chunk = (op.len - written > 256) ? 256 : (op.len - written);
                        memcpy(dram_buf, op.data + written, chunk);
                        f.write(dram_buf, chunk);
                        written += chunk;
                        yield();
                    }
                }
                f.close();
                if (debug_enabled) Serial.printf("[LittleFS] Wrote %s (%u B)\n", path.c_str(), (unsigned int)op.len);
            } else {
                Serial.printf("[LittleFS] Failed to open %s for write\n", path.c_str());
            }
            if (op.data) {
                heap_caps_free(op.data);
            }
        }
        yield();
    }

    heap_caps_free(dram_buf);

    if (did_work) {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
        if (pending_sneaky_ops.empty()) {
            cached_fs_total = total;
            cached_fs_used = used;
        }
        if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);
        request_sneaky_ui_refresh();
    }
}

bool init_sneaky_fs() {
    if (!sneaky_mutex) {
        sneaky_mutex = xSemaphoreCreateMutex();
    }
    if (!sneaky_step_mutex) {
        sneaky_step_mutex = xSemaphoreCreateMutex();
    }
    if (!LittleFS.begin(true)) {
        sneaky_fs_mounted = false;
        Serial.println("[LittleFS] Mount Failed");
        return false;
    }
    sneaky_fs_mounted = true;
    refresh_sneaky_cache();
    Serial.printf("[LittleFS] Mounted OK. Total: %uB, Used: %uB\n",
                  (unsigned int)cached_fs_total, (unsigned int)cached_fs_used);
    return true;
}

bool is_sneaky_fs_mounted() {
    return sneaky_fs_mounted;
}

void refresh_sneaky_fs() {
    refresh_sneaky_cache();
}

size_t get_sneaky_fs_total_bytes() {
    size_t val = 0;
    if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
    val = cached_fs_total;
    if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);
    return val;
}

size_t get_sneaky_fs_used_bytes() {
    size_t val = 0;
    if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
    val = cached_fs_used;
    if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);
    return val;
}

std::vector<SneakyFileInfo> get_sneaky_files() {
    std::vector<SneakyFileInfo> copy;
    if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
    copy = cached_sneaky_files;
    if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);
    return copy;
}

void notify_sneaky_http_activity(bool upload_active) {
    last_http_activity_ms = millis();
    sneaky_upload_in_progress = upload_active;
}

bool queue_sneaky_file_write(const String& filename, uint8_t* data, size_t len) {
    if (!sneaky_fs_mounted) {
        if (data) heap_caps_free(data);
        return false;
    }
    String clean = clean_sneaky_filename(filename);
    if (clean.length() == 0) {
        if (data) heap_caps_free(data);
        return false;
    }

    uint8_t *ramCopy = (uint8_t *)heap_caps_malloc(len > 0 ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ramCopy && data && len > 0) {
        memcpy(ramCopy, data, len);
    }

    if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
    for (auto it = pending_sneaky_ops.begin(); it != pending_sneaky_ops.end(); ) {
        if (it->name == clean) {
            if (it->data) heap_caps_free(it->data);
            it = pending_sneaky_ops.erase(it);
        } else {
            ++it;
        }
    }
    PendingSneakyOp op;
    op.is_delete = false;
    op.name = clean;
    op.data = data;
    op.len = len;
    pending_sneaky_ops.push_back(op);

    bool ram_found = false;
    for (auto &rf : cached_ram_files) {
        if (rf.name == clean) {
            if (rf.data) heap_caps_free(rf.data);
            rf.data = ramCopy;
            rf.size = len;
            ram_found = true;
            break;
        }
    }
    if (!ram_found) {
        SneakyRamFile rf;
        rf.name = clean;
        rf.size = len;
        rf.data = ramCopy;
        cached_ram_files.push_back(rf);
        std::sort(cached_ram_files.begin(), cached_ram_files.end(), [](const SneakyRamFile &a, const SneakyRamFile &b) {
            return a.name.compareTo(b.name) < 0;
        });
    }

    bool found = false;
    for (auto &f : cached_sneaky_files) {
        if (f.name == clean) {
            if (cached_fs_used >= f.size) cached_fs_used -= f.size;
            f.size = len;
            cached_fs_used += len;
            found = true;
            break;
        }
    }
    if (!found) {
        SneakyFileInfo info;
        info.name = clean;
        info.size = len;
        cached_sneaky_files.push_back(info);
        cached_fs_used += len;
        std::sort(cached_sneaky_files.begin(), cached_sneaky_files.end(), [](const SneakyFileInfo &a, const SneakyFileInfo &b) {
            return a.name.compareTo(b.name) < 0;
        });
    }
    if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);

    last_http_activity_ms = millis();
    sneaky_upload_in_progress = false;
    request_sneaky_ui_refresh();
    return true;
}

bool delete_sneaky_file(const String& filename) {
    if (!sneaky_fs_mounted) return false;
    String clean = clean_sneaky_filename(filename);
    if (clean.length() == 0) return false;

    if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
    for (auto it = pending_sneaky_ops.begin(); it != pending_sneaky_ops.end(); ) {
        if (it->name == clean) {
            if (it->data) heap_caps_free(it->data);
            it = pending_sneaky_ops.erase(it);
        } else {
            ++it;
        }
    }
    PendingSneakyOp op;
    op.is_delete = true;
    op.name = clean;
    op.data = nullptr;
    op.len = 0;
    pending_sneaky_ops.push_back(op);

    for (auto it = cached_ram_files.begin(); it != cached_ram_files.end(); ++it) {
        if (it->name == clean) {
            if (it->data) heap_caps_free(it->data);
            cached_ram_files.erase(it);
            break;
        }
    }

    for (auto it = cached_sneaky_files.begin(); it != cached_sneaky_files.end(); ++it) {
        if (it->name == clean) {
            if (cached_fs_used >= it->size) cached_fs_used -= it->size;
            cached_sneaky_files.erase(it);
            break;
        }
    }
    if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);

    last_http_activity_ms = millis();
    request_sneaky_ui_refresh();
    return true;
}

void stop_sneaky_paste() {
    sneaky_active = false;
    bleKeyboard.setDelay(15);
    for (auto &item : sneaky_play_queue) {
        if (item.data) heap_caps_free(item.data);
    }
    sneaky_play_queue.clear();
    sneaky_queue_idx = 0;
    sneaky_file_pos = 0;
    sneaky_file_started = false;
    sneaky_header_buf = "";
    sneaky_header_pos = 0;
    if (debug_enabled) Serial.println("SneaKeyboard stopped.");
}

bool is_sneaky_paste_active() {
    return sneaky_active;
}

void trigger_sneaky_paste_file(const String& filename) {
    if (!bleKeyboard.isConnected() || !sneaky_fs_mounted) {
        if (debug_enabled) Serial.println("Cannot trigger SneaKeyboard: BLE not connected or FS unmounted.");
        return;
    }
    if (sneaky_step_mutex) xSemaphoreTake(sneaky_step_mutex, portMAX_DELAY);
    stop_sneaky_paste();
    String clean = clean_sneaky_filename(filename);

    if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
    for (const auto &rf : cached_ram_files) {
        if (rf.name == clean) {
            uint8_t *copy = (uint8_t *)heap_caps_malloc(rf.size > 0 ? rf.size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (copy && rf.data && rf.size > 0) {
                memcpy(copy, rf.data, rf.size);
            }
            SneakyRamFile item;
            item.name = rf.name;
            item.size = rf.size;
            item.data = copy;
            sneaky_play_queue.push_back(item);
            break;
        }
    }
    if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);

    if (!sneaky_play_queue.empty()) {
        sneaky_queue_idx = 0;
        sneaky_file_pos = 0;
        sneaky_file_started = false;
        sneaky_multi_file_format = false;
        bleKeyboard.setDelay(10);
        sneaky_active = true;
    }
    if (sneaky_step_mutex) xSemaphoreGive(sneaky_step_mutex);
    if (debug_enabled) Serial.printf("SneaKeyboard triggered for file: %s\n", clean.c_str());
}

void trigger_sneaky_paste_all() {
    if (!bleKeyboard.isConnected() || !sneaky_fs_mounted) {
        if (debug_enabled) Serial.println("Cannot trigger SneaKeyboard All: BLE not connected or FS unmounted.");
        return;
    }
    if (sneaky_step_mutex) xSemaphoreTake(sneaky_step_mutex, portMAX_DELAY);
    stop_sneaky_paste();

    if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
    for (const auto &rf : cached_ram_files) {
        uint8_t *copy = (uint8_t *)heap_caps_malloc(rf.size > 0 ? rf.size : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy && rf.data && rf.size > 0) {
            memcpy(copy, rf.data, rf.size);
        }
        SneakyRamFile item;
        item.name = rf.name;
        item.size = rf.size;
        item.data = copy;
        sneaky_play_queue.push_back(item);
    }
    if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);

    if (!sneaky_play_queue.empty()) {
        sneaky_queue_idx = 0;
        sneaky_file_pos = 0;
        sneaky_file_started = false;
        sneaky_multi_file_format = (sneaky_play_queue.size() > 1);
        bleKeyboard.setDelay(10);
        sneaky_active = true;
    }
    if (sneaky_step_mutex) xSemaphoreGive(sneaky_step_mutex);
    if (debug_enabled) Serial.printf("SneaKeyboard All triggered (%u files).\n", (unsigned int)sneaky_play_queue.size());
}

void process_sneaky_paste_logic() {
    if (!sneaky_step_mutex) return;
    if (xSemaphoreTake(sneaky_step_mutex, 0) != pdTRUE) return;

    uint32_t now = millis();
    if (sneaky_upload_in_progress && (now - last_http_activity_ms > 3000)) {
        sneaky_upload_in_progress = false;
    }

    if (!sneaky_active) {
        bool has_pending = false;
        if (sneaky_mutex) xSemaphoreTake(sneaky_mutex, portMAX_DELAY);
        has_pending = !pending_sneaky_ops.empty();
        if (sneaky_mutex) xSemaphoreGive(sneaky_mutex);

        if (has_pending && !sneaky_upload_in_progress && (now - last_http_activity_ms > 1500)) {
            flush_pending_sneaky_ops();
        }
        xSemaphoreGive(sneaky_step_mutex);
        return;
    }

    if (!bleKeyboard.isConnected() || !sneaky_fs_mounted) {
        stop_sneaky_paste();
        xSemaphoreGive(sneaky_step_mutex);
        return;
    }

    for (int step = 0; step < 8 && sneaky_active && bleKeyboard.isConnected(); step++) {
        if (!sneaky_file_started) {
            if (sneaky_queue_idx >= sneaky_play_queue.size()) {
                stop_sneaky_paste();
                if (debug_enabled) Serial.println("SneaKeyboard completed.");
                xSemaphoreGive(sneaky_step_mutex);
                return;
            }

            const String &fname = sneaky_play_queue[sneaky_queue_idx].name;
            if (sneaky_multi_file_format) {
                sneaky_header_buf = "------ " + fname + " ------\n";
                sneaky_header_pos = 0;
            } else {
                sneaky_header_buf = "";
                sneaky_header_pos = 0;
            }
            sneaky_file_pos = 0;
            sneaky_file_started = true;
        }

        if (sneaky_header_pos < sneaky_header_buf.length()) {
            uint32_t cp = read_utf8_from_string(sneaky_header_buf, sneaky_header_pos);
            if (cp != 0) {
                type_utf8_codepoint(cp);
            }
            continue;
        }

        const SneakyRamFile &cur = sneaky_play_queue[sneaky_queue_idx];
        if (sneaky_file_pos < cur.size && cur.data) {
            uint32_t cp = read_utf8_from_buf(cur.data, cur.size, sneaky_file_pos);
            if (cp != 0) {
                type_utf8_codepoint(cp);
            }
        } else {
            type_utf8_codepoint('\n');
            sneaky_queue_idx++;
            sneaky_file_started = false;
        }
    }

    xSemaphoreGive(sneaky_step_mutex);
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
