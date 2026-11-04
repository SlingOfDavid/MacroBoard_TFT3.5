#include "web_server.h"
#include "macro_logic.h"
#include "macro_screen.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <Arduino.h>
#include <esp_mac.h>
#include <memory>

extern Preferences preferences;
static AsyncWebServer server(80);
static bool web_server_running = false;
static bool routes_registered = false;
bool should_reboot = false;

static String url_encode(const String &str) {
    String encoded = "";
    char hex[4];
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = (unsigned char)str[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += (char)c;
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

bool is_web_server_active() {
    return web_server_running;
}

void stop_web_server() {
    if (!web_server_running) return;
    Serial.println("Stopping Web Server...");
    server.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    web_server_running = false;
    resume_ble_after_wifi();
    Serial.println("Web Server stopped.");
}

void start_web_server() {
    if (web_server_running) return;
    Serial.printf("Starting Web Server... (Free Internal: %uB, Free PSRAM: %uB)\n",
                  (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    pause_ble_for_wifi();
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MacroBoard-Config", "12345678");

    wifi_config_t ap_config;
    memset(&ap_config, 0, sizeof(ap_config));
    strcpy((char *)ap_config.ap.ssid, "MacroBoard-Config");
    ap_config.ap.ssid_len = strlen("MacroBoard-Config");
    strcpy((char *)ap_config.ap.password, "12345678");
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_TKIP_CCMP;
    ap_config.ap.max_connection = 4;
    ap_config.ap.beacon_interval = 100;
    ap_config.ap.pmf_cfg.capable = false;
    ap_config.ap.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    WiFi.setTxPower(WIFI_POWER_15dBm);

    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());

    if (!routes_registered) {
        routes_registered = true;

        server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
            notify_sneaky_http_activity(false);
            int color_idx = get_tui_color_index();
            String accent = "#00ffff";
            String accent_hover = "#00cccc";
            if (color_idx == 1) {
                accent = "#ffff00"; accent_hover = "#cccc00";
            } else if (color_idx == 2) {
                accent = "#00ff00"; accent_hover = "#00cc00";
            } else if (color_idx == 3) {
                accent = "#ff00ff"; accent_hover = "#cc00cc";
            } else if (color_idx == 4) {
                accent = "#ff0000"; accent_hover = "#cc0000";
            }

            String html;
            html.reserve(4500);
            html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
            html += "<title>MacroBoard TUI Configuration</title><style>";
            html += "body{background:#0f0f13;color:#e2e2e9;font-family:sans-serif;margin:0;padding:16px;display:flex;justify-content:center}";
            html += ".c{max-width:800px;width:100%}";
            html += "h2{color:" + accent + ";border-bottom:2px solid " + accent + ";padding-bottom:8px;margin-bottom:16px}";
            html += "h3{color:" + accent + ";margin:0 0 12px;font-size:1.05em}";
            html += ".cd{background:#181822;border:1px solid #2a2a3a;border-radius:8px;padding:16px;margin-bottom:14px}";
            html += ".cd:hover{border-color:" + accent + "}";
            html += ".g3{display:grid;grid-template-columns:1fr 1fr 2fr;gap:12px}";
            html += "@media(max-width:600px){.g3{grid-template-columns:1fr}}";
            html += "label{display:block;margin-bottom:4px;font-size:.82em;color:#8a8a9a;font-weight:600}";
            html += "input,select{background:#0f0f13;border:1px solid #3a3a4a;border-radius:4px;color:#fff;padding:8px;width:100%;box-sizing:border-box}";
            html += "input:focus,select:focus{border-color:" + accent + ";outline:none}";
            html += "button{background:" + accent + ";color:#0f0f13;border:none;border-radius:6px;padding:12px;font-size:1.05em;font-weight:bold;width:100%;cursor:pointer}";
            html += "button:hover{background:" + accent_hover + "}";
            html += ".ins{background:#141824;border:1px solid " + accent + ";border-radius:8px;padding:12px;margin-bottom:16px;font-size:.88em;line-height:1.4}";
            html += "code{background:#0f0f13;color:" + accent + ";padding:1px 5px;border-radius:3px}";
            html += ".fl{list-style:none;padding:0;margin:0 0 12px}";
            html += ".fi{display:flex;justify-content:space-between;align-items:center;background:#0f0f13;border:1px solid #2a2a3a;border-radius:4px;padding:8px 12px;margin-bottom:6px}";
            html += ".dl{color:#ff5555;text-decoration:none;font-weight:bold;border:1px solid #ff5555;padding:3px 8px;border-radius:4px;font-size:.82em}";
            html += "</style></head><body><div class='c'><h2>MacroBoard TUI Configuration</h2>";

            html += "<div class='cd'><h3>SneaKeyboard - Memory Files (" + String(get_sneaky_fs_used_bytes()) + "/" + String(get_sneaky_fs_total_bytes()) + " B)</h3><ul class='fl'>";
            std::vector<SneakyFileInfo> sfiles = get_sneaky_files();
            if (sfiles.empty()) {
                html += "<li class='fi'><span>No files in memory.</span></li>";
            } else {
                for (const auto& f : sfiles) {
                    html += "<li class='fi'><span>" + f.name + " (" + String(f.size) + " B)</span>";
                    html += "<a class='dl' href='/delete?file=" + url_encode(f.name) + "'>Delete</a></li>";
                }
            }
            html += "</ul><form id='uf' method='POST' action='/upload' enctype='multipart/form-data'>";
            html += "<label>Upload Files (.txt, .md, .json, .xml, .html, ...):</label>";
            html += "<input id='fi' type='file' name='upload' multiple required style='margin-bottom:10px'>";
            html += "<button id='ub' type='submit'>Upload to Memory</button></form>";
            html += "<script>document.getElementById('uf').onsubmit=async function(e){var fi=document.getElementById('fi'),ub=document.getElementById('ub');if(!fi.files||!fi.files.length)return;e.preventDefault();ub.disabled=true;async function sc(u){for(var a=0;a<4;a++){try{var r=await fetch(u+'&a='+a,{cache:'no-store'});if(r.ok)return;}catch(err){if(a===3)throw err;}await new Promise(function(res){setTimeout(res,150);});}throw new Error('Chunk error');}try{for(var i=0;i<fi.files.length;i++){var f=fi.files[i],u8=new Uint8Array(await f.arrayBuffer()),tot=u8.length,fn=encodeURIComponent(f.name);if(tot===0){await sc('/up?f='+fn+'&o=0&t=0&d=');}else{for(var o=0;o<tot;o+=256){var end=Math.min(o+256,tot),hx='';for(var j=o;j<end;j++){var b=u8[j];hx+=(b<16?'0':'')+b.toString(16);}var pct=Math.floor((o*100)/tot);ub.textContent='Uploading '+(i+1)+'/'+fi.files.length+' ('+pct+'%)...';await sc('/up?f='+fn+'&o='+o+'&t='+tot+'&d='+hx);}}}ub.textContent='Saving...';await new Promise(function(res){setTimeout(res,450);});location.href='/';}catch(err){alert('Upload failed: '+err.message);ub.disabled=false;ub.textContent='Upload to Memory';}};</script></div>";

            // Instruction Box
            html += "<div class='ins'><strong>Chord Formatting:</strong> Separate keys with <code>+</code> (e.g. <code>Ctrl + Shift + a</code>). ";
            html += "Modifiers: <code>Ctrl</code>, <code>Shift</code>, <code>Alt</code>, <code>Gui</code>. ";
            html += "Keys: <code>Enter</code>, <code>Esc</code>, <code>Tab</code>, <code>Backspace</code>, <code>Delete</code>, <code>Insert</code>, <code>Home</code>, <code>End</code>, <code>PageUp</code>, <code>PageDown</code>, <code>Up</code>, <code>Down</code>, <code>Left</code>, <code>Right</code>, <code>F1</code>-<code>F24</code>.</div>";

            html += "<form action='/save' method='POST'>";

            preferences.begin("macro", true);

            // System Settings Card
            html += "<div class='cd'><h3>System Settings</h3><div class='g3'>";
            
            // Layout selector
            int layout = preferences.getInt("layout", 0);
            html += "<div><label>Keyboard Layout:</label><select name='layout'>";
            html += "<option value='0'" + String(layout==0?" selected":"") + ">en-gb (UK)</option>";
            html += "<option value='1'" + String(layout==1?" selected":"") + ">en-us (US)</option>";
            html += "<option value='2'" + String(layout==2?" selected":"") + ">pt-br (ABNT)</option></select></div>";

            // Screensaver selector
            int saver = preferences.getInt("saver", 0);
            html += "<div><label>Screensaver Timeout:</label><select name='saver'>";
            html += "<option value='0'" + String(saver==0?" selected":"") + ">Disabled</option>";
            html += "<option value='1'" + String(saver==1?" selected":"") + ">1 Minute</option>";
            html += "<option value='2'" + String(saver==2?" selected":"") + ">5 Minutes</option>";
            html += "<option value='3'" + String(saver==3?" selected":"") + ">10 Minutes</option></select></div>";

            int color = preferences.getInt("color", 0);
            html += "<div><label>Theme Color:</label><select name='color'>";
            html += "<option value='0'" + String(color==0?" selected":"") + ">Cyan</option>";
            html += "<option value='1'" + String(color==1?" selected":"") + ">Yellow</option>";
            html += "<option value='2'" + String(color==2?" selected":"") + ">Green</option>";
            html += "<option value='3'" + String(color==3?" selected":"") + ">Magenta</option>";
            html += "<option value='4'" + String(color==4?" selected":"") + ">Red</option></select></div></div></div>";

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
                
                html += "<div class='cd'><h3>Slot " + String(i+1) + "</h3><div class='g3'>";
                html += "<div><label>Label:</label><input type='text' name='" + lbl_key + "' value='" + lbl + "'></div>";
                html += "<div><label>Type:</label><select name='" + t_key + "'>";
                html += "<option value='0'" + String(type==0?" selected":"") + ">String</option>";
                html += "<option value='1'" + String(type==1?" selected":"") + ">Password (XOR)</option>";
                html += "<option value='2'" + String(type==2?" selected":"") + ">Chord</option></select></div>";
                html += "<div><label>Value:</label><input type='text' name='" + v_key + "' value='" + val + "'></div></div></div>";
            }
            preferences.end();
            
            html += "<button type='submit'>Save Configuration</button></form></div></body></html>";
            Serial.printf("[WebServer] Serving GET / (%u bytes)\n", (unsigned int)html.length());
            auto htmlPtr = std::make_shared<String>(std::move(html));
            AsyncWebServerResponse *response = request->beginChunkedResponse("text/html", [htmlPtr](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                if (index >= htmlPtr->length()) return 0;
                size_t toSend = htmlPtr->length() - index;
                if (toSend > maxLen) toSend = maxLen;
                if (toSend > 1400) toSend = 1400;
                memcpy(buffer, htmlPtr->c_str() + index, toSend);
                return toSend;
            });
            request->send(response);
        });

        // Chunked GET upload endpoint (/up) to avoid multi-segment TCP bursts on SoftAP
        server.on("/up", HTTP_GET, [](AsyncWebServerRequest *request) {
            if (!request->hasParam("f") || !request->hasParam("o") || !request->hasParam("t") || !request->hasParam("d")) {
                request->send(400, "text/plain", "Bad Request");
                return;
            }
            static String upName = "";
            static uint8_t *upBuf = nullptr;
            static size_t upTotal = 0;

            String fname = request->getParam("f")->value();
            size_t offset = (size_t)request->getParam("o")->value().toInt();
            size_t total = (size_t)request->getParam("t")->value().toInt();
            const String &hexStr = request->getParam("d")->value();

            if (offset == 0 || upBuf == nullptr || upName != fname || upTotal != total) {
                if (upBuf) {
                    heap_caps_free(upBuf);
                    upBuf = nullptr;
                }
                upName = fname;
                upTotal = total;
                size_t allocSize = (total > 0) ? total : 1;
                upBuf = (uint8_t *)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!upBuf) {
                    upName = "";
                    upTotal = 0;
                    request->send(500, "text/plain", "OOM");
                    return;
                }
            }

            size_t hexLen = hexStr.length();
            size_t chunkLen = hexLen / 2;
            const char *hex = hexStr.c_str();
            if (upBuf && chunkLen > 0 && (offset + chunkLen <= upTotal)) {
                for (size_t i = 0; i < chunkLen; i++) {
                    char c1 = hex[i * 2];
                    char c2 = hex[i * 2 + 1];
                    uint8_t v1 = (c1 >= 'a') ? (c1 - 'a' + 10) : ((c1 >= 'A') ? (c1 - 'A' + 10) : (c1 - '0'));
                    uint8_t v2 = (c2 >= 'a') ? (c2 - 'a' + 10) : ((c2 >= 'A') ? (c2 - 'A' + 10) : (c2 - '0'));
                    upBuf[offset + i] = (uint8_t)((v1 << 4) | v2);
                }
            }

            if (offset + chunkLen >= upTotal) {
                uint8_t *finalBuf = upBuf;
                size_t finalTotal = upTotal;
                upBuf = nullptr;
                upTotal = 0;
                upName = "";
                queue_sneaky_file_write(fname, finalBuf, finalTotal);
            } else {
                notify_sneaky_http_activity(true);
            }

            AsyncWebServerResponse *resp = request->beginResponse(200, "text/plain", "OK");
            resp->addHeader("Cache-Control", "no-store");
            request->send(resp);
        });

        server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
            notify_sneaky_http_activity(false);
            if (request->hasParam("name")) {
                request->send(200, "text/plain", "OK");
            } else {
                request->redirect("/");
            }
        }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            static uint8_t *mpBuf = nullptr;
            static size_t mpCap = 0;
            static size_t mpLen = 0;
            if (!index) {
                request->client()->setRxTimeout(30);
                notify_sneaky_http_activity(true);
                if (mpBuf) {
                    heap_caps_free(mpBuf);
                    mpBuf = nullptr;
                }
                size_t initCap = request->contentLength() > 0 ? request->contentLength() : 16384;
                mpBuf = (uint8_t *)heap_caps_malloc(initCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                mpCap = mpBuf ? initCap : 0;
                mpLen = 0;
            }
            if (mpBuf && len) {
                notify_sneaky_http_activity(true);
                if (mpLen + len > mpCap) {
                    size_t newCap = (mpCap * 2 > mpLen + len) ? (mpCap * 2) : (mpLen + len + 8192);
                    uint8_t *newBuf = (uint8_t *)heap_caps_realloc(mpBuf, newCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (newBuf) {
                        mpBuf = newBuf;
                        mpCap = newCap;
                    }
                }
                if (mpLen + len <= mpCap) {
                    memcpy(mpBuf + mpLen, data, len);
                    mpLen += len;
                }
            }
            if (final) {
                uint8_t *outBuf = mpBuf;
                size_t outLen = mpLen;
                mpBuf = nullptr;
                mpCap = 0;
                mpLen = 0;
                queue_sneaky_file_write(filename, outBuf, outLen);
            }
        }, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (!index) {
                request->client()->setRxTimeout(30);
                notify_sneaky_http_activity(true);
                if (request->_tempObject != NULL) {
                    free(request->_tempObject);
                    request->_tempObject = NULL;
                }
                size_t allocSize = (total > 0) ? total : 1;
                request->_tempObject = heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (request->_tempObject != NULL && len > 0 && (index + len <= total)) {
                notify_sneaky_http_activity(true);
                memcpy((uint8_t *)(request->_tempObject) + index, data, len);
            }
            if (index + len == total) {
                String fname = request->hasParam("name") ? request->getParam("name")->value() : "upload.txt";
                uint8_t *buf = (uint8_t *)(request->_tempObject);
                request->_tempObject = NULL;
                queue_sneaky_file_write(fname, buf, total);
            }
        });

        server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
            notify_sneaky_http_activity(false);
            if (request->hasParam("file")) {
                String fileName = request->getParam("file")->value();
                delete_sneaky_file(fileName);
                request->redirect("/");
            } else {
                request->send(400, "text/plain", "Missing file parameter.");
            }
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
            if(request->hasParam("color", true)) {
                int color = request->getParam("color", true)->value().toInt();
                preferences.putInt("color", color);
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
    }

    server.begin();
    web_server_running = true;
    if (is_debug_enabled()) Serial.println("HTTP server started");
}
