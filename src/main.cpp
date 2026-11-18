#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <Wire.h>
#include <TAMC_GT911.h>

// ================= CONFIGURACAO WI-FI E KLIPPER =================
const char* WIFI_SSID = "SUA_REDE_WIFI";
const char* WIFI_PASS = "SUA_SENHA_WIFI";
String MOONRAKER_IP   = "192.168.1.100"; // IP do seu Host Klipper
const int MOONRAKER_PORT = 7125;

// ================= PINAGEM JC4827W543 (QSPI NV3041A) =================
#define GFX_BL 1
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */
);
Arduino_GFX *gfx = new Arduino_NV3041A(bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, true /* IPS */);

// ================= PINAGEM TOUCH GT911 =================
#define TOUCH_SDA  8
#define TOUCH_SCL  4
#define TOUCH_INT  3
#define TOUCH_RST 38
TAMC_GT911 ts = TAMC_GT911(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, 480, 272);

// ================= BUFFERS LVGL =================
static const uint16_t screenWidth  = 480;
static const uint16_t screenHeight = 272;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf;
static lv_disp_drv_t disp_drv;

// Objetos da Interface
lv_obj_t *lbl_status;
lv_obj_t *lbl_nozzle;
lv_obj_t *lbl_bed;
lv_obj_t *lbl_ip;

// Flush da tela via QSPI
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    #if (LV_COLOR_16_SWAP != 0)
        gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
    #else
        gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
    #endif
    lv_disp_flush_ready(disp);
}

// Leitura do Touch GT911
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

// Envio de G-code para o Moonraker
void send_gcode(const char* cmd) {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    String url = "http://" + MOONRAKER_IP + ":" + String(MOONRAKER_PORT) + "/printer/gcode/script?script=" + String(cmd);
    url.replace(" ", "%20");
    http.begin(url);
    http.POST("");
    http.end();
}

// Eventos dos botoes
static void btn_home_event(lv_event_t *e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) send_gcode("G28");
}

static void btn_Cooldown_event(lv_event_t *e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) send_gcode("TURN_OFF_HEATERS");
}

static void btn_emergency_event(lv_event_t *e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (WiFi.status() == WL_CONNECTED) {
            HTTPClient http;
            http.begin("http://" + MOONRAKER_IP + ":" + String(MOONRAKER_PORT) + "/printer/emergency_stop");
            http.POST("");
            http.end();
        }
    }
}

void build_ui() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x121212), LV_PART_MAIN);

    // Barra Superior
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 460, 42);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lbl_status = lv_label_create(header);
    lv_label_set_text(lbl_status, "Klipper JC4827W543 - Pronto");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x00E676), 0);
    lv_obj_align(lbl_status, LV_ALIGN_LEFT_MID, 5, 0);

    lbl_ip = lv_label_create(header);
    lv_label_set_text(lbl_ip, "Wi-Fi...");
    lv_obj_set_style_text_color(lbl_ip, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(lbl_ip, LV_ALIGN_RIGHT_MID, -5, 0);

    // Card Extrusora
    lv_obj_t *card_noz = lv_obj_create(scr);
    lv_obj_set_size(card_noz, 222, 110);
    lv_obj_align(card_noz, LV_ALIGN_TOP_LEFT, 10, 58);
    lv_obj_set_style_bg_color(card_noz, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(card_noz, lv_color_hex(0xFF5252), 0);

    lv_obj_t *t_noz = lv_label_create(card_noz);
    lv_label_set_text(t_noz, "EXTRUSORA");
    lv_obj_set_style_text_color(t_noz, lv_color_hex(0xFF5252), 0);
    lv_obj_align(t_noz, LV_ALIGN_TOP_MID, 0, 0);

    lbl_nozzle = lv_label_create(card_noz);
    lv_label_set_text(lbl_nozzle, "-- / -- C");
    lv_obj_set_style_text_font(lbl_nozzle, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_nozzle, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_nozzle, LV_ALIGN_CENTER, 0, 12);

    // Card Mesa
    lv_obj_t *card_bed = lv_obj_create(scr);
    lv_obj_set_size(card_bed, 222, 110);
    lv_obj_align(card_bed, LV_ALIGN_TOP_RIGHT, -10, 58);
    lv_obj_set_style_bg_color(card_bed, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(card_bed, lv_color_hex(0x40C4FF), 0);

    lv_obj_t *t_bed = lv_label_create(card_bed);
    lv_label_set_text(t_bed, "MESA AQUECIDA");
    lv_obj_set_style_text_color(t_bed, lv_color_hex(0x40C4FF), 0);
    lv_obj_align(t_bed, LV_ALIGN_TOP_MID, 0, 0);

    lbl_bed = lv_label_create(card_bed);
    lv_label_set_text(lbl_bed, "-- / -- C");
    lv_obj_set_style_text_font(lbl_bed, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_bed, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_bed, LV_ALIGN_CENTER, 0, 12);

    // Botao Home G28
    lv_obj_t *btn_home = lv_btn_create(scr);
    lv_obj_set_size(btn_home, 145, 75);
    lv_obj_align(btn_home, LV_ALIGN_BOTTOM_LEFT, 10, -12);
    lv_obj_set_style_bg_color(btn_home, lv_color_hex(0x2979FF), 0);
    lv_obj_add_event_cb(btn_home, btn_home_event, LV_EVENT_ALL, NULL);
    lv_obj_t *l_home = lv_label_create(btn_home);
    lv_label_set_text(l_home, "HOME (G28)");
    lv_obj_center(l_home);

    // Botao Esfriar
    lv_obj_t *btn_cool = lv_btn_create(scr);
    lv_obj_set_size(btn_cool, 145, 75);
    lv_obj_align(btn_cool, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(btn_cool, lv_color_hex(0x00B0FF), 0);
    lv_obj_add_event_cb(btn_cool, btn_Cooldown_event, LV_EVENT_ALL, NULL);
    lv_obj_t *l_cool = lv_label_create(btn_cool);
    lv_label_set_text(l_cool, "ESFRIAR");
    lv_obj_center(l_cool);

    // Botao Emergencia
    lv_obj_t *btn_estop = lv_btn_create(scr);
    lv_obj_set_size(btn_estop, 145, 75);
    lv_obj_align(btn_estop, LV_ALIGN_BOTTOM_RIGHT, -10, -12);
    lv_obj_set_style_bg_color(btn_estop, lv_color_hex(0xD50000), 0);
    lv_obj_add_event_cb(btn_estop, btn_emergency_event, LV_EVENT_ALL, NULL);
    lv_obj_t *l_estop = lv_label_create(btn_estop);
    lv_label_set_text(l_estop, "EMERGENCIA");
    lv_obj_center(l_estop);
}

uint32_t last_poll = 0;

void update_moonraker() {
    if (WiFi.status() != WL_CONNECTED) {
        lv_label_set_text(lbl_ip, "Sem Wi-Fi");
        return;
    }
    lv_label_set_text(lbl_ip, WiFi.localIP().toString().c_str());

    HTTPClient http;
    String url = "http://" + MOONRAKER_IP + ":" + String(MOONRAKER_PORT) + "/printer/objects/query?extruder&heater_bed&print_stats";
    http.begin(url);
    http.setTimeout(800);
    int code = http.GET();
    if (code == 200) {
        JsonDocument doc;
        deserializeJson(doc, http.getString());
        float e_temp = doc["result"]["status"]["extruder"]["temperature"] | 0.0;
        float e_targ = doc["result"]["status"]["extruder"]["target"] | 0.0;
        float b_temp = doc["result"]["status"]["heater_bed"]["temperature"] | 0.0;
        float b_targ = doc["result"]["status"]["heater_bed"]["target"] | 0.0;
        const char* state = doc["result"]["status"]["print_stats"]["state"] | "conectado";

        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f / %.0f C", e_temp, e_targ);
        lv_label_set_text(lbl_nozzle, buf);

        snprintf(buf, sizeof(buf), "%.1f / %.0f C", b_temp, b_targ);
        lv_label_set_text(lbl_bed, buf);

        snprintf(buf, sizeof(buf), "Klipper: %s", state);
        lv_label_set_text(lbl_status, buf);
    }
    http.end();
}

void setup() {
    Serial.begin(115200);

    // 1. Inicializa Display QSPI NV3041A
    gfx->begin();
    gfx->fillScreen(BLACK);

    // 2. Liga o Backlight (Pino 1 da JC4827W543)
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, HIGH);

    // 3. Inicializa Touch GT911
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    ts.begin();
    ts.setRotation(ROTATION_NORMAL);

    // 4. Inicializa LVGL na PSRAM ou RAM interna
    lv_init();
    disp_draw_buf = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * screenWidth * 40, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!disp_draw_buf) {
        disp_draw_buf = (lv_color_t *)malloc(sizeof(lv_color_t) * screenWidth * 40);
    }
    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, screenWidth * 40);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    build_ui();

    // 5. Inicia conexao Wi-Fi em background
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
    lv_timer_handler();
    if (millis() - last_poll > 2000) {
        last_poll = millis();
        update_moonraker();
    }
    delay(5);
}
