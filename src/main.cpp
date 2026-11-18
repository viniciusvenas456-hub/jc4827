#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <Wire.h>
#include <TAMC_GT911.h>

// ================= CONFIGURAÇÕES DE REDE =================
const char* WIFI_SSID = "SUA_REDE_WIFI";
const char* WIFI_PASS = "SUA_SENHA_WIFI";
String MOONRAKER_IP   = "192.168.1.100"; // Coloque o IP do seu Klipper
const int MOONRAKER_PORT = 7125;

// ================= PINOS OFICIAIS (DO SEU JSON) =================
#define LCD_CS  45
#define LCD_CLK 47
#define LCD_D0  21
#define LCD_D1  48
#define LCD_D2  40
#define LCD_D3  39
#define LCD_BL  1

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_CLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_NV3041A(bus, GFX_NOT_DEFINED, 0 /* rotação */, true /* IPS */);

// Touch Capacitivo GT911 (JC4827W543C)
#define TOUCH_SDA  8
#define TOUCH_SCL  4
#define TOUCH_INT  3
#define TOUCH_RST 38
TAMC_GT911 ts = TAMC_GT911(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, 480, 272);

// ================= VARIÁVEIS LVGL E LAYOUT CYD =================
#define SCREEN_W 480
#define SCREEN_H 272
#define SIDEBAR_W 50
#define GAP_PX 8

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf;
static lv_disp_drv_t disp_drv;

// Containers das Abas
lv_obj_t *panel_temp;
lv_obj_t *panel_move;
lv_obj_t *panel_macros;
lv_obj_t *panel_sys;

// Elementos de Interface atualizados via Moonraker
lv_obj_t *lbl_noz_val;
lv_obj_t *lbl_bed_val;
lv_obj_t *lbl_state;
lv_obj_t *bar_progress;
lv_obj_t *lbl_progress;
lv_obj_t *lbl_pos_xyz;
lv_obj_t *lbl_ip_info;
lv_obj_t *macro_grid;

float current_step = 10.0;
bool macros_loaded = false;

// ================= DRIVERS DE TELA E TOUCH =================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    ts.read();
    if (ts.isTouched) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = ts.points[0].x;
        data->point.y = ts.points[0].y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ================= COMUNICAÇÃO MOONRAKER API =================
void send_gcode(String script) {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    script.replace(" ", "%20");
    String url = "http://" + MOONRAKER_IP + ":" + String(MOONRAKER_PORT) + "/printer/gcode/script?script=" + script;
    http.begin(url);
    http.setTimeout(800);
    http.POST("");
    http.end();
}

void send_post_endpoint(String endpoint) {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    String url = "http://" + MOONRAKER_IP + ":" + String(MOONRAKER_PORT) + endpoint;
    http.begin(url);
    http.setTimeout(800);
    http.POST("");
    http.end();
}

// ================= CALLBACKS DA INTERFACE =================
static void switch_tab_cb(lv_event_t *e) {
    int tab = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_add_flag(panel_temp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_move, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_macros, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(panel_sys, LV_OBJ_FLAG_HIDDEN);

    if (tab == 0) lv_obj_clear_flag(panel_temp, LV_OBJ_FLAG_HIDDEN);
    if (tab == 1) lv_obj_clear_flag(panel_move, LV_OBJ_FLAG_HIDDEN);
    if (tab == 2) lv_obj_clear_flag(panel_macros, LV_OBJ_FLAG_HIDDEN);
    if (tab == 3) lv_obj_clear_flag(panel_sys, LV_OBJ_FLAG_HIDDEN);
}

static void gcode_btn_cb(lv_event_t *e) {
    const char* cmd = (const char*)lv_event_get_user_data(e);
    send_gcode(String(cmd));
}

static void move_axis_cb(lv_event_t *e) {
    const char* axis_dir = (const char*)lv_event_get_user_data(e); 
    // Exemplo: "X+" ou "Z-"
    char axis = axis_dir[0];
    char dir  = axis_dir[1];
    float dist = (dir == '-') ? -current_step : current_step;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "G91\nG1 %c%.1f F3000\nG90", axis, dist);
    send_gcode(String(cmd));
}

static void step_select_cb(lv_event_t *e) {
    int step_val = (int)(intptr_t)lv_event_get_user_data(e);
    current_step = (float)step_val;
}

static void macro_click_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        send_gcode(String(lv_label_get_text(lbl)));
    }
}

static void estop_cb(lv_event_t *e) {
    send_post_endpoint("/printer/emergency_stop");
}

// Função auxiliar para criar botões estilizados padrão CYD-Klipper
lv_obj_t* create_cyd_btn(lv_obj_t *parent, const char* text, int w, int h, uint32_t color_hex, lv_event_cb_t cb, void* user_data) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color_hex), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

// ================= CONSTRUÇÃO DO LAYOUT CYD-KLIPPER =================
void build_cyd_klipper_ui() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x121212), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // 1. SIDEBAR LATERAL ESQUERDA (50px conforme seu JSON)
    lv_obj_t *sidebar = lv_obj_create(scr);
    lv_obj_set_size(sidebar, SIDEBAR_W, SCREEN_H);
    lv_obj_align(sidebar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(sidebar, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_width(sidebar, 0, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);
    lv_obj_set_style_pad_all(sidebar, 4, 0);
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sidebar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    create_cyd_btn(sidebar, LV_SYMBOL_CHARGE, 42, 48, 0x2979FF, switch_tab_cb, (void*)0);
    create_cyd_btn(sidebar, LV_SYMBOL_HOME,   42, 48, 0x37474F, switch_tab_cb, (void*)1);
    create_cyd_btn(sidebar, LV_SYMBOL_LIST,   42, 48, 0x37474F, switch_tab_cb, (void*)2);
    create_cyd_btn(sidebar, LV_SYMBOL_SETTINGS, 42, 48, 0x37474F, switch_tab_cb, (void*)3);
    create_cyd_btn(sidebar, LV_SYMBOL_WARNING, 42, 48, 0xD50000, estop_cb, NULL);

    int content_w = SCREEN_W - SIDEBAR_W;

    // ---------------- ABA 0: TEMPERATURAS E STATUS ----------------
    panel_temp = lv_obj_create(scr);
    lv_obj_set_size(panel_temp, content_w, SCREEN_H);
    lv_obj_align(panel_temp, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(panel_temp, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(panel_temp, 0, 0);
    lv_obj_set_style_pad_all(panel_temp, GAP_PX, 0);
    lv_obj_clear_flag(panel_temp, LV_OBJ_FLAG_SCROLLABLE);

    lbl_state = lv_label_create(panel_temp);
    lv_label_set_text(lbl_state, "Conectando ao Klipper...");
    lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x00E676), 0);
    lv_obj_align(lbl_state, LV_ALIGN_TOP_LEFT, 4, 2);

    // Card Hotend
    lv_obj_t *c_noz = lv_obj_create(panel_temp);
    lv_obj_set_size(c_noz, 200, 85);
    lv_obj_align(c_noz, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_set_style_bg_color(c_noz, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(c_noz, lv_color_hex(0xFF5252), 0);
    lv_obj_clear_flag(c_noz, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t_noz = lv_label_create(c_noz);
    lv_label_set_text(t_noz, "EXTRUSORA");
    lv_obj_set_style_text_font(t_noz, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t_noz, lv_color_hex(0xFF5252), 0);
    lv_obj_align(t_noz, LV_ALIGN_TOP_LEFT, 0, -4);

    lbl_noz_val = lv_label_create(c_noz);
    lv_label_set_text(lbl_noz_val, "-- / -- C");
    lv_obj_set_style_text_font(lbl_noz_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_noz_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_noz_val, LV_ALIGN_BOTTOM_MID, 0, 2);

    // Card Mesa
    lv_obj_t *c_bed = lv_obj_create(panel_temp);
    lv_obj_set_size(c_bed, 200, 85);
    lv_obj_align(c_bed, LV_ALIGN_TOP_RIGHT, 0, 26);
    lv_obj_set_style_bg_color(c_bed, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(c_bed, lv_color_hex(0x40C4FF), 0);
    lv_obj_clear_flag(c_bed, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t_bed = lv_label_create(c_bed);
    lv_label_set_text(t_bed, "MESA AQUECIDA");
    lv_obj_set_style_text_font(t_bed, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t_bed, lv_color_hex(0x40C4FF), 0);
    lv_obj_align(t_bed, LV_ALIGN_TOP_LEFT, 0, -4);

    lbl_bed_val = lv_label_create(c_bed);
    lv_label_set_text(lbl_bed_val, "-- / -- C");
    lv_obj_set_style_text_font(lbl_bed_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_bed_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_bed_val, LV_ALIGN_BOTTOM_MID, 0, 2);

    // Barra de Progresso de Impressão
    bar_progress = lv_bar_create(panel_temp);
    lv_obj_set_size(bar_progress, 410, 22);
    lv_obj_align(bar_progress, LV_ALIGN_TOP_MID, 0, 122);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);

    lbl_progress = lv_label_create(panel_temp);
    lv_label_set_text(lbl_progress, "Progresso: 0%");
    lv_obj_set_style_text_font(lbl_progress, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_progress, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_progress, LV_ALIGN_TOP_MID, 0, 125);

    // Presets Rápidos de Temperatura (PLA, PETG, ABS, ESFRIAR)
    lv_obj_t *b_pla = create_cyd_btn(panel_temp, "PLA\n200/60", 96, 52, 0x2E7D32, gcode_btn_cb, (void*)"M104 S200\nM140 S60");
    lv_obj_align(b_pla, LV_ALIGN_BOTTOM_LEFT, 0, -52);

    lv_obj_t *b_petg = create_cyd_btn(panel_temp, "PETG\n235/75", 96, 52, 0xEF6C00, gcode_btn_cb, (void*)"M104 S235\nM140 S75");
    lv_obj_align(b_petg, LV_ALIGN_BOTTOM_LEFT, 104, -52);

    lv_obj_t *b_abs = create_cyd_btn(panel_temp, "ABS\n250/100", 96, 52, 0xC62828, gcode_btn_cb, (void*)"M104 S250\nM140 S100");
    lv_obj_align(b_abs, LV_ALIGN_BOTTOM_LEFT, 208, -52);

    lv_obj_t *b_cool = create_cyd_btn(panel_temp, "ESFRIAR", 98, 52, 0x0277BD, gcode_btn_cb, (void*)"TURN_OFF_HEATERS");
    lv_obj_align(b_cool, LV_ALIGN_BOTTOM_RIGHT, 0, -52);

    // Controle de Impressão na base
    lv_obj_t *b_pause = create_cyd_btn(panel_temp, "PAUSAR", 132, 42, 0x455A64, gcode_btn_cb, (void*)"PAUSE");
    lv_obj_align(b_pause, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *b_resume = create_cyd_btn(panel_temp, "RETOMAR", 132, 42, 0x2E7D32, gcode_btn_cb, (void*)"RESUME");
    lv_obj_align(b_resume, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *b_cancel = create_cyd_btn(panel_temp, "CANCELAR", 132, 42, 0xB71C1C, gcode_btn_cb, (void*)"CANCEL_PRINT");
    lv_obj_align(b_cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // ---------------- ABA 1: MOVIMENTAÇÃO E HOME (X/Y/Z) ----------------
    panel_move = lv_obj_create(scr);
    lv_obj_set_size(panel_move, content_w, SCREEN_H);
    lv_obj_align(panel_move, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(panel_move, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(panel_move, 0, 0);
    lv_obj_set_style_pad_all(panel_move, GAP_PX, 0);
    lv_obj_add_flag(panel_move, LV_OBJ_FLAG_HIDDEN);

    lbl_pos_xyz = lv_label_create(panel_move);
    lv_label_set_text(lbl_pos_xyz, "X: 0.0   Y: 0.0   Z: 0.00");
    lv_obj_set_style_text_color(lbl_pos_xyz, lv_color_hex(0x00E676), 0);
    lv_obj_align(lbl_pos_xyz, LV_ALIGN_TOP_LEFT, 4, 2);

    // Grade Direcional
    lv_obj_t *b_yp = create_cyd_btn(panel_move, "Y+", 64, 52, 0x37474F, move_axis_cb, (void*)"Y+");
    lv_obj_align(b_yp, LV_ALIGN_TOP_LEFT, 70, 30);

    lv_obj_t *b_xm = create_cyd_btn(panel_move, "X-", 64, 52, 0x37474F, move_axis_cb, (void*)"X-");
    lv_obj_align(b_xm, LV_ALIGN_TOP_LEFT, 0, 88);

    lv_obj_t *b_hall = create_cyd_btn(panel_move, "HOME\nALL", 64, 52, 0x2979FF, gcode_btn_cb, (void*)"G28");
    lv_obj_align(b_hall, LV_ALIGN_TOP_LEFT, 70, 88);

    lv_obj_t *b_xp = create_cyd_btn(panel_move, "X+", 64, 52, 0x37474F, move_axis_cb, (void*)"X+");
    lv_obj_align(b_xp, LV_ALIGN_TOP_LEFT, 140, 88);

    lv_obj_t *b_ym = create_cyd_btn(panel_move, "Y-", 64, 52, 0x37474F, move_axis_cb, (void*)"Y-");
    lv_obj_align(b_ym, LV_ALIGN_TOP_LEFT, 70, 146);

    // Eixo Z
    lv_obj_t *b_zp = create_cyd_btn(panel_move, "Z+ (Desce)", 95, 52, 0x37474F, move_axis_cb, (void*)"Z+");
    lv_obj_align(b_zp, LV_ALIGN_TOP_LEFT, 220, 30);

    lv_obj_t *b_zm = create_cyd_btn(panel_move, "Z- (Sobe)", 95, 52, 0x37474F, move_axis_cb, (void*)"Z-");
    lv_obj_align(b_zm, LV_ALIGN_TOP_LEFT, 220, 146);

    // Homes individuais
    lv_obj_t *b_hx = create_cyd_btn(panel_move, "Home XY", 85, 52, 0x1565C0, gcode_btn_cb, (void*)"G28 X Y");
    lv_obj_align(b_hx, LV_ALIGN_TOP_RIGHT, 0, 30);

    lv_obj_t *b_hz = create_cyd_btn(panel_move, "Home Z", 85, 52, 0x1565C0, gcode_btn_cb, (void*)"G28 Z");
    lv_obj_align(b_hz, LV_ALIGN_TOP_RIGHT, 0, 88);

    lv_obj_t *b_off = create_cyd_btn(panel_move, "Motores\nOFF", 85, 52, 0x455A64, gcode_btn_cb, (void*)"M84");
    lv_obj_align(b_off, LV_ALIGN_TOP_RIGHT, 0, 146);

    // Seleção de Passo (1mm, 10mm, 50mm)
    lv_obj_t *b_s1 = create_cyd_btn(panel_move, "Passo 1mm", 130, 42, 0x263238, step_select_cb, (void*)1);
    lv_obj_align(b_s1, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *b_s10 = create_cyd_btn(panel_move, "Passo 10mm", 130, 42, 0x263238, step_select_cb, (void*)10);
    lv_obj_align(b_s10, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *b_s50 = create_cyd_btn(panel_move, "Passo 50mm", 130, 42, 0x263238, step_select_cb, (void*)50);
    lv_obj_align(b_s50, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    // ---------------- ABA 2: MACROS DO KLIPPER ----------------
    panel_macros = lv_obj_create(scr);
    lv_obj_set_size(panel_macros, content_w, SCREEN_H);
    lv_obj_align(panel_macros, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(panel_macros, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(panel_macros, 0, 0);
    lv_obj_set_style_pad_all(panel_macros, GAP_PX, 0);
    lv_obj_add_flag(panel_macros, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lbl_m_title = lv_label_create(panel_macros);
    lv_label_set_text(lbl_m_title, "Macros do Klipper (Carregadas via API):");
    lv_obj_set_style_text_color(lbl_m_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_m_title, LV_ALIGN_TOP_LEFT, 4, 2);

    macro_grid = lv_obj_create(panel_macros);
    lv_obj_set_size(macro_grid, content_w - 16, SCREEN_H - 42);
    lv_obj_align(macro_grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(macro_grid, lv_color_hex(0x181818), 0);
    lv_obj_set_style_border_width(macro_grid, 0, 0);
    lv_obj_set_flex_flow(macro_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(macro_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(macro_grid, 6, 0);
    lv_obj_set_style_pad_gap(macro_grid, 8, 0);

    // ---------------- ABA 3: SISTEMA E REINICIALIZAÇÃO ----------------
    panel_sys = lv_obj_create(scr);
    lv_obj_set_size(panel_sys, content_w, SCREEN_H);
    lv_obj_align(panel_sys, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(panel_sys, lv_color_hex(0x121212), 0);
    lv_obj_set_style_border_width(panel_sys, 0, 0);
    lv_obj_set_style_pad_all(panel_sys, GAP_PX, 0);
    lv_obj_add_flag(panel_sys, LV_OBJ_FLAG_HIDDEN);

    lbl_ip_info = lv_label_create(panel_sys);
    lv_label_set_text(lbl_ip_info, "Wi-Fi: Conectando...");
    lv_obj_set_style_text_color(lbl_ip_info, lv_color_hex(0x00E676), 0);
    lv_obj_align(lbl_ip_info, LV_ALIGN_TOP_LEFT, 4, 6);

    lv_obj_t *b_rst = create_cyd_btn(panel_sys, "RESTART KLIPPER", 195, 60, 0x0277BD, gcode_btn_cb, (void*)"RESTART");
    lv_obj_align(b_rst, LV_ALIGN_TOP_LEFT, 0, 45);

    lv_obj_t *b_fw_rst = create_cyd_btn(panel_sys, "FIRMWARE RESTART", 195, 60, 0xEF6C00, gcode_btn_cb, (void*)"FIRMWARE_RESTART");
    lv_obj_align(b_fw_rst, LV_ALIGN_TOP_RIGHT, 0, 45);

    lv_obj_t *b_estop_big = create_cyd_btn(panel_sys, "PARADA DE EMERGENCIA (M112)", 410, 75, 0xD50000, estop_cb, NULL);
    lv_obj_align(b_estop_big, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ================= BUSCA AUTOMÁTICA DE MACROS E DADOS =================
void fetch_klipper_macros() {
    if (WiFi.status() != WL_CONNECTED || macros_loaded) return;
    HTTPClient http;
    String url = "http://" + MOONRAKER_IP + ":" + String(MOONRAKER_PORT) + "/printer/objects/list";
    http.begin(url);
    http.setTimeout(1000);
    if (http.GET() == 200) {
        JsonDocument doc;
        deserializeJson(doc, http.getString());
        JsonArray objs = doc["result"]["objects"].as<JsonArray>();
        lv_obj_clean(macro_grid);
        for (JsonVariant v : objs) {
            String name = v.as<String>();
            if (name.startsWith("gcode_macro ")) {
                String macro_name = name.substring(12);
                if (!macro_name.startsWith("_")) { // Ignora macros ocultas de sistema
                    create_cyd_btn(macro_grid, macro_name.c_str(), 122, 44, 0x37474F, macro_click_cb, NULL);
                }
            }
        }
        macros_loaded = true;
    }
    http.end();
}

void poll_moonraker_status() {
    if (WiFi.status() != WL_CONNECTED) {
        lv_label_set_text(lbl_state, "Wi-Fi Desconectado...");
        return;
    }

    String ip_txt = "IP da Tela: " + WiFi.localIP().toString() + "  |  Moonraker: " + MOONRAKER_IP;
    lv_label_set_text(lbl_ip_info, ip_txt.c_str());

    if (!macros_loaded) fetch_klipper_macros();

    HTTPClient http;
    String url = "http://" + MOONRAKER_IP + ":" + String(MOONRAKER_PORT) + "/printer/objects/query?extruder&heater_bed&print_stats&virtual_sdcard&toolhead";
    http.begin(url);
    http.setTimeout(600);
    int code = http.GET();
    if (code == 200) {
        JsonDocument doc;
        deserializeJson(doc, http.getString());
        JsonObject status = doc["result"]["status"];

        float e_temp = status["extruder"]["temperature"] | 0.0;
        float e_targ = status["extruder"]["target"] | 0.0;
        float b_temp = status["heater_bed"]["temperature"] | 0.0;
        float b_targ = status["heater_bed"]["target"] | 0.0;
        const char* st = status["print_stats"]["state"] | "ready";
        float prog = (status["virtual_sdcard"]["progress"] | 0.0) * 100.0;

        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f / %.0f C", e_temp, e_targ);
        lv_label_set_text(lbl_noz_val, buf);

        snprintf(buf, sizeof(buf), "%.1f / %.0f C", b_temp, b_targ);
        lv_label_set_text(lbl_bed_val, buf);

        snprintf(buf, sizeof(buf), "Klipper: %s", st);
        lv_label_set_text(lbl_state, buf);

        lv_bar_set_value(bar_progress, (int)prog, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "Progresso: %.0f%%", prog);
        lv_label_set_text(lbl_progress, buf);

        JsonArray pos = status["toolhead"]["position"];
        if (!pos.isNull() && pos.size() >= 3) {
            snprintf(buf, sizeof(buf), "X: %.1f   Y: %.1f   Z: %.2f   (Passo: %.0fmm)", 
                     pos[0].as<float>(), pos[1].as<float>(), pos[2].as<float>(), current_step);
            lv_label_set_text(lbl_pos_xyz, buf);
        }
    }
    http.end();
}

// ================= SETUP E LOOP =================
uint32_t last_poll = 0;

void setup() {
    Serial.begin(115200);

    // Liga o Backlight (Pino 1)
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    // Inicia Display QSPI NV3041A
    gfx->begin();
    gfx->fillScreen(BLACK);

    // Inicia Touch GT911
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    ts.begin();
    ts.setRotation(ROTATION_INVERTED);

    lv_init();

    // Aloca buffer na RAM interna ou PSRAM OPI
    uint32_t buf_pixels = SCREEN_W * 40;
    disp_draw_buf = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!disp_draw_buf) {
        disp_draw_buf = (lv_color_t *)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }

    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, buf_pixels);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_W;
    disp_drv.ver_res = SCREEN_H;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    build_cyd_klipper_ui();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
    lv_timer_handler();
    if (millis() - last_poll > 1500) {
        last_poll = millis();
        poll_moonraker_status();
    }
    delay(5);
}
