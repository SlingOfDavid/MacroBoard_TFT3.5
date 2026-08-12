#include "macro_screen.h"
#include "macro_logic.h"
#include "web_server.h"
#include <Arduino.h>
#include <Preferences.h>

extern Preferences preferences;

lv_obj_t* macro_btns[12];

static lv_obj_t * main_scr = NULL;
static lv_obj_t * screensaver_scr = NULL;
static lv_timer_t * screensaver_timer = NULL;
static uint32_t screensaver_timeout_ms = 0;
static bool screensaver_active = false;
static uint32_t screensaver_exit_time = 0;
static lv_obj_t * ble_icon_lbl = NULL;
static lv_obj_t * wifi_icon_lbl = NULL;
static lv_obj_t * afk_icon_lbl = NULL;

static lv_color_t get_tui_color() {
    int idx = get_tui_color_index();
    switch (idx) {
        case 1: return lv_color_make(255, 255, 0);   // Yellow
        case 2: return lv_color_make(0, 255, 0);     // Green
        case 3: return lv_color_make(255, 0, 255);   // Magenta
        case 4: return lv_color_make(255, 0, 0);     // Red
        case 0:
        default: return lv_color_make(0, 255, 255);  // Cyan
    }
}

void set_screensaver_timeout(uint32_t timeout_ms) {
    screensaver_timeout_ms = timeout_ms;
    if (is_debug_enabled()) Serial.printf("Screensaver timeout set to %d ms\n", timeout_ms);
}

static void macro_btn_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        // Ignore accidental clicks for 333ms after exiting screensaver
        if (millis() - screensaver_exit_time < 333) {
            return;
        }
        int index = (int)(intptr_t)lv_event_get_user_data(e);
        if (is_debug_enabled()) Serial.printf("Macro Button %d clicked\n", index);
        execute_macro(index);
    }
}

static void lang_dd_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dd = lv_event_get_target(e);
        int selected = lv_dropdown_get_selected(dd);
        if (is_debug_enabled()) Serial.printf("Language layout selected: %d\n", selected);
        
        preferences.begin("macro", false);
        preferences.putInt("layout", selected);
        preferences.end();
        
        apply_settings();
    }
}

static void saver_dd_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dd = lv_event_get_target(e);
        int selected = lv_dropdown_get_selected(dd);
        if (is_debug_enabled()) Serial.printf("Saver timeout selected: %d\n", selected);
        
        preferences.begin("macro", false);
        preferences.putInt("saver", selected);
        preferences.end();
        
        apply_settings();
    }
}

static void reload_ui_async(void * p) {
    setup_macro_screen();
}

static void color_dd_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dd = lv_event_get_target(e);
        int selected = lv_dropdown_get_selected(dd);
        if (is_debug_enabled()) Serial.printf("Theme color selected: %d\n", selected);
        
        preferences.begin("macro", false);
        preferences.putInt("color", selected);
        preferences.end();
        
        apply_settings();
        lv_async_call(reload_ui_async, NULL);
    }
}

static void debug_dd_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dd = lv_event_get_target(e);
        int selected = lv_dropdown_get_selected(dd);
        
        preferences.begin("macro", false);
        preferences.putInt("debug", selected);
        preferences.end();
        
        apply_settings();
    }
}

static int ss_text_index = 0;
static bool ss_typing = true;
static int ss_blink_count = 0;
static int ss_x = 50;
static int ss_y = 50;

static void screensaver_timer_cb(lv_timer_t * timer) {
    lv_obj_t * label = (lv_obj_t *)timer->user_data;
    
    const char * full_text = "MACROBOARD TUI V1.0";
    int full_len = strlen(full_text);
    
    if (ss_typing) {
        char buf[64];
        int len = ss_text_index < full_len ? ss_text_index + 1 : full_len;
        strncpy(buf, full_text, len);
        buf[len] = '\0';
        strcat(buf, " _"); // Standard cursor character
        
        lv_label_set_text(label, buf);
        ss_text_index++;
        
        if (ss_text_index > full_len) {
            ss_typing = false;
            ss_blink_count = 0;
        }
    } else {
        if (ss_blink_count % 2 == 0) {
            char buf[64];
            strcpy(buf, full_text);
            strcat(buf, " _");
            lv_label_set_text(label, buf);
        } else {
            lv_label_set_text(label, full_text);
        }
        ss_blink_count++;
        
        if (ss_blink_count > 10) {
            ss_typing = true;
            ss_text_index = 0;
            ss_blink_count = 0;
            
            // Random coordinate chosen safely to avoid overflow off limits
            ss_x = rand() % 240 + 20; // range 20-260
            ss_y = rand() % 220 + 20; // range 20-240
            lv_obj_set_pos(label, ss_x, ss_y);
            lv_label_set_text(label, "");
        }
    }
}

static void start_screensaver() {
    if (screensaver_active) return;
    screensaver_active = true;

    // Reset animation state
    ss_text_index = 0;
    ss_typing = true;
    ss_blink_count = 0;
    ss_x = 50;
    ss_y = 50;

    screensaver_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screensaver_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screensaver_scr, LV_OPA_COVER, 0);

    lv_color_t ss_color;
    if (is_afk_enabled()) {
        int theme_idx = get_tui_color_index();
        // ponytail: if current theme is Yellow (1), shift AFK screensaver color to Cyan, else Yellow
        if (theme_idx == 1) {
            ss_color = lv_color_make(0, 255, 255); // Cyan
        } else {
            ss_color = lv_color_make(255, 255, 0); // Yellow
        }
    } else {
        ss_color = get_tui_color();
    }

    lv_obj_t * label = lv_label_create(screensaver_scr);
    lv_obj_set_style_text_color(label, ss_color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(label, ss_x, ss_y);
    lv_label_set_text(label, "");

    lv_obj_add_event_cb(screensaver_scr, [](lv_event_t * e) {
        lv_scr_load(main_scr);
        lv_obj_del_async(screensaver_scr); // Safe asynchronous deletion
        screensaver_scr = NULL;
        
        if (screensaver_timer) {
            lv_timer_del(screensaver_timer);
            screensaver_timer = NULL;
        }
        
        lv_disp_trig_activity(NULL);
        screensaver_active = false;
        screensaver_exit_time = millis(); // Record exit time to prevent accidental clicks
    }, LV_EVENT_PRESSED, NULL);

    lv_scr_load(screensaver_scr);
    screensaver_timer = lv_timer_create(screensaver_timer_cb, 150, label);
}

void setup_macro_screen() {
    main_scr = lv_scr_act();
    lv_obj_clean(main_scr); 

    lv_color_t tui_color = get_tui_color();

    // Apply main screen style (pure black background)
    static lv_style_t style_scr;
    lv_style_init(&style_scr);
    lv_style_set_bg_color(&style_scr, lv_color_black());
    lv_style_set_bg_opa(&style_scr, LV_OPA_COVER);
    lv_obj_add_style(main_scr, &style_scr, 0);

    // Create Fixed Header Panel (TUI status bar)
    lv_obj_t * header = lv_obj_create(main_scr);
    lv_obj_set_size(header, 480, 35);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, tui_color, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Header Label
    lv_obj_t * header_lbl = lv_label_create(header);
    lv_label_set_text(header_lbl, " MACROBOARD TUI v1.0");
    lv_obj_set_style_text_color(header_lbl, tui_color, 0);
    lv_obj_set_style_text_font(header_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(header_lbl, LV_ALIGN_LEFT_MID, 10, 0);

    // Bluetooth Icon
    ble_icon_lbl = lv_label_create(header);
    lv_label_set_text(ble_icon_lbl, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(ble_icon_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(ble_icon_lbl, LV_ALIGN_RIGHT_MID, -10, 0);

    // Wi-Fi Icon
    wifi_icon_lbl = lv_label_create(header);
    lv_label_set_text(wifi_icon_lbl, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(wifi_icon_lbl, LV_ALIGN_RIGHT_MID, -35, 0);

    // AFK Indicator Icon (Keyboard symbol + AFK, placed left of Wi-Fi icon)
    afk_icon_lbl = lv_label_create(header);
    lv_label_set_text(afk_icon_lbl, LV_SYMBOL_KEYBOARD " AFK");
    lv_obj_set_style_text_font(afk_icon_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(afk_icon_lbl, LV_ALIGN_RIGHT_MID, -65, 0);
    if (!is_afk_enabled()) {
        lv_obj_add_flag(afk_icon_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // Create Tileview for horizontal swipe navigation below the header
    lv_obj_t * tv = lv_tileview_create(main_scr);
    lv_obj_set_size(tv, 480, 285);
    lv_obj_set_pos(tv, 0, 35);
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);
    lv_obj_set_style_border_width(tv, 0, 0);

    // Add horizontal tiles (0: AFK Mode, 1: Macro Board, 2: System Config)
    lv_obj_t * tile_afk    = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    lv_obj_t * tile_macros = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
    lv_obj_t * tile_config = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);

    // Default active tile to Macro Board (1, 0)
    lv_obj_set_tile_id(tv, 1, 0, LV_ANIM_OFF);

    // Button Styles
    static lv_style_t style_btn;
    lv_style_init(&style_btn);
    lv_style_set_radius(&style_btn, 0); // Sharp corners
    lv_style_set_bg_color(&style_btn, lv_color_black());
    lv_style_set_bg_opa(&style_btn, LV_OPA_COVER);
    lv_style_set_border_width(&style_btn, 2);
    lv_style_set_border_color(&style_btn, tui_color);
    lv_style_set_text_color(&style_btn, tui_color);
    lv_style_set_text_font(&style_btn, &lv_font_montserrat_14);

    static lv_style_t style_btn_pressed;
    lv_style_init(&style_btn_pressed);
    lv_style_set_bg_color(&style_btn_pressed, tui_color);
    lv_style_set_bg_opa(&style_btn_pressed, LV_OPA_COVER);
    lv_style_set_text_color(&style_btn_pressed, lv_color_black());
    lv_style_set_border_color(&style_btn_pressed, tui_color);

    // ==========================================
    // TILE 0: AFK Controller & Typer Mode
    // ==========================================
    lv_obj_t * afk_title = lv_label_create(tile_afk);
    lv_label_set_text(afk_title, "[ AUTOMATION MODES ]");
    lv_obj_set_style_text_color(afk_title, tui_color, 0);
    lv_obj_set_style_text_font(afk_title, &lv_font_montserrat_14, 0);
    lv_obj_align(afk_title, LV_ALIGN_TOP_MID, 0, 10);

    // AFK Mode Button
    lv_obj_t * afk_btn = lv_btn_create(tile_afk);
    lv_obj_set_size(afk_btn, 320, 44);
    lv_obj_set_pos(afk_btn, 80, 40);
    lv_obj_add_flag(afk_btn, LV_OBJ_FLAG_CHECKABLE);
    if (is_afk_enabled()) {
        lv_obj_add_state(afk_btn, LV_STATE_CHECKED);
    }
    lv_obj_add_style(afk_btn, &style_btn, LV_STATE_DEFAULT);
    lv_obj_add_style(afk_btn, &style_btn_pressed, LV_STATE_CHECKED);
    lv_obj_add_style(afk_btn, &style_btn_pressed, LV_STATE_PRESSED);

    lv_obj_t * afk_btn_lbl = lv_label_create(afk_btn);
    lv_label_set_text(afk_btn_lbl, is_afk_enabled() ? "[ AFK MODE: ENABLED ]" : "[ AFK MODE: DISABLED ]");
    lv_obj_center(afk_btn_lbl);

    // Typer Mode Button
    lv_obj_t * typer_btn = lv_btn_create(tile_afk);
    lv_obj_set_size(typer_btn, 320, 44);
    lv_obj_set_pos(typer_btn, 80, 95);
    lv_obj_add_flag(typer_btn, LV_OBJ_FLAG_CHECKABLE);
    if (is_typer_enabled()) {
        lv_obj_add_state(typer_btn, LV_STATE_CHECKED);
    }
    lv_obj_add_style(typer_btn, &style_btn, LV_STATE_DEFAULT);
    lv_obj_add_style(typer_btn, &style_btn_pressed, LV_STATE_CHECKED);
    lv_obj_add_style(typer_btn, &style_btn_pressed, LV_STATE_PRESSED);

    lv_obj_t * typer_btn_lbl = lv_label_create(typer_btn);
    lv_label_set_text(typer_btn_lbl, is_typer_enabled() ? "[ TYPER MODE: ENABLED ]" : "[ TYPER MODE: DISABLED ]");
    lv_obj_center(typer_btn_lbl);

    // AFK Event Handler
    struct BtnPair {
        lv_obj_t * own_lbl;
        lv_obj_t * other_btn;
        lv_obj_t * other_lbl;
    };
    static BtnPair afk_pair;
    afk_pair.own_lbl = afk_btn_lbl;
    afk_pair.other_btn = typer_btn;
    afk_pair.other_lbl = typer_btn_lbl;

    lv_obj_add_event_cb(afk_btn, [](lv_event_t * e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_CLICKED) {
            lv_obj_t * btn = lv_event_get_target(e);
            BtnPair * pair = (BtnPair *)lv_event_get_user_data(e);
            bool is_checked = lv_obj_has_state(btn, LV_STATE_CHECKED);
            set_afk_enabled(is_checked);
            if (pair->own_lbl) {
                lv_label_set_text(pair->own_lbl, is_checked ? "[ AFK MODE: ENABLED ]" : "[ AFK MODE: DISABLED ]");
            }
            if (is_checked && pair->other_btn) {
                lv_obj_clear_state(pair->other_btn, LV_STATE_CHECKED);
                if (pair->other_lbl) {
                    lv_label_set_text(pair->other_lbl, "[ TYPER MODE: DISABLED ]");
                }
            }
        }
    }, LV_EVENT_ALL, &afk_pair);

    // Typer Event Handler
    static BtnPair typer_pair;
    typer_pair.own_lbl = typer_btn_lbl;
    typer_pair.other_btn = afk_btn;
    typer_pair.other_lbl = afk_btn_lbl;

    lv_obj_add_event_cb(typer_btn, [](lv_event_t * e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_CLICKED) {
            lv_obj_t * btn = lv_event_get_target(e);
            BtnPair * pair = (BtnPair *)lv_event_get_user_data(e);
            bool is_checked = lv_obj_has_state(btn, LV_STATE_CHECKED);
            set_typer_enabled(is_checked);
            if (pair->own_lbl) {
                lv_label_set_text(pair->own_lbl, is_checked ? "[ TYPER MODE: ENABLED ]" : "[ TYPER MODE: DISABLED ]");
            }
            if (is_checked && pair->other_btn) {
                lv_obj_clear_state(pair->other_btn, LV_STATE_CHECKED);
                if (pair->other_lbl) {
                    lv_label_set_text(pair->other_lbl, "[ AFK MODE: DISABLED ]");
                }
            }
        }
    }, LV_EVENT_ALL, &typer_pair);

    lv_obj_t * mode_desc = lv_label_create(tile_afk);
    lv_label_set_text(mode_desc, "Modes are mutually exclusive."); //old text: "Typer Mode types text files from MicroSD (/typer).\nAFK Mode presses safe random keys periodically."
    lv_obj_set_style_text_color(mode_desc, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(mode_desc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(mode_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mode_desc, LV_ALIGN_TOP_MID, 0, 150);

    lv_obj_t * afk_swipe_hint = lv_label_create(tile_afk);
    lv_label_set_text(afk_swipe_hint, "Swipe left to view Macro Board >>");
    lv_obj_set_style_text_color(afk_swipe_hint, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(afk_swipe_hint, &lv_font_montserrat_12, 0);
    lv_obj_align(afk_swipe_hint, LV_ALIGN_BOTTOM_MID, 0, -10);


    // ==========================================
    // TILE 1: Macro Board Buttons
    // ==========================================
    const char* default_btn_labels[12] = {
        "[F1] Email", "[F2] User", "[F3] Dom 1", "[F4] Dom 2",
        "[F5] Pass 1", "[F6] Pass 2", "[F7] Mac 1", "[F8] Mac 2",
        "[F9] Macro 9", "[F10] Macro 10", "[F11] Macro 11", "[F12] Macro 12"
    };

    preferences.begin("macro", true);
    
    // Create 3x4 Grid of Macro Buttons on tile_macros
    int start_x = 15;
    int start_y = 15;
    int spacing_x = 10;
    int spacing_y = 10;
    int btn_w = 105;
    int btn_h = 75;

    for (int i = 0; i < 12; i++) {
        int row = i / 4;
        int col = i % 4;

        lv_obj_t * btn = lv_btn_create(tile_macros);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_set_pos(btn, start_x + (col * (btn_w + spacing_x)), start_y + (row * (btn_h + spacing_y)));
        
        lv_obj_add_style(btn, &style_btn, LV_STATE_DEFAULT);
        lv_obj_add_style(btn, &style_btn_pressed, LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, macro_btn_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        
        lv_obj_t * label = lv_label_create(btn);
        
        // Load custom label text from preferences
        String t_key = "lbl_" + String(i);
        String saved_lbl = preferences.getString(t_key.c_str(), "");
        if (saved_lbl == "") {
            saved_lbl = default_btn_labels[i];
        }
        
        lv_label_set_text(label, saved_lbl.c_str());
        lv_obj_center(label);

        macro_btns[i] = btn;
    }

    // ==========================================
    // TILE 2: System Configuration Page
    // ==========================================
    lv_obj_t * config_title = lv_label_create(tile_config);
    lv_label_set_text(config_title, "[ SYSTEM CONFIGURATION ]");
    lv_obj_set_style_text_color(config_title, tui_color, 0);
    lv_obj_set_style_text_font(config_title, &lv_font_montserrat_14, 0);
    lv_obj_align(config_title, LV_ALIGN_TOP_MID, 0, 10);

    // Keyboard Layout Label and Dropdown
    lv_obj_t * lang_lbl = lv_label_create(tile_config);
    lv_label_set_text(lang_lbl, "Keyboard Layout:");
    lv_obj_set_style_text_color(lang_lbl, tui_color, 0);
    lv_obj_set_style_text_font(lang_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lang_lbl, 40, 40);

    lv_obj_t * dd_lang = lv_dropdown_create(tile_config);
    lv_dropdown_set_options(dd_lang, "en-gb\nen-us\npt-br");
    lv_obj_set_size(dd_lang, 160, 34);
    lv_obj_set_pos(dd_lang, 240, 34);
    lv_obj_add_style(dd_lang, &style_btn, 0);
    int current_lang = preferences.getInt("layout", 0);
    lv_dropdown_set_selected(dd_lang, current_lang);
    lv_obj_add_event_cb(dd_lang, lang_dd_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Screensaver Timeout Label and Dropdown
    lv_obj_t * saver_lbl = lv_label_create(tile_config);
    lv_label_set_text(saver_lbl, "Screensaver Idle:");
    lv_obj_set_style_text_color(saver_lbl, tui_color, 0);
    lv_obj_set_style_text_font(saver_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(saver_lbl, 40, 82);

    lv_obj_t * dd_saver = lv_dropdown_create(tile_config);
    lv_dropdown_set_options(dd_saver, "Disabled\n1 Minute\n5 Minutes\n10 Minutes");
    lv_obj_set_size(dd_saver, 160, 34);
    lv_obj_set_pos(dd_saver, 240, 76);
    lv_obj_add_style(dd_saver, &style_btn, 0);
    int current_saver = preferences.getInt("saver", 0);
    lv_dropdown_set_selected(dd_saver, current_saver);
    lv_obj_add_event_cb(dd_saver, saver_dd_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // TUI Theme Color Dropdown
    lv_obj_t * color_lbl = lv_label_create(tile_config);
    lv_label_set_text(color_lbl, "Theme Color:");
    lv_obj_set_style_text_color(color_lbl, tui_color, 0);
    lv_obj_set_style_text_font(color_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(color_lbl, 40, 124);

    lv_obj_t * dd_color = lv_dropdown_create(tile_config);
    lv_dropdown_set_options(dd_color, "Cyan\nYellow\nGreen\nMagenta\nRed");
    lv_obj_set_size(dd_color, 160, 34);
    lv_obj_set_pos(dd_color, 240, 118);
    lv_obj_add_style(dd_color, &style_btn, 0);
    int current_color = preferences.getInt("color", 0);
    lv_dropdown_set_selected(dd_color, current_color);
    lv_obj_add_event_cb(dd_color, color_dd_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Debug Mode Dropdown
    lv_obj_t * debug_lbl = lv_label_create(tile_config);
    lv_label_set_text(debug_lbl, "Debug Output:");
    lv_obj_set_style_text_color(debug_lbl, tui_color, 0);
    lv_obj_set_style_text_font(debug_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(debug_lbl, 40, 166);

    lv_obj_t * dd_debug = lv_dropdown_create(tile_config);
    lv_dropdown_set_options(dd_debug, "Disabled\nEnabled");
    lv_obj_set_size(dd_debug, 160, 34);
    lv_obj_set_pos(dd_debug, 240, 160);
    lv_obj_add_style(dd_debug, &style_btn, 0);
    int current_debug = preferences.getInt("debug", 0);
    lv_dropdown_set_selected(dd_debug, current_debug);
    lv_obj_add_event_cb(dd_debug, debug_dd_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Start Config AP button
    lv_obj_t * config_btn = lv_btn_create(tile_config);
    lv_obj_set_size(config_btn, 320, 36);
    lv_obj_set_pos(config_btn, 80, 202);
    
    lv_obj_add_style(config_btn, &style_btn, LV_STATE_DEFAULT);
    lv_obj_add_style(config_btn, &style_btn_pressed, LV_STATE_PRESSED);

    lv_obj_t * config_label = lv_label_create(config_btn);
    lv_label_set_text(config_label, is_web_server_active() ? "[ AP ACTIVE: MacroBoard-Config ]" : "[ START CONFIG WEB SERVER ]");
    lv_obj_center(config_label);

    lv_obj_add_event_cb(config_btn, [](lv_event_t * e){
        lv_obj_t * btn = lv_event_get_target(e);
        lv_obj_t * lbl = lv_obj_get_child(btn, 0);
        if (is_web_server_active()) {
            stop_web_server();
            lv_label_set_text(lbl, "[ START CONFIG WEB SERVER ]");
        } else {
            start_web_server();
            lv_label_set_text(lbl, "[ AP ACTIVE: MacroBoard-Config ]");
        }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t * swipe_hint = lv_label_create(tile_config);
    lv_label_set_text(swipe_hint, "<< Swipe right to view Macro Board");
    lv_obj_set_style_text_color(swipe_hint, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(swipe_hint, &lv_font_montserrat_12, 0);
    lv_obj_align(swipe_hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    preferences.end();

    // Register status loop timer (200ms tick)
    lv_timer_create([](lv_timer_t * timer) {
        lv_color_t active_theme = get_tui_color();

        if (is_afk_enabled()) {
            lv_label_set_text(afk_icon_lbl, LV_SYMBOL_KEYBOARD " AFK");
            lv_obj_set_style_text_color(afk_icon_lbl, active_theme, 0);
            lv_obj_clear_flag(afk_icon_lbl, LV_OBJ_FLAG_HIDDEN);
        } else if (is_typer_enabled()) {
            lv_label_set_text(afk_icon_lbl, LV_SYMBOL_EDIT " TYPE");
            lv_obj_set_style_text_color(afk_icon_lbl, active_theme, 0);
            lv_obj_clear_flag(afk_icon_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(afk_icon_lbl, LV_OBJ_FLAG_HIDDEN);
        }


        if (is_ble_connected()) {
            lv_obj_set_style_text_color(ble_icon_lbl, active_theme, 0);
        } else {
            lv_obj_set_style_text_color(ble_icon_lbl, lv_color_make(80, 80, 80), 0);
        }
        
        if (is_web_server_active()) {
            lv_obj_set_style_text_color(wifi_icon_lbl, active_theme, 0);
        } else {
            lv_obj_set_style_text_color(wifi_icon_lbl, lv_color_make(80, 80, 80), 0);
        }

        if (screensaver_timeout_ms > 0 && !screensaver_active) {
            if (lv_disp_get_inactive_time(NULL) > screensaver_timeout_ms) {
                start_screensaver();
            }
        }
    }, 200, NULL);
}
