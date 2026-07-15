#include "macro_logic.h"
#include <BleKeyboard.h>
#include <Preferences.h>
#include <Arduino.h>
#include <esp_mac.h>

BleKeyboard bleKeyboard("MacroBoard ESP32", "Espressif", 100);
Preferences preferences;

// We will store MAC address here to use as XOR key
uint8_t mac_addr[6];

void init_macro_logic() {
    bleKeyboard.begin();
    esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
    Serial.println("Bluetooth Keyboard initialized.");
}

void execute_macro(int index) {
    if(!bleKeyboard.isConnected()) {
        Serial.println("BLE not connected.");
        return;
    }
    
    preferences.begin("macro", true); // read-only
    String key_type = "type_" + String(index);
    String key_val = "val_" + String(index);
    
    int type = preferences.getInt(key_type.c_str(), 0); 
    // 0: String, 1: Password (XOR), 2: Chord
    
    String value = preferences.getString(key_val.c_str(), "");
    preferences.end();
    
    if(value == "") {
        Serial.println("Macro empty.");
        // Fallback for testing:
        // value = "Test Macro " + String(index);
        // type = 0;
        return;
    }

    if (type == 0) {
        // String
        bleKeyboard.print(value);
    } 
    else if (type == 1) {
        // Password (XOR reversal)
        String plain = "";
        for(size_t i = 0; i < value.length(); i++) {
            plain += (char)(value[i] ^ mac_addr[i % 6]);
        }
        bleKeyboard.print(plain);
    }
    else if (type == 2) {
        // Chord (e.g. Ctrl+Shift+1)
        // Basic parsing for testing
        // ... (We will implement robust parsing later)
        bleKeyboard.print(value); // fallback
    }
}
