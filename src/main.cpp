/*
 * Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 * Copyright (c) 2026 Mark Bijwaard
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Arduino.h>
#include <osdp.hpp>
#include <vector>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <freertos/semphr.h>
#include <cctype>
#include <cstring>
#include "wifi_secrets.h"

// ===== CONFIG =====
#define UID_LEN 5
#define DOOR_PIN 5
#define RXD2 16
#define TXD2 17
#define RS485_DE_PIN 4
#define LOG_SIZE 20
#define CMD_POOL_SIZE 8
#define LEARN_TIMEOUT 10000
#define DOOR_OPEN_TIME 3000  // ms
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define PD_RECOVERY_DISABLE_DELAY_MS 250
#define PD_RECOVERY_ENABLE_DELAY_MS 250

#ifndef WEB_USERNAME
#define WEB_USERNAME ""
#endif

#ifndef WEB_PASSWORD
#define WEB_PASSWORD ""
#endif

// ===== OSDP =====
OSDP::ControlPanel cp;
osdp_pd_info_t pd_info[1];
static struct osdp_channel cp_channel = {};
static uint8_t scbk[16] = {
    0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30,
    0x30,0x30,0x30,0x30
};

// ===== STATE =====
bool pd_online = false;
bool init_done = false;
bool learn_mode = false;
bool door_active = false;
bool pd_recovery_pending = false;
bool pd_recovery_disable_sent = false;

uint32_t door_open_time = 0;
uint32_t learn_start = 0;
uint32_t last_action_time = 0;
uint32_t pd_recovery_at = 0;

// ===== STORAGE =====
struct Card {
    uint8_t uid[UID_LEN];
};
struct LogEntry {
    uint8_t uid[UID_LEN];
    uint32_t timestamp;
};
std::vector<Card> allowed_cards;
LogEntry log_buffer[LOG_SIZE];
int log_index = 0;
int log_count = 0;

// ===== COMMAND POOL =====
osdp_cmd cmd_pool[CMD_POOL_SIZE];
uint8_t cmd_index = 0;

AsyncWebServer server(80);
SemaphoreHandle_t state_mutex = nullptr;

bool lock_state(TickType_t timeout = pdMS_TO_TICKS(250))
{
    return state_mutex != nullptr &&
           xSemaphoreTake(state_mutex, timeout) == pdTRUE;
}

void unlock_state()
{
    if (state_mutex != nullptr) {
        xSemaphoreGive(state_mutex);
    }
}

void format_uid_hex(const uint8_t *uid, char *buf, size_t len)
{
    if (len < (UID_LEN * 2 + 1)) {
        return;
    }

    for (int i = 0; i < UID_LEN; i++) {
        snprintf(buf + i * 2, len - (i * 2), "%02X", uid[i]);
    }
}

bool parse_uid_string(const String &uid_str, uint8_t *uid)
{
    if (uid_str.length() != UID_LEN * 2) {
        return false;
    }

    for (int i = 0; i < UID_LEN; i++) {
        char hi = uid_str.charAt(i * 2);
        char lo = uid_str.charAt(i * 2 + 1);

        if (!isxdigit(static_cast<unsigned char>(hi)) ||
            !isxdigit(static_cast<unsigned char>(lo))) {
            return false;
        }

        char byte_str[3] = { hi, lo, '\0' };
        uid[i] = static_cast<uint8_t>(strtoul(byte_str, nullptr, 16));
    }

    return true;
}

bool parse_uid_param(AsyncWebServerRequest *request, uint8_t *uid)
{
    if (!request->hasParam("uid")) {
        request->send(400, "text/plain", "missing uid");
        return false;
    }

    String uid_str = request->getParam("uid")->value();
    uid_str.trim();
    uid_str.toUpperCase();

    if (!parse_uid_string(uid_str, uid)) {
        request->send(400, "text/plain", "uid must be 10 hex characters");
        return false;
    }

    return true;
}

bool is_web_auth_enabled()
{
    return strlen(WEB_USERNAME) > 0 || strlen(WEB_PASSWORD) > 0;
}

bool authorize_request(AsyncWebServerRequest *request)
{
    if (!is_web_auth_enabled()) {
        return true;
    }

    if (request->authenticate(WEB_USERNAME, WEB_PASSWORD)) {
        return true;
    }

    request->requestAuthentication();
    return false;
}

osdp_cmd* get_cmd()
{
    osdp_cmd *cmd = &cmd_pool[cmd_index];
    cmd_index = (cmd_index + 1) % CMD_POOL_SIZE;

    memset(cmd, 0, sizeof(osdp_cmd));
    return cmd;
}

enum Action {
    ACTION_NONE,
    ACTION_GRANTED,
    ACTION_DENIED,
    ACTION_LEARN_OK,
    ACTION_LEARN_DUP
};

volatile Action pending_action = ACTION_NONE;

void init_wifi() {
    if (strlen(WIFI_SSID) == 0 || strlen(WIFI_PASSWORD) == 0) {
        Serial.println("WiFi disabled: set WIFI_SSID and WIFI_PASSWORD in include/wifi_secrets.h");
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println("\nWiFi connect timeout");
            return;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println(WiFi.localIP());
}

void init_fs() {
    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed");
    }
}

void door_unlock()
{
    digitalWrite(DOOR_PIN, HIGH);
    door_active = true;
    door_open_time = millis();

    Serial.println("Door unlocked");
}

void load_cards() {
    File f = LittleFS.open("/cards.bin", "r");
    if (!f) {
        Serial.println("No cards file");
        return;
    }

    allowed_cards.clear();

    while (f.available() >= sizeof(Card)) {
        Card c;
        f.read((uint8_t*)&c, sizeof(Card));
        allowed_cards.push_back(c);
    }

    f.close();
    Serial.printf("Loaded %d cards\n", allowed_cards.size());
}

void save_cards() {
    File f = LittleFS.open("/cards.bin", "w");
    if (!f) {
        Serial.println("Failed to open cards.bin for writing");
        return;
    }

    for (auto &c : allowed_cards) {
        f.write((uint8_t*)&c, sizeof(Card));
    }

    f.close();
    Serial.println("Cards saved");
}

bool remove_card(uint8_t *uid)
{
    for (auto it = allowed_cards.begin(); it != allowed_cards.end(); ++it) {
        if (memcmp(it->uid, uid, UID_LEN) == 0) {
            allowed_cards.erase(it);
            save_cards();
            return true;
        }
    }
    return false;
}

bool card_equals(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, UID_LEN) == 0;
}

bool is_allowed(uint8_t *uid) {
    for (auto &c : allowed_cards) {
        if (card_equals(c.uid, uid)) return true;
    }
    return false;
}

void add_card(uint8_t *uid) {
    if (is_allowed(uid)) return;

    Card c;
    memcpy(c.uid, uid, UID_LEN);
    allowed_cards.push_back(c);

    save_cards();   // 🔴 direct persistent maken
}

void add_log(uint8_t *uid) {
    memcpy(log_buffer[log_index].uid, uid, UID_LEN);
    log_buffer[log_index].timestamp = millis();
    log_index = (log_index + 1) % LOG_SIZE;
    if (log_count < LOG_SIZE) {
        log_count++;
    }
}

void init_webserver()
{
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!authorize_request(request)) {
            return;
        }
        request->send(LittleFS, "/index.html", "text/html");
    });

    server.on("/cards", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!authorize_request(request)) {
        return;
    }

    std::vector<Card> cards_snapshot;
    if (!lock_state()) {
        request->send(503, "text/plain", "state busy");
        return;
    }
    cards_snapshot = allowed_cards;
    unlock_state();

    String json = "[";
    for (size_t i = 0; i < cards_snapshot.size(); i++) {
        char buf[UID_LEN * 2 + 1] = {0};
        format_uid_hex(cards_snapshot[i].uid, buf, sizeof(buf));
        json += "\"" + String(buf) + "\"";
        if (i + 1 < cards_snapshot.size()) {
            json += ",";
        }
    }

    json += "]";
    request->send(200, "application/json", json);
    });

    server.on("/add", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!authorize_request(request)) {
        return;
    }

    uint8_t uid[UID_LEN];
    if (!parse_uid_param(request, uid)) {
        return;
    }

    if (!lock_state()) {
        request->send(503, "text/plain", "state busy");
        return;
    }
    add_card(uid);
    unlock_state();
    
    request->send(200, "text/plain", "ok");
    });

    server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!authorize_request(request)) {
        return;
    }

    LogEntry log_snapshot[LOG_SIZE];
    int log_snapshot_count = 0;
    int log_snapshot_index = 0;

    if (!lock_state()) {
        request->send(503, "text/plain", "state busy");
        return;
    }
    memcpy(log_snapshot, log_buffer, sizeof(log_snapshot));
    log_snapshot_count = log_count;
    log_snapshot_index = log_index;
    unlock_state();

    String json = "[";
    for (int i = 0; i < log_snapshot_count; i++) {
        int idx = (log_snapshot_index - log_snapshot_count + i + LOG_SIZE) % LOG_SIZE;
        char buf[UID_LEN * 2 + 1] = {0};
        format_uid_hex(log_snapshot[idx].uid, buf, sizeof(buf));
        json += "\"" + String(buf) + "\"";
        if (i + 1 < log_snapshot_count) {
            json += ",";
        }
    }

    json += "]";
    request->send(200, "application/json", json);
    });

    server.on("/learn", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!authorize_request(request)) {
        return;
    }

    if (!lock_state()) {
        request->send(503, "text/plain", "state busy");
        return;
    }
    learn_mode = true;
    learn_start = millis();
    unlock_state();
    request->send(200, "text/plain", "learning");
    });

    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!authorize_request(request)) {
        return;
    }

    uint8_t uid[UID_LEN];
    if (!parse_uid_param(request, uid)) {
        return;
    }

    if (!lock_state()) {
        request->send(503, "text/plain", "state busy");
        return;
    }

    if (remove_card(uid)) {
        unlock_state();
        request->send(200, "text/plain", "deleted");
    } else {
        unlock_state();
        request->send(404, "text/plain", "not found");
    }
    });

    server.on("/open", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!authorize_request(request)) {
        return;
    }

    if (!lock_state()) {
        request->send(503, "text/plain", "state busy");
        return;
    }
    pending_action = ACTION_GRANTED;  // hergebruik bestaande flow
    unlock_state();

    request->send(200, "text/plain", "door triggered");
    });
}

// void led_set_idle()
// {
//     osdp_cmd *cmd = get_cmd();

//     cmd->id = OSDP_CMD_LED;
//     cmd->led.reader = 0;
//     cmd->led.led_number = 0;

//     cmd->led.permanent.control_code = 1;
//     cmd->led.permanent.on_color = OSDP_LED_COLOR_BLUE;
//     cmd->led.permanent.off_color = OSDP_LED_COLOR_NONE;

//     cp.submit_command(0, cmd);
// }

void led_feedback(uint8_t color)
{
    osdp_cmd *cmd = get_cmd();

    cmd->id = OSDP_CMD_LED;
    cmd->led.reader = 0;
    cmd->led.led_number = 0;

    cmd->led.temporary.control_code = 2;
    cmd->led.temporary.on_count = 30;
    cmd->led.temporary.off_count = 1;
    cmd->led.temporary.on_color = color;
    cmd->led.temporary.off_color = OSDP_LED_COLOR_BLUE;
    cmd->led.temporary.timer_count = 31;

    cp.submit_command(0, cmd);
}

void buzzer_beep()
{
    osdp_cmd *cmd = get_cmd();

    cmd->id = OSDP_CMD_BUZZER;
    cmd->buzzer.reader = 0;
    cmd->buzzer.control_code = 2;
    cmd->buzzer.on_count = 1;
    cmd->buzzer.off_count = 0;
    cmd->buzzer.rep_count = 1;

    cp.submit_command(0, cmd);
}

void clear_serial2_rx()
{
    while (Serial2.available() > 0) {
        Serial2.read();
    }
}

void osdp_task(void *param)
{
    while (true) {

        cp.refresh();

        bool do_init_led = false;
        Action action_to_run = ACTION_NONE;
        bool do_pd_disable = false;
        bool do_pd_enable = false;

        if (lock_state(pdMS_TO_TICKS(10))) {
            // init LED na online
            if (pd_online && !init_done) {
                init_done = true;
                do_init_led = true;
            }

            // learn timeout
            if (learn_mode && millis() - learn_start > LEARN_TIMEOUT) {
                learn_mode = false;
                Serial.println("Learn mode timeout");
            }

            // dispatcher
            if (pending_action != ACTION_NONE &&
                millis() - last_action_time > 120)
            {
                action_to_run = pending_action;
                pending_action = ACTION_NONE;
                last_action_time = millis();
            }

            if (pd_recovery_pending && millis() >= pd_recovery_at) {
                if (!pd_recovery_disable_sent) {
                    do_pd_disable = cp.is_pd_enabled(0);
                    if (!do_pd_disable) {
                        pd_recovery_disable_sent = true;
                        pd_recovery_at = millis() + PD_RECOVERY_ENABLE_DELAY_MS;
                    }
                } else if (!cp.is_pd_enabled(0)) {
                    do_pd_enable = true;
                }
            }

            unlock_state();
        }

        if (do_pd_disable) {
            clear_serial2_rx();
            if (cp.disable_pd(0) == 0) {
                if (lock_state()) {
                    pd_recovery_disable_sent = true;
                    pd_recovery_at = millis() + PD_RECOVERY_ENABLE_DELAY_MS;
                    unlock_state();
                }
                Serial.println("PD recovery: disable requested");
            }
        }

        if (do_pd_enable) {
            clear_serial2_rx();
            if (cp.enable_pd(0) == 0) {
                if (lock_state()) {
                    pd_recovery_pending = false;
                    pd_recovery_disable_sent = false;
                    unlock_state();
                }
                Serial.println("PD recovery: enable requested");
            } else if (lock_state()) {
                pd_recovery_at = millis() + PD_RECOVERY_ENABLE_DELAY_MS;
                unlock_state();
            }
        }

        if (do_init_led) {
            led_feedback(OSDP_LED_COLOR_BLUE);
        }

        if (door_active && millis() - door_open_time > DOOR_OPEN_TIME) {
            digitalWrite(DOOR_PIN, LOW);
            door_active = false;
            Serial.println("Door locked");
        }
        
        if (action_to_run != ACTION_NONE) {
            switch (action_to_run) {

                case ACTION_GRANTED:
                    led_feedback(OSDP_LED_COLOR_GREEN);
                    buzzer_beep();
                    door_unlock();
                    break;

                case ACTION_DENIED:
                    led_feedback(OSDP_LED_COLOR_RED);
                    buzzer_beep();
                    break;

                case ACTION_LEARN_OK:
                    led_feedback(OSDP_LED_COLOR_GREEN);
                    buzzer_beep();
                    break;

                case ACTION_LEARN_DUP:
                    led_feedback(OSDP_LED_COLOR_RED);
                    buzzer_beep();
                    break;

                default:
                    break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // ~100 Hz
    }
}

int serial1_send_func(void *data, uint8_t *buf, int len)
{
    digitalWrite(RS485_DE_PIN, HIGH);
    delayMicroseconds(50);

    Serial2.write(buf, len);
    Serial2.flush();

    delayMicroseconds(50);
    digitalWrite(RS485_DE_PIN, LOW);

    return len;
}

int serial1_recv_func(void *data, uint8_t *buf, int maxlen)
{
    int available = Serial2.available();

    if (available <= 0) {
        return 0;
    }

    int len = Serial2.readBytes(buf, min(available, maxlen));
    return len;
}

void init_cp_info()
{
    pd_info[0].name = "pd[1]";
    pd_info[0].baud_rate = 9600;
    pd_info[0].address = 1;
    pd_info[0].id.version = 0;
    pd_info[0].id.model = 0;
    pd_info[0].id.vendor_code = 0;
    pd_info[0].id.serial_number = 0;
    pd_info[0].id.firmware_version = 0;
    pd_info[0].cap = nullptr;
    pd_info[0].scbk = scbk;
    pd_info[0].flags = OSDP_FLAG_ENABLE_NOTIFICATION;

    cp_channel.data = NULL;
    cp_channel.recv = serial1_recv_func;
    cp_channel.send = serial1_send_func;
    cp_channel.flush = NULL;
}

int event_handler(void *data, int pd, struct osdp_event *event)
{
    if (event->type == OSDP_EVENT_NOTIFICATION) {
        if (event->notif.type == OSDP_EVENT_NOTIFICATION_PD_STATUS) {
            if (lock_state()) {
                pd_online = event->notif.arg0 == 1;
                if (pd_online) {
                    pd_recovery_pending = false;
                    pd_recovery_disable_sent = false;
                } else {
                    init_done = false;
                    pd_recovery_pending = true;
                    pd_recovery_disable_sent = false;
                    pd_recovery_at = millis() + PD_RECOVERY_DISABLE_DELAY_MS;
                }
                unlock_state();
            }

            Serial.println(event->notif.arg0 == 1 ? "PD ONLINE" : "PD OFFLINE");
        }
    }

    if (event->type == OSDP_EVENT_CARDREAD) {

        uint8_t uid[UID_LEN];
        memcpy(uid, event->cardread.data, UID_LEN);

        if (!lock_state()) {
            return -1;
        }

        add_log(uid);

        if (learn_mode) {
            if (!is_allowed(uid)) {
                add_card(uid);
                pending_action = ACTION_LEARN_OK;
            } else {
                pending_action = ACTION_LEARN_DUP;
            }

            learn_mode = false;
            unlock_state();
            return 0;
        }

        if (is_allowed(uid)) {
            pending_action = ACTION_GRANTED;
        } else {
            pending_action = ACTION_DENIED;
        }

        unlock_state();
    }

    return 0;
}

void setup()
{
    Serial.begin(115200);
    state_mutex = xSemaphoreCreateMutex();

    pinMode(DOOR_PIN, OUTPUT);
    digitalWrite(DOOR_PIN, LOW);  // deur dicht

    init_wifi();
    init_fs();
    load_cards();

    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, LOW); // receive mode

    Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
    cp.logger_init("osdp::cp", OSDP_LOG_DEBUG, NULL);

    if (!is_web_auth_enabled()) {
        Serial.println("Web auth disabled: set WEB_USERNAME/WEB_PASSWORD build flags");
    }

    init_webserver();
    server.begin();

    // OSDP init
    init_cp_info();
    cp.setup(&cp_channel, 1, pd_info);
    cp.set_event_callback(event_handler, nullptr);

    Serial.print("OSDP CP Started....\n");

    // Create a separate task for OSDP processing
    xTaskCreatePinnedToCore(osdp_task, "osdp_task", 4096, NULL, 2, NULL, 1);
}

void loop() {
    delay(100);
}







