#include "web_server.h"
#include "macro_logic.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <Arduino.h>
#include <esp_mac.h>

extern Preferences preferences;
static AsyncWebServer server(80);
static bool web_server_running = false;
bool should_reboot = false;

bool is_web_server_active() {
    return web_server_running;
}

void stop_web_server() {
    if (!web_server_running) return;
    if (is_debug_enabled()) Serial.println("Stopping Web Server...");
    server.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    web_server_running = false;
    if (is_debug_enabled()) Serial.println("Web Server stopped.");
}

void start_web_server() {
    if (web_server_running) return;
    if (is_debug_enabled()) Serial.println("Starting Web Server...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MacroBoard-Config", "12345678");
    if (is_debug_enabled()) {
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
    }

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
        html += "<title>MacroBoard TUI Configuration</title>";
        html += "<style>";
        html += "body { background-color: #0f0f13; color: #e2e2e9; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; margin: 0; padding: 20px; display: flex; justify-content: center; }";
        html += ".container { max-width: 800px; width: 100%; }";
        html += "h2 { color: #00ffff; border-bottom: 2px solid #00ffff; padding-bottom: 10px; margin-bottom: 20px; text-shadow: 0 0 5px rgba(0, 255, 255, 0.3); }";
        html += "h3 { color: #00ffff; margin-top: 0; margin-bottom: 15px; font-size: 1.1em; }";
        html += ".card { background: #181822; border: 1px solid #2a2a3a; border-radius: 8px; padding: 20px; margin-bottom: 20px; box-shadow: 0 4px 6px rgba(0, 0, 0, 0.3); }";
        html += ".card:hover { border-color: #00ffff; }";
        html += ".grid-3 { display: grid; grid-template-columns: 1fr 1fr 2fr; gap: 15px; }";
        html += "@media (max-width: 600px) { .grid-3 { grid-template-columns: 1fr; } }";
        html += "label { display: block; margin-bottom: 5px; font-size: 0.85em; color: #8a8a9a; font-weight: 600; }";
        html += "input[type='text'], select { background: #0f0f13; border: 1px solid #3a3a4a; border-radius: 4px; color: #fff; padding: 10px; width: 100%; box-sizing: border-box; font-family: inherit; font-size: 0.95em; }";
        html += "input[type='text']:focus, select:focus { border-color: #00ffff; outline: none; box-shadow: 0 0 5px rgba(0, 255, 255, 0.5); }";
        html += "button { background: #00ffff; color: #0f0f13; border: none; border-radius: 6px; padding: 14px; font-size: 1.1em; font-weight: bold; width: 100%; cursor: pointer; transition: all 0.2s ease-in-out; }";
        html += "button:hover { background: #00cccc; box-shadow: 0 0 10px rgba(0, 255, 255, 0.4); }";
        html += ".instructions { background: #14222a; border: 1px solid #005f5f; border-radius: 8px; padding: 15px; margin-bottom: 25px; font-size: 0.9em; line-height: 1.5; }";
        html += ".instructions code { background: #0f0f13; color: #00ffff; padding: 2px 6px; border-radius: 4px; font-family: Consolas, monospace; }";
        html += ".instructions ul { margin: 5px 0 0 20px; padding: 0; }";
        html += "</style></head>";
        html += "<body><div class='container'><h2>MacroBoard TUI Configuration</h2>";

        // Instruction Box
        html += "<div class='instructions'>";
        html += "<strong>Chord Formatting Instructions:</strong><br>";
        html += "Chords simulate key combinations pressed simultaneously. Separate keys with a <code>+</code> sign.<br>";
        html += "<ul>";
        html += "<li><strong>Modifiers:</strong> <code>Ctrl</code>, <code>Shift</code>, <code>Alt</code>, <code>Gui</code> (or <code>Win</code>/<code>Cmd</code>)</li>";
        html += "<li><strong>Special Keys:</strong> <code>Enter</code>, <code>Esc</code>, <code>Tab</code>, <code>Backspace</code>, <code>Delete</code>, <code>Insert</code>, <code>Home</code>, <code>End</code>, <code>PageUp</code>, <code>PageDown</code>, <code>Up</code>, <code>Down</code>, <code>Left</code>, <code>Right</code>, <code>F1</code> to <code>F24</code></li>";
        html += "<li><strong>Example (Ctrl + Shift + a):</strong> <code>Ctrl + Shift + a</code></li>";
        html += "</ul>";
        html += "</div>";

        html += "<form action='/save' method='POST'>";

        preferences.begin("macro", true);

        // System Settings Card
        html += "<div class='card'><h3>System Settings</h3><div class='grid-3'>";
        
        // Layout selector
        int layout = preferences.getInt("layout", 0);
        html += "<div><label>Keyboard Layout:</label>";
        html += "<select name='layout'>";
        html += "<option value='0'" + String(layout==0?" selected":"") + ">en-gb (UK English)</option>";
        html += "<option value='1'" + String(layout==1?" selected":"") + ">en-us (US English)</option>";
        html += "<option value='2'" + String(layout==2?" selected":"") + ">pt-br (PT-BR ABNT2)</option>";
        html += "</select></div>";

        // Screensaver selector
        int saver = preferences.getInt("saver", 0);
        html += "<div><label>Screensaver Inactive Timeout:</label>";
        html += "<select name='saver'>";
        html += "<option value='0'" + String(saver==0?" selected":"") + ">Disabled</option>";
        html += "<option value='1'" + String(saver==1?" selected":"") + ">1 Minute</option>";
        html += "<option value='2'" + String(saver==2?" selected":"") + ">5 Minutes</option>";
        html += "<option value='3'" + String(saver==3?" selected":"") + ">10 Minutes</option>";
        html += "</select></div>";

        html += "<div></div></div></div>"; // end system card

        // Slots configuration cards
        const char* default_btn_labels[12] = {
            "[F1] Email", "[F2] User", "[F3] Dom 1", "[F4] Dom 2",
            "[F5] Pass 1", "[F6] Pass 2", "[F7] Mac 1", "[F8] Mac 2",
            "[F9] Macro 9", "[F10] Macro 10", "[F11] Macro 11", "[F12] Macro 12"
        };

        for(int i=0; i<12; i++) {
            String lbl_key = "lbl_" + String(i);
            String t_key = "type_" + String(i);
            String v_key = "val_" + String(i);
            
            String lbl = preferences.getString(lbl_key.c_str(), "");
            if (lbl == "") lbl = default_btn_labels[i];
            
            int type = preferences.getInt(t_key.c_str(), 0);
            String val = preferences.getString(v_key.c_str(), "");
            
            html += "<div class='card'>";
            html += "<h3>Slot " + String(i+1) + "</h3>";
            html += "<div class='grid-3'>";
            
            html += "<div><label>Button Label Text:</label><input type='text' name='" + lbl_key + "' value='" + lbl + "'></div>";
            
            html += "<div><label>Macro Type:</label><select name='" + t_key + "'>";
            html += "<option value='0'" + String(type==0?" selected":"") + ">String</option>";
            html += "<option value='1'" + String(type==1?" selected":"") + ">Password (XOR)</option>";
            html += "<option value='2'" + String(type==2?" selected":"") + ">Chord</option>";
            html += "</select></div>";
            
            html += "<div><label>Value / Key Combination:</label><input type='text' name='" + v_key + "' value='" + val + "'></div>";
            
            html += "</div></div>";
        }
        preferences.end();
        
        html += "<button type='submit'>Save Configuration</button></form></div></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        preferences.begin("macro", false);
        
        uint8_t mac_addr[6];
        esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
        
        for(int i=0; i<12; i++) {
            String lbl_key = "lbl_" + String(i);
            String t_key = "type_" + String(i);
            String v_key = "val_" + String(i);
            
            if(request->hasParam(lbl_key, true)) {
                String lbl_val = request->getParam(lbl_key, true)->value();
                preferences.putString(lbl_key.c_str(), lbl_val);
            }
            if(request->hasParam(t_key, true) && request->hasParam(v_key, true)) {
                int type = request->getParam(t_key, true)->value().toInt();
                String raw_val = request->getParam(v_key, true)->value();
                
                if(type == 1) { // Password -> encrypt with XOR
                    String cipher = "";
                    for(size_t j=0; j<raw_val.length(); j++) {
                        cipher += (char)(raw_val[j] ^ mac_addr[j % 6]);
                    }
                    raw_val = cipher;
                }
                
                preferences.putInt(t_key.c_str(), type);
                preferences.putString(v_key.c_str(), raw_val);
            }
        }

        if(request->hasParam("layout", true)) {
            int layout = request->getParam("layout", true)->value().toInt();
            preferences.putInt("layout", layout);
        }
        if(request->hasParam("saver", true)) {
            int saver = request->getParam("saver", true)->value().toInt();
            preferences.putInt("saver", saver);
        }
        
        preferences.end();
        
        // Immediately apply newly loaded settings
        apply_settings();
        
        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
        html += "<style>body{background-color: #0f0f13; color: #fff; font-family:sans-serif; margin:20px; text-align:center;} a{display:inline-block; padding:10px; background:#00ffff; color:#0f0f13; font-weight:bold; text-decoration:none; border-radius:5px; margin-top:20px;}</style></head>";
        html += "<body><h2>Configuration Saved! Rebooting board...</h2><br><a href='/'>Go Back</a></body></html>";
        request->send(200, "text/html", html);
        should_reboot = true;
    });

    server.begin();
    web_server_running = true;
    if (is_debug_enabled()) Serial.println("HTTP server started");
}
