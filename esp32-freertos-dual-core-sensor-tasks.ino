/*
 * ESP32 FreeRTOS Dual-Core Sensor Tasks
 * -------------------------------------
 * Splits an IoT node into two FreeRTOS tasks pinned to different cores:
 *
 *   core 1  sensorTask   samples an analog input at a fixed 10 Hz using
 *                        vTaskDelayUntil() and pushes each Sample into a
 *                        queue. It never touches WiFi, so its timing stays
 *                        stable even while the radio stack blocks.
 *   core 0  networkTask  owns WiFi + MQTT. It drains the queue, aggregates a
 *                        10 second window (avg/min/max) and publishes JSON.
 *                        Reconnects block only this task.
 *   core 1  loop()       the normal Arduino task, prints a status line every
 *                        5 s from a mutex-protected shared stats struct.
 *
 * What it demonstrates:
 *   - xTaskCreatePinnedToCore() and xPortGetCoreID()
 *   - a FreeRTOS queue as the hand-off between producer and consumer
 *   - a mutex (xSemaphoreCreateMutex) guarding shared counters
 *   - vTaskDelayUntil() for drift-free periodic sampling
 *   - measuring sampling jitter and queue drops so you can prove timing holds
 *
 * Required libraries (Library Manager):
 *   - "PubSubClient" by Nick O'Leary
 *   FreeRTOS, WiFi and the ADC driver are built into the ESP32 Arduino core.
 *
 * Wiring (any analog sensor or a potentiometer):
 *   Sensor / pot signal -> ESP32 GPIO34  (ADC1 - ADC2 pins stop working when
 *                                         WiFi is on, so stay on GPIO32-39)
 *   Sensor VCC          -> ESP32 3V3
 *   Sensor GND          -> ESP32 GND
 *
 * Setup:
 *   1. Copy config.h.example to config.h and fill in WiFi + broker.
 *   2. Flash, open Serial Monitor at 115200.
 *
 * Sample payload:
 *   {"avg_v":1.648,"min_v":1.612,"max_v":1.690,"samples":100,"dropped":0,
 *    "max_jitter_us":38,"seq":1299,"heap":231204,"uptime_s":130}
 *
 * (c) 2026 Fluidwire. Released under the MIT License.
 * https://github.com/fluidwire
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "config.h"

// --- Tunables ---------------------------------------------------------------
const int      SENSOR_PIN          = 34;       // ADC1_CH6, WiFi-safe
const uint32_t SAMPLE_PERIOD_MS    = 100UL;    // 10 Hz
const uint32_t PUBLISH_INTERVAL_MS = 10000UL;
const uint32_t STATUS_INTERVAL_MS  = 5000UL;
const UBaseType_t QUEUE_LENGTH     = 200;      // ~20 s of samples buffered during a reconnect

// --- Shared types -----------------------------------------------------------
struct Sample {
  uint32_t seq;
  uint32_t ms;
  uint16_t raw;
  float    volts;
};

struct SharedStats {
  uint32_t samplesTaken;
  uint32_t samplesDropped;   // queue was full when the sensor task tried to push
  uint32_t maxJitterUs;      // worst deviation from SAMPLE_PERIOD_MS seen so far
  float    lastVolts;
  uint32_t published;
  bool     mqttUp;
};

// --- Globals ----------------------------------------------------------------
WiFiClient        wifiClient;
PubSubClient      mqtt(wifiClient);

QueueHandle_t     sampleQueue       = nullptr;
SemaphoreHandle_t statsMutex        = nullptr;
SharedStats       stats             = {};
TaskHandle_t      sensorTaskHandle  = nullptr;
TaskHandle_t      networkTaskHandle = nullptr;

// Small helper so every writer/reader of `stats` goes through the mutex.
template <typename F>
void withStats(F fn) {
  if (xSemaphoreTake(statsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    fn(stats);
    xSemaphoreGive(statsMutex);
  }
}

// --- Core 1: sensor producer ------------------------------------------------
void sensorTask(void *param) {
  (void)param;
  Serial.printf("[sensor] running on core %d\n", xPortGetCoreID());

  const TickType_t period   = pdMS_TO_TICKS(SAMPLE_PERIOD_MS);
  TickType_t       lastWake = xTaskGetTickCount();
  uint32_t         seq      = 0;
  uint32_t         lastUs   = micros();

  for (;;) {
    // Sleeps until exactly one period after the previous wake, so the
    // schedule does not drift even if the body below takes a while.
    vTaskDelayUntil(&lastWake, period);

    uint32_t nowUs    = micros();
    int32_t  deltaUs  = (int32_t)(nowUs - lastUs) - (int32_t)(SAMPLE_PERIOD_MS * 1000UL);
    uint32_t jitterUs = (uint32_t)(deltaUs < 0 ? -deltaUs : deltaUs);
    lastUs = nowUs;

    Sample s;
    s.seq   = seq++;
    s.ms    = millis();
    s.raw   = (uint16_t)analogRead(SENSOR_PIN);
    s.volts = analogReadMilliVolts(SENSOR_PIN) / 1000.0f;

    // Non-blocking push: if the consumer is stuck (WiFi down for ages) we
    // count the drop rather than stall the sampler.
    bool queued = (xQueueSend(sampleQueue, &s, 0) == pdTRUE);

    withStats([&](SharedStats &st) {
      st.samplesTaken++;
      if (!queued) st.samplesDropped++;
      if (jitterUs > st.maxJitterUs) st.maxJitterUs = jitterUs;
      st.lastVolts = s.volts;
    });
  }
}

// --- Core 0: network consumer -----------------------------------------------
void connectWiFi() {
  Serial.print(F("[wifi] connecting to "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));   // blocks only this task; sampling continues
    Serial.print('.');
  }
  Serial.println();
  Serial.print(F("[wifi] IP: "));
  Serial.println(WiFi.localIP());
}

bool connectMQTT() {
  Serial.print(F("[mqtt] connecting... "));
  bool ok = strlen(MQTT_USER) > 0
    ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
    : mqtt.connect(MQTT_CLIENT_ID);
  if (ok) {
    Serial.println(F("ok"));
  } else {
    Serial.print(F("failed, state="));
    Serial.println(mqtt.state());
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  return ok;
}

void publishWindow(uint32_t count, float sum, float minV, float maxV, uint32_t lastSeq) {
  uint32_t dropped = 0, jitter = 0;
  withStats([&](SharedStats &st) {
    dropped = st.samplesDropped;
    jitter  = st.maxJitterUs;
  });

  char buf[220];
  snprintf(buf, sizeof(buf),
           "{\"avg_v\":%.3f,\"min_v\":%.3f,\"max_v\":%.3f,\"samples\":%lu,\"dropped\":%lu,"
           "\"max_jitter_us\":%lu,\"seq\":%lu,\"heap\":%lu,\"uptime_s\":%lu}",
           sum / count, minV, maxV,
           (unsigned long)count, (unsigned long)dropped, (unsigned long)jitter,
           (unsigned long)lastSeq, (unsigned long)ESP.getFreeHeap(),
           (unsigned long)(millis() / 1000UL));

  bool ok = mqtt.publish(MQTT_TOPIC, buf);
  Serial.print(F("[mqtt] "));
  Serial.print(ok ? F("published ") : F("FAILED "));
  Serial.println(buf);

  withStats([&](SharedStats &st) {
    if (ok) st.published++;
  });
}

void networkTask(void *param) {
  (void)param;
  Serial.printf("[net] running on core %d\n", xPortGetCoreID());

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  connectWiFi();

  uint32_t windowCount = 0;
  float    windowSum   = 0.0f;
  float    windowMin   = 99.0f;
  float    windowMax   = -99.0f;
  uint32_t lastSeq     = 0;
  uint32_t lastPublish = millis();

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      withStats([](SharedStats &st) { st.mqttUp = false; });
      connectWiFi();
    }
    if (!mqtt.connected()) {
      withStats([](SharedStats &st) { st.mqttUp = false; });
      if (!connectMQTT()) continue;   // retry; the queue keeps buffering meanwhile
      withStats([](SharedStats &st) { st.mqttUp = true; });
    }
    mqtt.loop();

    // Drain everything the sensor task has produced since last time.
    Sample s;
    while (xQueueReceive(sampleQueue, &s, 0) == pdTRUE) {
      windowCount++;
      windowSum += s.volts;
      if (s.volts < windowMin) windowMin = s.volts;
      if (s.volts > windowMax) windowMax = s.volts;
      lastSeq = s.seq;
    }

    if (millis() - lastPublish >= PUBLISH_INTERVAL_MS && windowCount > 0) {
      publishWindow(windowCount, windowSum, windowMin, windowMax, lastSeq);
      windowCount = 0;
      windowSum   = 0.0f;
      windowMin   = 99.0f;
      windowMax   = -99.0f;
      lastPublish = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// --- Arduino entry points (loopTask runs on core 1) -------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("[main] setup on core %d\n", xPortGetCoreID());

  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);   // full 0-3.3 V range

  sampleQueue = xQueueCreate(QUEUE_LENGTH, sizeof(Sample));
  statsMutex  = xSemaphoreCreateMutex();
  if (sampleQueue == nullptr || statsMutex == nullptr) {
    Serial.println(F("[rtos] failed to create queue or mutex"));
    while (true) delay(1000);
  }

  // Sensor task: higher priority than loopTask (1) so it always wins core 1.
  xTaskCreatePinnedToCore(sensorTask,  "sensor",  4096, nullptr, 2, &sensorTaskHandle,  1);
  // Network task: lives on core 0 next to the WiFi/LwIP stack it talks to.
  xTaskCreatePinnedToCore(networkTask, "network", 8192, nullptr, 1, &networkTaskHandle, 0);
}

void loop() {
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = millis();

    SharedStats snap = {};
    withStats([&](SharedStats &st) { snap = st; });

    Serial.printf("[status] samples=%lu dropped=%lu maxJitter=%luus last=%.3fV "
                  "queued=%u published=%lu mqtt=%s stack(sensor/net)=%u/%u\n",
                  (unsigned long)snap.samplesTaken,
                  (unsigned long)snap.samplesDropped,
                  (unsigned long)snap.maxJitterUs,
                  snap.lastVolts,
                  (unsigned)uxQueueMessagesWaiting(sampleQueue),
                  (unsigned long)snap.published,
                  snap.mqttUp ? "up" : "down",
                  (unsigned)uxTaskGetStackHighWaterMark(sensorTaskHandle),
                  (unsigned)uxTaskGetStackHighWaterMark(networkTaskHandle));
  }
  delay(100);
}
