#include "macro_screen.h"
#include "ui.h"
#include "web_server.h"
#include <Arduino.h>

extern const lv_img_dsc_t ui_img_email_png;
extern const lv_img_dsc_t ui_img_contacts_png;
extern const lv_img_dsc_t ui_img_browser_png;
extern const lv_img_dsc_t ui_img_settings_png; // Password fallback
extern const lv_img_dsc_t ui_img_notes_png;    // Shortcut fallback

lv_obj_t* macro_btns[8];

static void macro_btn_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        int index = (int)(intptr_t)lv_event_get_user_data(e);
        Serial.printf("Macro Button %d clicked\n", index);
        execute_macro(index);
    }
}

void setup_macro_screen() {
    // ui_home2 is the screen accessed by swiping left
    lv_obj_clean(ui_home2); 
    
    const lv_img_dsc_t* icons[8] = {
        &ui_img_email_png,
        &ui_img_contacts_png,
        &ui_img_browser_png,
        &ui_img_browser_png,
        &ui_img_settings_png, // keys / password placeholder
        &ui_img_settings_png, // keys / password placeholder
        &ui_img_notes_png,    // shortcut placeholder
        &ui_img_notes_png     // shortcut placeholder
    };

    const char* labels[8] = {
        "Email", "User", "Domain 1", "Domain 2",
        "Pass 1", "Pass 2", "Macro 1", "Macro 2"
    };

    int start_x = -105;
    int start_y = -70;
    int spacing_x = 70;
    int spacing_y = 100;

    for (int i = 0; i < 8; i++) {
        int row = i / 4;
        int col = i % 4;

        lv_obj_t * img = lv_img_create(ui_home2);
        lv_img_set_src(img, icons[i]);
        lv_obj_set_align(img, LV_ALIGN_CENTER);
        lv_obj_set_x(img, start_x + (col * spacing_x));
        lv_obj_set_y(img, start_y + (row * spacing_y));
        lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(img, macro_btn_event_cb, LV_EVENT_ALL, (void*)(intptr_t)i);
        macro_btns[i] = img;

        lv_obj_t * label = lv_label_create(ui_home2);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_font(label, &ui_font_misans16, 0); 
        lv_obj_set_align(label, LV_ALIGN_CENTER);
        lv_obj_set_x(label, start_x + (col * spacing_x));
        lv_obj_set_y(label, start_y + (row * spacing_y) + 35);
    }

    // Add a button to the existing Settings menu (ui_setting) to launch Web Config AP
    lv_obj_t * config_btn = lv_btn_create(ui_setting);
    lv_obj_set_size(config_btn, 200, 50);
    lv_obj_align(config_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_t * config_label = lv_label_create(config_btn);
    lv_label_set_text(config_label, "Start Web Config AP");
    lv_obj_center(config_label);
    lv_obj_add_event_cb(config_btn, [](lv_event_t * e){
        start_web_server();
        lv_obj_t * btn = lv_event_get_target(e);
        lv_obj_t * lbl = lv_obj_get_child(btn, 0);
        lv_label_set_text(lbl, "AP Started: MacroBoard-Config");
    }, LV_EVENT_CLICKED, NULL);
}
