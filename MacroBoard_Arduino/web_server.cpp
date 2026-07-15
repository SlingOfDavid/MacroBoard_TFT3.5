#include "web_server.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <Arduino.h>
#include <esp_mac.h>

extern Preferences preferences;
static AsyncWebServer server(80);

void start_web_server() {
    Serial.println("Starting Web Server...");
    WiFi.softAP("MacroBoard-Config", "12345678");
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
        html += "<style>body{font-family:sans-serif;margin:20px;} input,select{margin-bottom:10px;padding:8px;width:100%;box-sizing:border-box;} button{padding:10px;background:#007bff;color:white;border:none;border-radius:5px;width:100%;}</style></head>";
        html += "<body><h2>MacroBoard Config</h2>";
        html += "<form action='/save' method='POST'>";
        
        preferences.begin("macro", true);
        for(int i=0; i<8; i++) {
            String t_key = "type_" + String(i);
            String v_key = "val_" + String(i);
            int type = preferences.getInt(t_key.c_str(), 0);
            String val = preferences.getString(v_key.c_str(), "");
            
            html += "<h3>Slot " + String(i+1) + "</h3>";
            html += "<label>Type:</label><select name='" + t_key + "'>";
            html += "<option value='0'" + String(type==0?" selected":"") + ">String</option>";
            html += "<option value='1'" + String(type==1?" selected":"") + ">Password (XOR)</option>";
            html += "<option value='2'" + String(type==2?" selected":"") + ">Chord</option>";
            html += "</select>";
            html += "<label>Value:</label><input type='text' name='" + v_key + "' value='" + val + "'>";
        }
        preferences.end();
        
        html += "<button type='submit'>Save Configuration</button></form></body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        preferences.begin("macro", false);
        
        uint8_t mac_addr[6];
        esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
        
        for(int i=0; i<8; i++) {
            String t_key = "type_" + String(i);
            String v_key = "val_" + String(i);
            
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
        preferences.end();
        
        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
        html += "<style>body{font-family:sans-serif;margin:20px;text-align:center;} a{display:inline-block;padding:10px;background:#28a745;color:white;text-decoration:none;border-radius:5px;}</style></head>";
        html += "<body><h2>Configuration Saved!</h2><br><a href='/'>Go Back</a></body></html>";
        request->send(200, "text/html", html);
    });

    server.begin();
    Serial.println("HTTP server started");
}
