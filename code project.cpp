// ============================================================
// ESP8266 
// ============================================================
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

const char* ssid = "ESP-0FB5CF"; // Must match ESP32-CAM ssid
const int   udpPort = 4210;

WiFiUDP Udp;

// Buffer sized for safe null termination
char incomingPacket[257];

void setup() {
    // Links directly to Mega Serial3 (TX/RX pins)
    Serial.begin(115200);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid); // Open AP, no password

    Udp.begin(udpPort);

    // Startup confirmation (Mega will see this on Serial3)
    Serial.print("ESP8266_READY\n");
}

void loop() {
    int packetSize = Udp.parsePacket();

    if (packetSize > 0) {
        // Cap read at 255 to keep room for null terminator
        int len = Udp.read(incomingPacket, 255);

        if (len > 0) {
            incomingPacket[len] = '\0'; // Safe null terminate

            String payload = String(incomingPacket);
            payload.trim(); // Strip \r, spaces, junk

            if (payload.length() > 0) {
                // Forward cleanly to Mega with explicit newline
                Serial.print(payload);
                Serial.print("\n");
            }
        }
    }
}
// ===========================================================
// ESP32-CAM 
// ============================================================
#include <ESP32_Classification_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>

#define CAMERA_MODEL_AI_THINKER
#if defined(CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#endif

// ---- WiFi / UDP Settings ----
const char* ssid       = "ESP-0FB5CF";
const char* password   = "";
const char* udpAddress = "192.168.4.1";
const int   udpPort    = 4210;

WiFiUDP udp;
static bool  is_initialised = false;
uint8_t     *snapshot_buf   = nullptr;

// ---- Heartbeat ----
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 5000;

// ---- Cooldown between same-label sends ----
String        lastSentLabel = "";
unsigned long lastSentTime  = 0;
const unsigned long SEND_COOLDOWN = 1500;

// ---- WiFi retry timing ----
unsigned long lastRetry = 0;

// ---- Camera Config ----
static camera_config_t camera_config = {
    .pin_pwdn     = PWDN_GPIO_NUM,
    .pin_reset    = RESET_GPIO_NUM,
    .pin_xclk     = XCLK_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl = SIOC_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM, .pin_d6 = Y8_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM, .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM, .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM, .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href  = HREF_GPIO_NUM,
    .pin_pclk  = PCLK_GPIO_NUM,
    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count     = 1,
    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

// ---- UDP send helper ----
void sendUDP(const String& msg) {
    udp.beginPacket(udpAddress, udpPort);
    udp.print(msg + "\n");
    udp.endPacket();
}

void setup() {
    Serial.begin(115200);
    Serial.println("==================================");
    Serial.println("ESP32-CAM: STARTING...");
    Serial.println("==================================");

    // Camera init
    if (esp_camera_init(&camera_config) == ESP_OK) {
        is_initialised = true;
        sensor_t *s = esp_camera_sensor_get();
        s->set_hmirror(s, 1);
        s->set_vflip(s, 1);
        Serial.println("Camera: ONLINE");
    } else {
        Serial.println("Camera: INIT FAILED");
    }

    // Initial WiFi connect
    Serial.print("Connecting to AP: ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
        delay(10);
    }
    if (WiFi.status() == WL_CONNECTED) {
        udp.begin(udpPort);
        Serial.println("WiFi: CONNECTED");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("WiFi: Not connected yet (will retry in loop)");
    }

    lastHeartbeat = millis();
    lastRetry     = millis();
}

void loop() {
    // ----------------------------------------------------------
    // WiFi Watchdog — retry every 5 seconds only
    // ----------------------------------------------------------
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastRetry < 5000) {
            delay(10);
            return;
        }

        lastRetry = millis();
        Serial.println("WiFi lost. Retrying...");
        WiFi.disconnect();
        delay(100);
        WiFi.begin(ssid, password);

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
            delay(10);
        }

        if (WiFi.status() == WL_CONNECTED) {
            udp.begin(udpPort);
            Serial.println("WiFi: RECONNECTED");
            Serial.print("IP: ");
            Serial.println(WiFi.localIP());
        } else {
            Serial.println("WiFi: Retry failed. Will try again in 5s.");
        }
        return;
    }

    // ----------------------------------------------------------
    // Heartbeat — keeps Mega watchdog alive
    // ----------------------------------------------------------
    if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        sendUDP("ALIVE");
        lastHeartbeat = millis();
    }

    // ----------------------------------------------------------
    // Camera not ready guard
    // ----------------------------------------------------------
    if (!is_initialised) {
        delay(100);
        return;
    }

    // ----------------------------------------------------------
    // Grab frame
    // ----------------------------------------------------------
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        delay(10);
        return;
    }

    // ----------------------------------------------------------
    // Decode JPEG → RGB888
    // ----------------------------------------------------------
    snapshot_buf = (uint8_t*)malloc(320 * 240 * 3);
    if (snapshot_buf == nullptr) {
        esp_camera_fb_return(fb);
        return;
    }

    if (fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf)) {

        ei::signal_t signal;
        signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
        signal.get_data = [](size_t offset, size_t length, float *out_ptr) -> int {
            size_t pixel_ix = offset * 3;
            size_t out_ix   = 0;
            while (out_ix < length) {
                out_ptr[out_ix++] = (snapshot_buf[pixel_ix + 2] << 16)
                                  + (snapshot_buf[pixel_ix + 1] << 8)
                                  +  snapshot_buf[pixel_ix];
                pixel_ix += 3;
            }
            return 0;
        };

        ei_impulse_result_t result = { 0 };
        if (run_classifier(&signal, &result, false) == EI_IMPULSE_OK) {

            // Find highest confidence label
            String highest_label = "";
            float  highest_value = 0.0f;

            for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
                if (result.classification[i].value > highest_value) {
                    highest_value = result.classification[i].value;
                    highest_label = ei_classifier_inferencing_categories[i];
                }
            }
            highest_label.toUpperCase();

            // Debug — print all labels and their confidence
            Serial.println("--- Inference Result ---");
            for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
                Serial.print("  ");
                Serial.print(ei_classifier_inferencing_categories[i]);
                Serial.print(": ");
                Serial.print(result.classification[i].value * 100, 1);
                Serial.println("%");
            }
            Serial.print("Winner: ");
            Serial.print(highest_label);
            Serial.print(" (");
            Serial.print(highest_value * 100, 1);
            Serial.println("%)");
            Serial.println("------------------------");

            // Send if confident and not background/idle
            bool isValid = highest_value > 0.60
                        && highest_label != "BACKGROUND"
                        && highest_label != "IDLE";

            // Cooldown: avoid spamming same label
            bool cooldownOk = (highest_label != lastSentLabel)
                           || (millis() - lastSentTime >= SEND_COOLDOWN);

            if (isValid && cooldownOk) {
                Serial.print(">>> SENDING: ");
                Serial.println(highest_label);
                sendUDP(highest_label);
                lastSentLabel = highest_label;
                lastSentTime  = millis();
            }
        }
    }

    free(snapshot_buf);
    snapshot_buf = nullptr;
    esp_camera_fb_return(fb);
}