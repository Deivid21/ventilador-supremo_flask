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
 * Supreme Fan - ESP-NOW, Wi-Fi and Flask gateway
 *
 * ESP-NOW starts immediately, even if Wi-Fi credentials are wrong. The gateway
 * answers discovery packets with an acknowledgement containing its current
 * channel. This allows the fan node to find the gateway automatically.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>

// -----------------------------------------------------------------------------
// Wi-Fi and Flask configuration
// -----------------------------------------------------------------------------
const char* WIFI_SSID = "Your WiFi SSID";
const char* WIFI_PASSWORD = "Your WiFi Password";
const char* FLASK_BASE_URL = "http://10.151.147.135:5000";

// LabVIEW UDP configuration.
// LABVIEW_IP is the computer running the LabVIEW VI.
const char* LABVIEW_IP = "10.151.147.12";
constexpr uint16_t LABVIEW_TELEMETRY_PORT = 5000;
constexpr uint16_t LABVIEW_COMMAND_PORT = 5001;

// -----------------------------------------------------------------------------
// Hardware and timing
// -----------------------------------------------------------------------------
constexpr int GREEN_LED_PIN = 25;
constexpr int RED_LED_PIN = 26;
constexpr int BLUE_LED_PIN = 27;

constexpr float ALERT_TEMPERATURE_C = 25.0F;
constexpr int ALERT_DISTANCE_MIN_CM = 1;
constexpr int ALERT_DISTANCE_MAX_CM = 10;

constexpr unsigned long DATA_POST_INTERVAL_MS = 3000;
constexpr unsigned long COMMAND_POLL_INTERVAL_MS = 2000;
constexpr unsigned long LABVIEW_TELEMETRY_INTERVAL_MS = 1000;
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
constexpr unsigned long TELEMETRY_STALE_MS = 7000;
constexpr uint16_t HTTP_TIMEOUT_MS = 2500;

constexpr uint8_t FALLBACK_WIFI_CHANNEL = 1;

// -----------------------------------------------------------------------------
// ESP-NOW protocol
// -----------------------------------------------------------------------------
constexpr uint32_t PACKET_MAGIC = 0x56464E31;  // "VFN1"
constexpr uint8_t PACKET_VERSION = 1;

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
// Runtime state
// -----------------------------------------------------------------------------
TelemetryPacket telemetryBuffer = {};
TelemetryPacket latestTelemetry = {};

volatile bool newTelemetryAvailable = false;
bool telemetryAvailable = false;

uint8_t pendingFanNodeMac[6] = {};
uint8_t fanNodeMac[6] = {};
volatile bool fanNodeMacPending = false;

bool fanNodeKnown = false;
bool fanNodePeerReady = false;
bool acknowledgementRequired = false;

volatile int8_t pendingRssi = 0;
int8_t latestRssi = 0;

unsigned long lastTelemetryReceivedMs = 0;
unsigned long lastDataPostMs = 0;
unsigned long lastCommandPollMs = 0;
unsigned long lastLabViewTelemetryMs = 0;
unsigned long lastWiFiAttemptMs = 0;

uint32_t lastFlaskCommandId = 0;
uint32_t espNowCommandSequence = 0;
uint8_t lastReportedChannel = 0;
bool previousWiFiConnected = false;
bool espNowReady = false;
bool udpReady = false;
bool pendingLabViewCommandAvailable = false;
RemoteCommand pendingLabViewCommand = RemoteCommand::None;

WiFiUDP labViewTelemetryUdp;
WiFiUDP labViewCommandUdp;
IPAddress labViewIpAddress;

portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

// Forward declarations for UDP services used by the Wi-Fi state machine.
void startLabViewUdp();
void stopLabViewUdp();
void processPendingLabViewCommand();

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------
void printMacAddress(const uint8_t* mac) {
  Serial.printf(
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
}

uint8_t readCurrentWiFiChannel() {
  uint8_t primaryChannel = 0;
  wifi_second_chan_t secondaryChannel = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&primaryChannel, &secondaryChannel);
  return primaryChannel;
}

bool telemetryIsStale() {
  return !telemetryAvailable ||
    (millis() - lastTelemetryReceivedMs > TELEMETRY_STALE_MS);
}

bool systemAlertActive() {
  if (!telemetryAvailable || telemetryIsStale()) {
    return true;
  }

  const bool temperatureAlert = latestTelemetry.sensorsValid &&
    latestTelemetry.temperatureC >= ALERT_TEMPERATURE_C;

  const bool distanceAlert = latestTelemetry.sensorsValid &&
    latestTelemetry.distanceCm >= ALERT_DISTANCE_MIN_CM &&
    latestTelemetry.distanceCm <= ALERT_DISTANCE_MAX_CM;

  return temperatureAlert || distanceAlert;
}

// -----------------------------------------------------------------------------
// ESP-NOW receive
// -----------------------------------------------------------------------------
void handleEspNowPacket(
  const uint8_t* sourceMac,
  const uint8_t* incomingData,
  int length,
  int8_t rssi
) {
  if (
    sourceMac == nullptr ||
    incomingData == nullptr ||
    length != static_cast<int>(sizeof(TelemetryPacket))
  ) {
    return;
  }

  TelemetryPacket packet;
  memcpy(&packet, incomingData, sizeof(packet));

  if (
    packet.magic != PACKET_MAGIC ||
    packet.version != PACKET_VERSION ||
    packet.type != static_cast<uint8_t>(PacketType::Telemetry)
  ) {
    return;
  }

  portENTER_CRITICAL(&telemetryMux);
  telemetryBuffer = packet;
  memcpy(pendingFanNodeMac, sourceMac, sizeof(pendingFanNodeMac));
  pendingRssi = rssi;
  newTelemetryAvailable = true;
  fanNodeMacPending = true;
  portEXIT_CRITICAL(&telemetryMux);
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

  const int8_t rssi = info->rx_ctrl != nullptr
    ? info->rx_ctrl->rssi
    : 0;

  handleEspNowPacket(info->src_addr, incomingData, length, rssi);
}
#else
void onEspNowReceive(
  const uint8_t* sourceMac,
  const uint8_t* incomingData,
  int length
) {
  handleEspNowPacket(sourceMac, incomingData, length, 0);
}
#endif

bool initializeEspNow() {
  if (espNowReady) {
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(100);

  esp_wifi_set_channel(FALLBACK_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Initialization failed");
    return false;
  }

  if (esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) {
    Serial.println("[ESP-NOW] Receive callback registration failed");
    return false;
  }

  espNowReady = true;

  Serial.println("[ESP-NOW] Gateway receiver ready");
  Serial.print("[ESP-NOW] Gateway MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.printf(
    "[ESP-NOW] Initial channel: %u\n",
    readCurrentWiFiChannel()
  );

  return true;
}

void processPendingFanNodeMac() {
  if (!fanNodeMacPending || !espNowReady) {
    return;
  }

  uint8_t macCopy[6];

  portENTER_CRITICAL(&telemetryMux);
  memcpy(macCopy, pendingFanNodeMac, sizeof(macCopy));
  fanNodeMacPending = false;
  portEXIT_CRITICAL(&telemetryMux);

  const bool macChanged = !fanNodeKnown ||
    memcmp(fanNodeMac, macCopy, sizeof(fanNodeMac)) != 0;

  memcpy(fanNodeMac, macCopy, sizeof(fanNodeMac));
  fanNodeKnown = true;

  if (!esp_now_is_peer_exist(fanNodeMac)) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, fanNodeMac, sizeof(fanNodeMac));
    peerInfo.channel = 0;
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.encrypt = false;

    const esp_err_t result = esp_now_add_peer(&peerInfo);
    fanNodePeerReady = result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;

    if (!fanNodePeerReady) {
      Serial.printf("[ESP-NOW] Peer add error: %d\n", result);
    }
  } else {
    fanNodePeerReady = true;
  }

  if (macChanged) {
    Serial.print("[ESP-NOW] Fan node detected: ");
    printMacAddress(fanNodeMac);
    Serial.println();
  }

  acknowledgementRequired = fanNodePeerReady;
}

bool sendAcknowledgement() {
  if (!fanNodeKnown || !fanNodePeerReady) {
    return false;
  }

  AcknowledgementPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = PACKET_VERSION;
  packet.type = static_cast<uint8_t>(PacketType::Acknowledgement);
  packet.channel = readCurrentWiFiChannel();

  const esp_err_t result = esp_now_send(
    fanNodeMac,
    reinterpret_cast<const uint8_t*>(&packet),
    sizeof(packet)
  );

  if (result == ESP_OK) {
    Serial.printf(
      "[ESP-NOW TX] Link ACK | Channel: %u\n",
      packet.channel
    );
    return true;
  }

  Serial.printf("[ESP-NOW TX] ACK queue error: %d\n", result);
  return false;
}

void processNewTelemetry() {
  if (!newTelemetryAvailable) {
    return;
  }

  TelemetryPacket telemetryCopy;
  int8_t rssiCopy;

  portENTER_CRITICAL(&telemetryMux);
  telemetryCopy = telemetryBuffer;
  rssiCopy = pendingRssi;
  newTelemetryAvailable = false;
  portEXIT_CRITICAL(&telemetryMux);

  latestTelemetry = telemetryCopy;
  latestRssi = rssiCopy;
  telemetryAvailable = true;
  lastTelemetryReceivedMs = millis();

  Serial.printf(
    "[ESP-NOW RX] Seq: %lu | Temp: %.1f C | Hum: %.1f %% | Dist: %d cm | Fan: %s | Mode: %s | RSSI: %d dBm\n",
    static_cast<unsigned long>(latestTelemetry.sequence),
    latestTelemetry.temperatureC,
    latestTelemetry.humidity,
    latestTelemetry.distanceCm,
    latestTelemetry.fanOn ? "ON" : "OFF",
    latestTelemetry.manualMode ? "MANUAL" : "AUTO",
    latestRssi
  );
}

// -----------------------------------------------------------------------------
// LEDs
// -----------------------------------------------------------------------------
void updateStatusLeds() {
  const bool stale = telemetryIsStale();
  const bool alert = systemAlertActive();

  digitalWrite(GREEN_LED_PIN, telemetryAvailable && !stale && !alert);
  digitalWrite(RED_LED_PIN, alert);
  digitalWrite(
    BLUE_LED_PIN,
    telemetryAvailable && latestTelemetry.manualMode
  );
}

// -----------------------------------------------------------------------------
// Wi-Fi
// -----------------------------------------------------------------------------
void startWiFiConnection() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();

  Serial.print("[Wi-Fi] Connecting to ");
  Serial.println(WIFI_SSID);
}

void maintainWiFi() {
  const bool connected = WiFi.status() == WL_CONNECTED;

  if (connected) {
    const uint8_t currentChannel = readCurrentWiFiChannel();

    if (!previousWiFiConnected) {
      Serial.print("[Wi-Fi] Connected. IP: ");
      Serial.println(WiFi.localIP());
      startLabViewUdp();
    }

    if (currentChannel != lastReportedChannel) {
      lastReportedChannel = currentChannel;
      Serial.printf("[Wi-Fi] Active channel: %u\n", currentChannel);

      if (fanNodeKnown) {
        acknowledgementRequired = true;
      }
    }

    previousWiFiConnected = true;
    return;
  }

  if (previousWiFiConnected) {
    Serial.println("[Wi-Fi] Connection lost");
    previousWiFiConnected = false;
    stopLabViewUdp();
  }

  if (millis() - lastWiFiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    Serial.println("[Wi-Fi] Reconnecting...");
    WiFi.disconnect(false, false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWiFiAttemptMs = millis();
  }
}

// -----------------------------------------------------------------------------
// LabVIEW UDP integration
// -----------------------------------------------------------------------------
void stopLabViewUdp() {
  labViewTelemetryUdp.stop();
  labViewCommandUdp.stop();
  udpReady = false;
}

void startLabViewUdp() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!labViewIpAddress.fromString(LABVIEW_IP)) {
    Serial.println("[LabVIEW] Invalid LABVIEW_IP");
    udpReady = false;
    return;
  }

  stopLabViewUdp();

  const bool telemetrySocketReady =
    labViewTelemetryUdp.begin(LABVIEW_TELEMETRY_PORT) == 1;
  const bool commandSocketReady =
    labViewCommandUdp.begin(LABVIEW_COMMAND_PORT) == 1;

  udpReady = telemetrySocketReady && commandSocketReady;

  if (udpReady) {
    Serial.printf(
      "[LabVIEW] UDP ready | TX %s:%u | RX 0.0.0.0:%u\n",
      LABVIEW_IP,
      LABVIEW_TELEMETRY_PORT,
      LABVIEW_COMMAND_PORT
    );
  } else {
    Serial.println("[LabVIEW] Could not start UDP sockets");
  }
}

void sendTelemetryToLabView() {
  if (
    !udpReady ||
    WiFi.status() != WL_CONNECTED ||
    !telemetryAvailable ||
    telemetryIsStale()
  ) {
    return;
  }

  // Original LabVIEW-compatible format:
  // temperatureC;temperatureF;humidity;fanState
  char message[128];
  snprintf(
    message,
    sizeof(message),
    "%.1f;%.1f;%.1f;%u",
    latestTelemetry.temperatureC,
    latestTelemetry.temperatureF,
    latestTelemetry.humidity,
    latestTelemetry.fanOn ? 1U : 0U
  );

  if (!labViewTelemetryUdp.beginPacket(
        labViewIpAddress,
        LABVIEW_TELEMETRY_PORT
      )) {
    Serial.println("[LabVIEW] UDP beginPacket failed");
    return;
  }

  labViewTelemetryUdp.write(
    reinterpret_cast<const uint8_t*>(message),
    strlen(message)
  );

  const int result = labViewTelemetryUdp.endPacket();

  if (result == 1) {
    Serial.print("[LabVIEW TX] ");
    Serial.println(message);
  } else {
    Serial.println("[LabVIEW] UDP telemetry send failed");
  }
}

RemoteCommand parseLabViewCommand(String commandText) {
  commandText.trim();
  commandText.toUpperCase();

  if (commandText == "MANUAL") {
    return RemoteCommand::SetManual;
  }

  if (commandText == "AUTO") {
    return RemoteCommand::SetAuto;
  }

  if (commandText == "FAN:ON" || commandText == "FAN_ON") {
    return RemoteCommand::FanOn;
  }

  if (commandText == "FAN:OFF" || commandText == "FAN_OFF") {
    return RemoteCommand::FanOff;
  }

  return RemoteCommand::None;
}

uint32_t nextEspNowCommandId() {
  espNowCommandSequence++;

  if (espNowCommandSequence == 0) {
    espNowCommandSequence = 1;
  }

  return espNowCommandSequence;
}

// -----------------------------------------------------------------------------
// Flask
// -----------------------------------------------------------------------------
void postTelemetryToFlask() {
  if (
    WiFi.status() != WL_CONNECTED ||
    !telemetryAvailable ||
    telemetryIsStale()
  ) {
    return;
  }

  WiFiClient client;
  HTTPClient http;
  const String url = String(FLASK_BASE_URL) + "/api/datos";

  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    Serial.println("[Flask] Could not open POST connection");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  String json;
  json.reserve(320);
  json += "{";
  json += "\"sequence\":" + String(latestTelemetry.sequence) + ",";
  json += "\"uptime_ms\":" + String(latestTelemetry.uptimeMs) + ",";
  json += "\"temperature_c\":" + String(latestTelemetry.temperatureC, 1) + ",";
  json += "\"temperature_f\":" + String(latestTelemetry.temperatureF, 1) + ",";
  json += "\"humidity\":" + String(latestTelemetry.humidity, 1) + ",";
  json += "\"distance_cm\":" + String(latestTelemetry.distanceCm) + ",";
  json += "\"fan_on\":" + String(latestTelemetry.fanOn ? "true" : "false") + ",";
  json += "\"manual_mode\":" + String(latestTelemetry.manualMode ? "true" : "false") + ",";
  json += "\"sensors_valid\":" + String(latestTelemetry.sensorsValid ? "true" : "false") + ",";
  json += "\"alert_active\":" + String(systemAlertActive() ? "true" : "false") + ",";
  json += "\"rssi\":" + String(latestRssi);
  json += "}";

  const int statusCode = http.POST(json);

  if (statusCode > 0) {
    Serial.printf("[Flask] POST /api/datos -> %d\n", statusCode);
  } else {
    Serial.printf(
      "[Flask] POST error: %s\n",
      http.errorToString(statusCode).c_str()
    );
  }

  http.end();
}

long extractJsonInteger(const String& json, const char* key) {
  const int keyPosition = json.indexOf(key);
  if (keyPosition < 0) {
    return -1;
  }

  const int colonPosition = json.indexOf(':', keyPosition);
  if (colonPosition < 0) {
    return -1;
  }

  int valueStart = colonPosition + 1;
  while (
    valueStart < static_cast<int>(json.length()) &&
    (json[valueStart] == ' ' || json[valueStart] == '\t')
  ) {
    valueStart++;
  }

  return json.substring(valueStart).toInt();
}

RemoteCommand parseCommand(const String& response) {
  String normalized = response;
  normalized.toUpperCase();

  if (normalized.indexOf("FAN_ON") >= 0) {
    return RemoteCommand::FanOn;
  }
  if (normalized.indexOf("FAN_OFF") >= 0) {
    return RemoteCommand::FanOff;
  }
  if (normalized.indexOf("MANUAL") >= 0) {
    return RemoteCommand::SetManual;
  }
  if (normalized.indexOf("AUTO") >= 0) {
    return RemoteCommand::SetAuto;
  }

  return RemoteCommand::None;
}

bool sendCommandToFan(RemoteCommand command, const char* source) {
  if (!espNowReady || !fanNodeKnown || !fanNodePeerReady) {
    Serial.printf(
      "[ESP-NOW] %s command not sent: fan node is not linked\n",
      source
    );
    return false;
  }

  CommandPacket packet = {};
  packet.magic = PACKET_MAGIC;
  packet.version = PACKET_VERSION;
  packet.type = static_cast<uint8_t>(PacketType::Command);
  packet.command = static_cast<uint8_t>(command);
  packet.commandId = nextEspNowCommandId();

  const esp_err_t result = esp_now_send(
    fanNodeMac,
    reinterpret_cast<const uint8_t*>(&packet),
    sizeof(packet)
  );

  if (result == ESP_OK) {
    Serial.printf(
      "[ESP-NOW TX] Source: %s | Command: %u | Gateway ID: %lu\n",
      source,
      static_cast<unsigned int>(packet.command),
      static_cast<unsigned long>(packet.commandId)
    );
    return true;
  }

  Serial.printf(
    "[ESP-NOW TX] %s command queue error: %d\n",
    source,
    result
  );
  return false;
}

void checkLabViewCommands() {
  if (!udpReady || WiFi.status() != WL_CONNECTED) {
    return;
  }

  const int packetSize = labViewCommandUdp.parsePacket();
  if (packetSize <= 0) {
    return;
  }

  char buffer[64] = {};
  const int bytesRead = labViewCommandUdp.read(
    buffer,
    sizeof(buffer) - 1
  );

  if (bytesRead <= 0) {
    return;
  }

  buffer[bytesRead] = '\0';
  String commandText(buffer);
  commandText.trim();

  Serial.print("[LabVIEW RX] ");
  Serial.println(commandText);

  const RemoteCommand command = parseLabViewCommand(commandText);

  if (command == RemoteCommand::None) {
    Serial.println("[LabVIEW] Unsupported command");
    return;
  }

  if (!sendCommandToFan(command, "LabVIEW")) {
    pendingLabViewCommand = command;
    pendingLabViewCommandAvailable = true;
    Serial.println("[LabVIEW] Command queued until ESP-NOW link is ready");
  }
}

void processPendingLabViewCommand() {
  if (!pendingLabViewCommandAvailable) {
    return;
  }

  if (sendCommandToFan(pendingLabViewCommand, "LabVIEW retry")) {
    pendingLabViewCommand = RemoteCommand::None;
    pendingLabViewCommandAvailable = false;
  }
}

void pollCommandFromFlask() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  WiFiClient client;
  HTTPClient http;
  const String url = String(FLASK_BASE_URL) + "/api/comando";

  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    Serial.println("[Flask] Could not open GET connection");
    return;
  }

  const int statusCode = http.GET();

  if (statusCode == HTTP_CODE_OK) {
    const String response = http.getString();
    const RemoteCommand command = parseCommand(response);
    const long parsedCommandId = extractJsonInteger(
      response,
      "\"command_id\""
    );

    if (
      command != RemoteCommand::None &&
      parsedCommandId > 0 &&
      static_cast<uint32_t>(parsedCommandId) > lastFlaskCommandId
    ) {
      if (sendCommandToFan(command, "Flask")) {
        lastFlaskCommandId = static_cast<uint32_t>(parsedCommandId);
      }
    }
  } else if (statusCode > 0) {
    Serial.printf("[Flask] GET /api/comando -> %d\n", statusCode);
  } else {
    Serial.printf(
      "[Flask] GET error: %s\n",
      http.errorToString(statusCode).c_str()
    );
  }

  http.end();
}

// -----------------------------------------------------------------------------
// Arduino
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  espNowCommandSequence = esp_random();
  if (espNowCommandSequence == 0) {
    espNowCommandSequence = 1;
  }

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);

  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(BLUE_LED_PIN, LOW);

  if (!initializeEspNow()) {
    Serial.println("[SYSTEM] ESP-NOW could not start");
  }

  startWiFiConnection();
  Serial.println("[SYSTEM] Gateway ready");
}

void loop() {
  const unsigned long now = millis();

  maintainWiFi();
  processPendingFanNodeMac();

  if (acknowledgementRequired) {
    if (sendAcknowledgement()) {
      acknowledgementRequired = false;
    }
  }

  processNewTelemetry();
  updateStatusLeds();
  processPendingLabViewCommand();
  checkLabViewCommands();

  if (
    now - lastLabViewTelemetryMs >= LABVIEW_TELEMETRY_INTERVAL_MS
  ) {
    lastLabViewTelemetryMs = now;
    sendTelemetryToLabView();
  }

  if (now - lastDataPostMs >= DATA_POST_INTERVAL_MS) {
    lastDataPostMs = now;
    postTelemetryToFlask();
  }

  if (now - lastCommandPollMs >= COMMAND_POLL_INTERVAL_MS) {
    lastCommandPollMs = now;
    pollCommandFromFlask();
  }

  delay(10);
}
