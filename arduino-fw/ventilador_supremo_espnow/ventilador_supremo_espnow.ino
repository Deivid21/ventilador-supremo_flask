//
// For more Support see: https://t.me/Deivid21Hub
//
// Copyright (C) 1996 - 2026 INACAP
// Copyright (C) 2017 - 2026 Deivid Ignacio Parra (Deivid21)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Board: ESP32
// Code-by: Cristian Muena & Deivid Ignacio Parra (Deivid21)
//

/*
 * Supreme Fan - ESP-NOW sensor and actuator node
 *
 * This ESP32 reads the DHT22 and HC-SR04, controls the fan locally and sends
 * telemetry to the gateway. It automatically scans Wi-Fi channels until the
 * gateway answers, so the router channel does not need to be hardcoded.
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

// -----------------------------------------------------------------------------
// ESP-NOW protocol
// -----------------------------------------------------------------------------
constexpr uint32_t PACKET_MAGIC = 0x56464E31;  // "VFN1"
constexpr uint8_t PACKET_VERSION = 1;

constexpr uint8_t FIRST_WIFI_CHANNEL = 1;
constexpr uint8_t LAST_WIFI_CHANNEL = 13;
constexpr unsigned long CHANNEL_SCAN_INTERVAL_MS = 400;
constexpr unsigned long GATEWAY_TIMEOUT_MS = 10000;
constexpr unsigned long TELEMETRY_INTERVAL_MS = 2000;

const uint8_t BROADCAST_MAC[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

enum class PacketType : uint8_t {
  Telemetry = 1,
  Command = 2,
  Acknowledgement = 3
};

enum class RemoteCommand : uint8_t {
  None = 0,
  SetAuto = 1,
  SetManual = 2,
  FanOn = 3,
  FanOff = 4
};

struct __attribute__((packed)) PacketHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
};

struct __attribute__((packed)) TelemetryPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint32_t sequence;
  uint32_t uptimeMs;
  float temperatureC;
  float temperatureF;
  float humidity;
  int16_t distanceCm;
  uint8_t fanOn;
  uint8_t manualMode;
  uint8_t sensorsValid;
};

struct __attribute__((packed)) CommandPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t command;
  uint32_t commandId;
};

struct __attribute__((packed)) AcknowledgementPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t channel;
};

// -----------------------------------------------------------------------------
// Hardware
// -----------------------------------------------------------------------------
constexpr int TRIG_PIN = 5;
constexpr int ECHO_PIN = 18;
constexpr int LED_1_PIN = 2;
constexpr int LED_2_PIN = 15;
constexpr int MOTOR_IN_1_PIN = 17;
constexpr int MOTOR_IN_2_PIN = 19;
constexpr int BUTTON_PIN = 14;
constexpr int DHT_PIN = 4;

#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// -----------------------------------------------------------------------------
// Control configuration
// -----------------------------------------------------------------------------
constexpr unsigned long BUTTON_DEBOUNCE_MS = 50;
constexpr unsigned long LONG_PRESS_MS = 1500;
constexpr unsigned long SENSOR_INTERVAL_MS = 2000;
constexpr unsigned long LCD_REFRESH_MS = 500;
constexpr unsigned long ULTRASONIC_TIMEOUT_US = 30000;

constexpr float TEMP_ON_C = 25.0F;
constexpr float TEMP_OFF_C = 24.9F;
constexpr int DISTANCE_MIN_CM = 1;
constexpr int DISTANCE_MAX_CM = 10;

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------
struct SensorReading {
  float temperatureC = NAN;
  float temperatureF = NAN;
  float humidity = NAN;
  int distanceCm = -1;
  bool dhtValid = false;
  bool ultrasonicValid = false;
};

SensorReading sensorReading;

bool fanOn = false;
bool manualMode = false;
bool manualFanState = false;

bool previousManualMode = false;
bool previousObjectNear = false;
bool previousTemperatureHigh = false;

bool buttonLastRawLevel = HIGH;
bool buttonStableLevel = HIGH;
bool buttonWasPressed = false;

unsigned long buttonLastChangeMs = 0;
unsigned long buttonPressStartMs = 0;
unsigned long lastSensorReadMs = 0;
unsigned long lastTelemetryMs = 0;
unsigned long lastLcdRefreshMs = 0;
unsigned long lastChannelScanMs = 0;
unsigned long lastGatewayContactMs = 0;

uint32_t telemetrySequence = 0;
uint8_t activeChannel = FIRST_WIFI_CHANNEL;
bool gatewayLinked = false;

volatile bool acknowledgementPending = false;
volatile uint8_t acknowledgedChannel = 0;

volatile bool commandPending = false;
volatile uint8_t pendingCommandValue = static_cast<uint8_t>(RemoteCommand::None);
volatile uint32_t pendingCommandId = 0;
uint32_t lastAppliedCommandId = 0;

portMUX_TYPE espNowMux = portMUX_INITIALIZER_UNLOCKED;

// -----------------------------------------------------------------------------
// Display
// -----------------------------------------------------------------------------
void writeLcdLine(uint8_t row, const char* text) {
  char padded[17];
  snprintf(padded, sizeof(padded), "%-16.16s", text);
  lcd.setCursor(0, row);
  lcd.print(padded);
}

void showTransientMessage(const char* line1, const char* line2) {
  writeLcdLine(0, line1);
  writeLcdLine(1, line2);
  lastLcdRefreshMs = millis();
}

void refreshNormalDisplay() {
  if (millis() - lastLcdRefreshMs < LCD_REFRESH_MS) {
    return;
  }

  lastLcdRefreshMs = millis();

  char line1[17];
  char line2[17];

  if (sensorReading.dhtValid) {
    snprintf(
      line1,
      sizeof(line1),
      "T:%4.1fC H:%2.0f%%",
      sensorReading.temperatureC,
      sensorReading.humidity
    );
  } else {
    snprintf(line1, sizeof(line1), "DHT22: NO DATA");
  }

  snprintf(
    line2,
    sizeof(line2),
    "%s F:%s C:%02u",
    manualMode ? "MAN" : "AUT",
    fanOn ? "ON" : "OFF",
    activeChannel
  );

  writeLcdLine(0, line1);
  writeLcdLine(1, line2);
}

// -----------------------------------------------------------------------------
// Fan control
// -----------------------------------------------------------------------------
void stopMotor() {
  digitalWrite(MOTOR_IN_1_PIN, LOW);
  digitalWrite(MOTOR_IN_2_PIN, LOW);
}

void runMotorForward() {
  digitalWrite(MOTOR_IN_1_PIN, HIGH);
  digitalWrite(MOTOR_IN_2_PIN, LOW);
}

void setFan(bool enabled) {
  fanOn = enabled;

  if (fanOn) {
    runMotorForward();
    digitalWrite(LED_1_PIN, HIGH);
    digitalWrite(LED_2_PIN, HIGH);
  } else {
    stopMotor();
    digitalWrite(LED_1_PIN, LOW);
    digitalWrite(LED_2_PIN, LOW);
  }
}

// -----------------------------------------------------------------------------
// Sensors
// -----------------------------------------------------------------------------
int measureDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  const unsigned long durationUs = pulseIn(
    ECHO_PIN,
    HIGH,
    ULTRASONIC_TIMEOUT_US
  );

  if (durationUs == 0) {
    return -1;
  }

  return static_cast<int>((durationUs * 0.0343F) / 2.0F);
}

void readSensors() {
  const float humidity = dht.readHumidity();
  const float temperatureC = dht.readTemperature();

  sensorReading.dhtValid = !isnan(humidity) && !isnan(temperatureC);

  if (sensorReading.dhtValid) {
    sensorReading.humidity = humidity;
    sensorReading.temperatureC = temperatureC;
    sensorReading.temperatureF = (temperatureC * 9.0F / 5.0F) + 32.0F;
  } else {
    sensorReading.humidity = NAN;
    sensorReading.temperatureC = NAN;
    sensorReading.temperatureF = NAN;
  }

  sensorReading.distanceCm = measureDistanceCm();
  sensorReading.ultrasonicValid = sensorReading.distanceCm >= 0;

  Serial.printf(
    "[SENSORS] Temp: %.1f C | Humidity: %.1f %% | Distance: %d cm | Valid: %s\n",
    sensorReading.temperatureC,
    sensorReading.humidity,
    sensorReading.distanceCm,
    (sensorReading.dhtValid && sensorReading.ultrasonicValid) ? "YES" : "NO"
  );
}

// -----------------------------------------------------------------------------
// Local control
// -----------------------------------------------------------------------------
void updateAutomaticControl() {
  const bool objectNear = sensorReading.ultrasonicValid &&
    sensorReading.distanceCm >= DISTANCE_MIN_CM &&
    sensorReading.distanceCm <= DISTANCE_MAX_CM;

  const bool temperatureHigh = sensorReading.dhtValid &&
    sensorReading.temperatureC >= TEMP_ON_C;

  const bool temperatureNormal = sensorReading.dhtValid &&
    sensorReading.temperatureC < TEMP_OFF_C;

  if (!fanOn && (temperatureHigh || objectNear)) {
    setFan(true);
  } else if (fanOn && temperatureNormal && !objectNear) {
    setFan(false);
  }
}

void processStateEvents() {
  const bool objectNear = sensorReading.ultrasonicValid &&
    sensorReading.distanceCm >= DISTANCE_MIN_CM &&
    sensorReading.distanceCm <= DISTANCE_MAX_CM;

  const bool temperatureHigh = sensorReading.dhtValid &&
    sensorReading.temperatureC >= TEMP_ON_C;

  if (manualMode != previousManualMode) {
    showTransientMessage(
      manualMode ? "MODE: MANUAL" : "MODE: AUTO",
      manualMode ? "Remote/local" : "Rules active"
    );
    previousManualMode = manualMode;
  }

  if (objectNear && !previousObjectNear) {
    showTransientMessage("OBJECT DETECTED", "Cooling active");
  }

  if (temperatureHigh && !previousTemperatureHigh) {
    showTransientMessage("HIGH TEMP", "Cooling active");
  }

  previousObjectNear = objectNear;
  previousTemperatureHigh = temperatureHigh;
}

void handleButton() {
  const bool rawLevel = digitalRead(BUTTON_PIN);
  const unsigned long now = millis();

  if (rawLevel != buttonLastRawLevel) {
    buttonLastRawLevel = rawLevel;
    buttonLastChangeMs = now;
  }

  if (now - buttonLastChangeMs < BUTTON_DEBOUNCE_MS) {
    return;
  }

  if (buttonStableLevel == rawLevel) {
    return;
  }

  buttonStableLevel = rawLevel;

  if (buttonStableLevel == LOW) {
    buttonWasPressed = true;
    buttonPressStartMs = now;
    return;
  }

  if (!buttonWasPressed) {
    return;
  }

  buttonWasPressed = false;
  const unsigned long pressDurationMs = now - buttonPressStartMs;

  if (pressDurationMs >= LONG_PRESS_MS) {
    manualMode = !manualMode;
    manualFanState = fanOn;
    Serial.printf("[BUTTON] Mode: %s\n", manualMode ? "MANUAL" : "AUTO");
  } else if (manualMode) {
    manualFanState = !manualFanState;
    setFan(manualFanState);
    Serial.printf("[BUTTON] Fan: %s\n", fanOn ? "ON" : "OFF");
  }
}

// -----------------------------------------------------------------------------
// ESP-NOW
// -----------------------------------------------------------------------------
bool setRadioChannel(uint8_t channel) {
  if (channel < FIRST_WIFI_CHANNEL || channel > LAST_WIFI_CHANNEL) {
    return false;
  }

  const esp_err_t result = esp_wifi_set_channel(
    channel,
    WIFI_SECOND_CHAN_NONE
  );

  if (result != ESP_OK) {
    Serial.printf("[ESP-NOW] Cannot set channel %u: %d\n", channel, result);
    return false;
  }

  activeChannel = channel;
  return true;
}

void handleEspNowPacket(
  const uint8_t* sourceMac,
  const uint8_t* incomingData,
  int length
) {
  if (
    sourceMac == nullptr ||
    incomingData == nullptr ||
    length < static_cast<int>(sizeof(PacketHeader))
  ) {
    return;
  }

  PacketHeader header;
  memcpy(&header, incomingData, sizeof(header));

  if (
    header.magic != PACKET_MAGIC ||
    header.version != PACKET_VERSION
  ) {
    return;
  }

  if (
    header.type == static_cast<uint8_t>(PacketType::Acknowledgement) &&
    length == static_cast<int>(sizeof(AcknowledgementPacket))
  ) {
    AcknowledgementPacket packet;
    memcpy(&packet, incomingData, sizeof(packet));

    portENTER_CRITICAL(&espNowMux);
    acknowledgedChannel = packet.channel;
    acknowledgementPending = true;
    portEXIT_CRITICAL(&espNowMux);
    return;
  }

  if (
    header.type == static_cast<uint8_t>(PacketType::Command) &&
    length == static_cast<int>(sizeof(CommandPacket))
  ) {
    CommandPacket packet;
    memcpy(&packet, incomingData, sizeof(packet));

    portENTER_CRITICAL(&espNowMux);
    pendingCommandValue = packet.command;
    pendingCommandId = packet.commandId;
    commandPending = true;
    portEXIT_CRITICAL(&espNowMux);
  }
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowReceive(
  const esp_now_recv_info_t* info,
  const uint8_t* incomingData,
  int length
) {
  if (info == nullptr) {
    return;
  }

  handleEspNowPacket(info->src_addr, incomingData, length);
}
#else
void onEspNowReceive(
  const uint8_t* sourceMac,
  const uint8_t* incomingData,
  int length
) {
  handleEspNowPacket(sourceMac, incomingData, length);
}
#endif

bool initializeEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(100);

  if (!setRadioChannel(FIRST_WIFI_CHANNEL)) {
    return false;
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Initialization failed");
    return false;
  }

  if (esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) {
    Serial.println("[ESP-NOW] Receive callback registration failed");
    return false;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, BROADCAST_MAC, sizeof(BROADCAST_MAC));
  peerInfo.channel = 0;  // Always use the current radio channel.
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  const esp_err_t peerResult = esp_now_add_peer(&peerInfo);
  if (peerResult != ESP_OK && peerResult != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("[ESP-NOW] Broadcast peer error: %d\n", peerResult);
    return false;
  }

  Serial.print("[ESP-NOW] Fan MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("[ESP-NOW] Automatic channel discovery enabled");
  return true;
}

void sendTelemetry() {
  TelemetryPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = PACKET_VERSION;
  packet.type = static_cast<uint8_t>(PacketType::Telemetry);
  packet.sequence = ++telemetrySequence;
  packet.uptimeMs = millis();
  packet.temperatureC = sensorReading.temperatureC;
  packet.temperatureF = sensorReading.temperatureF;
  packet.humidity = sensorReading.humidity;
  packet.distanceCm = sensorReading.distanceCm;
  packet.fanOn = fanOn ? 1 : 0;
  packet.manualMode = manualMode ? 1 : 0;
  packet.sensorsValid = (
    sensorReading.dhtValid && sensorReading.ultrasonicValid
  ) ? 1 : 0;

  const esp_err_t result = esp_now_send(
    BROADCAST_MAC,
    reinterpret_cast<const uint8_t*>(&packet),
    sizeof(packet)
  );

  if (result == ESP_OK) {
    Serial.printf(
      "[ESP-NOW TX] Seq: %lu | Channel: %u | Link: %s\n",
      static_cast<unsigned long>(packet.sequence),
      activeChannel,
      gatewayLinked ? "CONNECTED" : "SEARCHING"
    );
  } else {
    Serial.printf(
      "[ESP-NOW TX] Queue error: %d | Channel: %u\n",
      result,
      activeChannel
    );
  }
}

void processAcknowledgement() {
  if (!acknowledgementPending) {
    return;
  }

  uint8_t channel;

  portENTER_CRITICAL(&espNowMux);
  channel = acknowledgedChannel;
  acknowledgementPending = false;
  portEXIT_CRITICAL(&espNowMux);

  if (channel >= FIRST_WIFI_CHANNEL && channel <= LAST_WIFI_CHANNEL) {
    setRadioChannel(channel);
  }

  const bool wasLinked = gatewayLinked;
  gatewayLinked = true;
  lastGatewayContactMs = millis();

  if (!wasLinked) {
    Serial.printf(
      "[LINK] Gateway found on Wi-Fi channel %u\n",
      activeChannel
    );
    showTransientMessage("ESP-NOW LINKED", "Gateway found");
  }
}

void scanForGateway() {
  if (gatewayLinked) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastChannelScanMs < CHANNEL_SCAN_INTERVAL_MS) {
    return;
  }

  lastChannelScanMs = now;

  setRadioChannel(activeChannel);
  Serial.printf("[LINK] Searching on channel %u\n", activeChannel);
  sendTelemetry();

  activeChannel++;
  if (activeChannel > LAST_WIFI_CHANNEL) {
    activeChannel = FIRST_WIFI_CHANNEL;
  }
}

void checkGatewayTimeout() {
  if (
    gatewayLinked &&
    millis() - lastGatewayContactMs > GATEWAY_TIMEOUT_MS
  ) {
    gatewayLinked = false;
    activeChannel = FIRST_WIFI_CHANNEL;
    lastChannelScanMs = 0;
    Serial.println("[LINK] Gateway lost. Restarting channel scan");
    showTransientMessage("LINK LOST", "Scanning...");
  }
}

void applyPendingCommand() {
  if (!commandPending) {
    return;
  }

  uint8_t commandValue;
  uint32_t commandId;

  portENTER_CRITICAL(&espNowMux);
  commandValue = pendingCommandValue;
  commandId = pendingCommandId;
  commandPending = false;
  portEXIT_CRITICAL(&espNowMux);

  gatewayLinked = true;
  lastGatewayContactMs = millis();

  if (commandId != 0 && commandId <= lastAppliedCommandId) {
    return;
  }

  const RemoteCommand command = static_cast<RemoteCommand>(commandValue);

  switch (command) {
    case RemoteCommand::SetAuto:
      manualMode = false;
      Serial.println("[REMOTE] AUTO mode");
      break;

    case RemoteCommand::SetManual:
      manualMode = true;
      manualFanState = fanOn;
      Serial.println("[REMOTE] MANUAL mode");
      break;

    case RemoteCommand::FanOn:
      manualMode = true;
      manualFanState = true;
      setFan(true);
      Serial.println("[REMOTE] Fan ON");
      break;

    case RemoteCommand::FanOff:
      manualMode = true;
      manualFanState = false;
      setFan(false);
      Serial.println("[REMOTE] Fan OFF");
      break;

    case RemoteCommand::None:
    default:
      return;
  }

  if (commandId != 0) {
    lastAppliedCommandId = commandId;
  }
}

// -----------------------------------------------------------------------------
// Arduino
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_1_PIN, OUTPUT);
  pinMode(LED_2_PIN, OUTPUT);
  pinMode(MOTOR_IN_1_PIN, OUTPUT);
  pinMode(MOTOR_IN_2_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(TRIG_PIN, LOW);
  setFan(false);

  lcd.init();
  lcd.backlight();
  showTransientMessage("SUPREME FAN", "Starting...");

  dht.begin();

  if (!initializeEspNow()) {
    showTransientMessage("ESP-NOW ERROR", "Restart board");
  }

  delay(1000);
  readSensors();
  lastSensorReadMs = millis();

  Serial.println("[SYSTEM] Supreme Fan ready");
}

void loop() {
  const unsigned long now = millis();

  handleButton();
  processAcknowledgement();
  applyPendingCommand();
  checkGatewayTimeout();

  if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = now;
    readSensors();
  }

  if (manualMode) {
    if (fanOn != manualFanState) {
      setFan(manualFanState);
    }
  } else {
    updateAutomaticControl();
  }

  processStateEvents();
  refreshNormalDisplay();

  if (gatewayLinked) {
    if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
      lastTelemetryMs = now;
      sendTelemetry();
    }
  } else {
    scanForGateway();
  }

  delay(10);
}
