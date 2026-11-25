/*
  IoT Smart Parking System - 3 Slots
  Board: ESP32 DevKit V1
  Sensors: 3 x HC-SR04
  Indicators: 3 green LEDs + 3 red LEDs
  Network: Wi-Fi + MQTT (PubSubClient)

  Wokwi Wi-Fi:
    SSID: Wokwi-GUEST
    Password: (empty)

  // MQTT broker (classroom demonstration only):
// broker.emqx.io:1883

  Physical hardware warning:
    HC-SR04 ECHO is normally 5V. ESP32 GPIO is NOT 5V tolerant.
    Use a voltage divider or logic-level shifter on every ECHO line
    when building the real circuit. Power HC-SR04 from 5V and share GND.
*/

#include <WiFi.h>
#include <PubSubClient.h>

// -------------------- Network configuration --------------------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;

const char* TOPIC_COMMAND = "smartparking/command";
const char* TOPIC_CONNECTION = "smartparking/system/connection";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// -------------------- Hardware configuration --------------------
const uint8_t SLOT_COUNT = 3;

const uint8_t trigPins[SLOT_COUNT]  = {5, 22, 27};
const uint8_t echoPins[SLOT_COUNT]  = {18, 23, 32};
const uint8_t greenPins[SLOT_COUNT] = {19, 25, 16};
const uint8_t redPins[SLOT_COUNT]   = {21, 26, 17};

// -------------------- Detection configuration --------------------
const float OCCUPIED_THRESHOLD_CM = 15.0;
const uint8_t CONFIRMATION_READINGS = 3;
const uint8_t SENSOR_ERROR_READINGS = 3;
const unsigned long SENSOR_INTERVAL_MS = 250;
const unsigned long HEARTBEAT_INTERVAL_MS = 10000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;
const unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
const unsigned long ECHO_TIMEOUT_US = 25000;

// -------------------- Slot state --------------------
enum SlotState {
  UNKNOWN,
  AVAILABLE,
  OCCUPIED,
  SENSOR_ERROR
};

SlotState slotState[SLOT_COUNT] = {
  UNKNOWN, UNKNOWN, UNKNOWN
};

uint8_t occupiedStreak[SLOT_COUNT] = {0, 0, 0};
uint8_t availableStreak[SLOT_COUNT] = {0, 0, 0};
uint8_t errorStreak[SLOT_COUNT] = {0, 0, 0};
uint8_t validStreak[SLOT_COUNT] = {0, 0, 0};

float lastDistanceCm[SLOT_COUNT] = {-1, -1, -1};

unsigned long lastSensorScan = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;

String mqttClientId;

// -------------------- Helper functions --------------------
const char* stateName(SlotState state) {
  switch (state) {
    case AVAILABLE:   return "AVAILABLE";
    case OCCUPIED:    return "OCCUPIED";
    case SENSOR_ERROR:return "SENSOR_ERROR";
    default:          return "UNKNOWN";
  }
}

void setIndicators(uint8_t slot) {
  // Sensor error and unknown: both indicators OFF.
  digitalWrite(greenPins[slot],
               slotState[slot] == AVAILABLE ? HIGH : LOW);
  digitalWrite(redPins[slot],
               slotState[slot] == OCCUPIED ? HIGH : LOW);
}

float readDistanceCm(uint8_t slot) {
  digitalWrite(trigPins[slot], LOW);
  delayMicroseconds(3);

  digitalWrite(trigPins[slot], HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPins[slot], LOW);

  unsigned long duration =
    pulseIn(echoPins[slot], HIGH, ECHO_TIMEOUT_US);

  if (duration == 0) {
    return -1.0; // No echo / timeout
  }

  return (duration * 0.0343f) / 2.0f;
}

void publishSlotStatus(uint8_t slot) {
  if (!mqtt.connected()) return;

  char topic[80];
  snprintf(topic, sizeof(topic),
           "smartparking/slot%u/status", slot + 1);

  char payload[180];
  snprintf(payload, sizeof(payload),
    "{\"slot\":%u,\"status\":\"%s\",\"distance_cm\":%.1f,"
    "\"uptime_ms\":%lu}",
    slot + 1,
    stateName(slotState[slot]),
    lastDistanceCm[slot],
    millis()
  );

  // Retained status lets a new dashboard subscriber get the latest state.
  bool ok = mqtt.publish(topic, payload, true);
  Serial.printf("Status publish %s: %s [%s]\n",
                topic, payload, ok ? "OK" : "FAILED");
}

void publishSlotEvent(uint8_t slot, SlotState oldState,
                      SlotState newState) {
  if (!mqtt.connected()) return;

  char topic[80];
  snprintf(topic, sizeof(topic),
           "smartparking/slot%u/event", slot + 1);

  char payload[180];
  snprintf(payload, sizeof(payload),
    "{\"slot\":%u,\"from\":\"%s\",\"to\":\"%s\","
    "\"distance_cm\":%.1f,\"uptime_ms\":%lu}",
    slot + 1,
    stateName(oldState),
    stateName(newState),
    lastDistanceCm[slot],
    millis()
  );

  // Events are not retained; they describe a transition that just occurred.
  mqtt.publish(topic, payload, false);
  Serial.printf("Event: %s\n", payload);
}

void changeSlotState(uint8_t slot, SlotState newState) {
  if (newState == slotState[slot]) return;

  SlotState oldState = slotState[slot];
  slotState[slot] = newState;
  setIndicators(slot);

  Serial.printf("Slot %u: %s -> %s\n",
                slot + 1,
                stateName(oldState),
                stateName(newState));

  publishSlotEvent(slot, oldState, newState);
  publishSlotStatus(slot);
}

void processSensorReading(uint8_t slot, float distance) {
  if (distance < 0) {
    errorStreak[slot]++;
    validStreak[slot] = 0;
    occupiedStreak[slot] = 0;
    availableStreak[slot] = 0;

    if (errorStreak[slot] >= SENSOR_ERROR_READINGS) {
      changeSlotState(slot, SENSOR_ERROR);
    }
    return;
  }

  lastDistanceCm[slot] = distance;
  errorStreak[slot] = 0;
  validStreak[slot]++;

  if (distance < OCCUPIED_THRESHOLD_CM) {
    occupiedStreak[slot]++;
    availableStreak[slot] = 0;

    if (occupiedStreak[slot] >= CONFIRMATION_READINGS &&
        validStreak[slot] >= CONFIRMATION_READINGS) {
      changeSlotState(slot, OCCUPIED);
    }
  } else {
    availableStreak[slot]++;
    occupiedStreak[slot] = 0;

    if (availableStreak[slot] >= CONFIRMATION_READINGS &&
        validStreak[slot] >= CONFIRMATION_READINGS) {
      changeSlotState(slot, AVAILABLE);
    }
  }
}

// -------------------- MQTT callback --------------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();
  message.toUpperCase();

  Serial.printf("MQTT command on %s: %s\n", topic, message.c_str());

  if (String(topic) == TOPIC_COMMAND) {
    if (message == "REFRESH" || message == "STATUS") {
      for (uint8_t i = 0; i < SLOT_COUNT; i++) {
        publishSlotStatus(i);
      }
    }
  }
}

// -------------------- Connection management --------------------
void startWifiConnection() {
  if (WiFi.status() == WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastWifiAttempt < WIFI_RETRY_INTERVAL_MS) return;

  lastWifiAttempt = now;
  Serial.println("Starting Wi-Fi connection...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
}

void tryMqttConnection() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastMqttAttempt < MQTT_RETRY_INTERVAL_MS) return;

  lastMqttAttempt = now;
  Serial.print("Attempting MQTT connection... ");

  // Last Will: broker publishes OFFLINE if this client disconnects unexpectedly.
  bool connected = mqtt.connect(
    mqttClientId.c_str(),
    TOPIC_CONNECTION,
    0,
    true,
    "OFFLINE"
  );

  if (connected) {
    Serial.println("connected");
    mqtt.publish(TOPIC_CONNECTION, "ONLINE", true);
    mqtt.subscribe(TOPIC_COMMAND);

    // Synchronize current state after every MQTT reconnection.
    for (uint8_t i = 0; i < SLOT_COUNT; i++) {
      if (slotState[i] != UNKNOWN) {
        publishSlotStatus(i);
      }
    }
  } else {
    Serial.printf("failed, state=%d\n", mqtt.state());
  }
}

// -------------------- Arduino setup and loop --------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    pinMode(trigPins[i], OUTPUT);
    pinMode(echoPins[i], INPUT);
    pinMode(greenPins[i], OUTPUT);
    pinMode(redPins[i], OUTPUT);

    digitalWrite(trigPins[i], LOW);
    digitalWrite(greenPins[i], LOW);
    digitalWrite(redPins[i], LOW);
  }

  mqttClientId = "SmartParking-";
  mqttClientId += String((uint32_t)ESP.getEfuseMac(), HEX);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(2);

  Serial.println();
  Serial.println("Connecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("Wi-Fi CONNECTED!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("Wi-Fi CONNECTION FAILED!");
  }
  lastWifiAttempt = millis();

  Serial.println();
  Serial.println("=== SMART PARKING SYSTEM STARTED ===");
  Serial.println("3 parking slots configured.");
  Serial.printf("Threshold: %.0f cm\n", OCCUPIED_THRESHOLD_CM);
  Serial.println("MQTT topics: smartparking/slotN/status");
}

void loop() {
  // Maintain network services without blocking local sensing.
  startWifiConnection();

  if (WiFi.status() == WL_CONNECTED) {
    tryMqttConnection();
  }

  if (mqtt.connected()) {
    mqtt.loop();
  }

  // Scan all three sensors at a controlled interval.
  unsigned long now = millis();
  if (now - lastSensorScan >= SENSOR_INTERVAL_MS) {
    lastSensorScan = now;

    for (uint8_t i = 0; i < SLOT_COUNT; i++) {
      float distance = readDistanceCm(i);
      processSensorReading(i, distance);

      Serial.printf("Slot %u distance: %.1f cm | state: %s\n",
                    i + 1,
                    distance,
                    stateName(slotState[i]));
    }
  }

  // Periodic retained status heartbeat.
  now = millis();
  if (mqtt.connected() &&
      now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;

    for (uint8_t i = 0; i < SLOT_COUNT; i++) {
      publishSlotStatus(i);
    }
  }
}


