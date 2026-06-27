#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <WiFiUdp.h>
#define MQTT_MAX_PACKET_SIZE 768
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// =====================================================
// STUDENT ZONE
// =====================================================

// This is a friendly display name only.
// chip_id is generated from ESP.getChipId() and must not be hardcoded.
#define TEAM_NAME "Team01"

// =====================================================
// WIFI CONFIG
// =====================================================

const char* WIFI_SSIDS[] = {
  "IOTCamp01",
  "IOTCamp02",
  "IOTCamp03"
};

const char* WIFI_PASSWORDS[] = {
  "11112222",
  "11112222",
  "11112222"
};

const int WIFI_NETWORK_COUNT = sizeof(WIFI_SSIDS) / sizeof(WIFI_SSIDS[0]);
const int WIFI_RETRY_PER_NETWORK = 20;
const int FORCE_WIFI_INDEX = -1;

// =====================================================
// ADMIN SYSTEM CONFIG
// =====================================================

const char* ADMIN_SERVER = "auto";
const int DISCOVERY_PORT = 42100;
const char* DISCOVERY_MESSAGES[] = {
  "IOT_ARENA_DISCOVER",
  "HEAT_CHALLENGE_DISCOVER"
};
const char* DISCOVERY_RESPONSE_PREFIXES[] = {
  "IOT_ARENA_ADMIN ",
  "HEAT_CHALLENGE_ADMIN "
};
const int DISCOVERY_MESSAGE_COUNT = sizeof(DISCOVERY_MESSAGES) / sizeof(DISCOVERY_MESSAGES[0]);
const unsigned long DISCOVERY_TIMEOUT_MS = 2500;

const char* DEVICE_API_KEY = "8531be1897bc4a5c6f779373f64ed5aaa15cfc0f0735a077";

// =====================================================
// MQTT / HIVEMQ CLOUD CONFIG
// =====================================================

const bool USE_MQTT = true;
const bool FALLBACK_TO_HTTP_WHEN_MQTT_MISSING = true;

const char* MQTT_SERVER = "964e3b99339c4543bae0651a089d1b1e.s1.eu.hivemq.cloud";
const int   MQTT_PORT   = 8883;
const char* MQTT_USERNAME = "Tanawat";
const char* MQTT_PASSWORD = "Tanawat0910";
const char* MQTT_BASE_TOPIC = "iot-elemental-arena/classroom-01";

const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const int MQTT_KEEP_ALIVE_SECONDS = 30;
const int MQTT_SOCKET_TIMEOUT_SECONDS = 10;
const int HTTPS_RX_BUFFER_SIZE = 4096;
const int HTTPS_TX_BUFFER_SIZE = 512;
const int ADMIN_HTTP_TIMEOUT_MS = 15000;

const unsigned long TELEMETRY_INTERVAL = 2000;
const unsigned long HEARTBEAT_INTERVAL = 10000;
const unsigned long WIFI_CHECK_INTERVAL = 5000;
const unsigned long OLED_REFRESH_INTERVAL = 1000;

// =====================================================
// PIN & HARDWARE CONFIG
// =====================================================

#define OLED_SCL D1
#define OLED_SDA D2
#define DHTPIN D5
#define DHTTYPE DHT22

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiUDP discoveryUdp;
DHT dht(DHTPIN, DHTTYPE);
BearSSL::WiFiClientSecure mqttSecureClient;
PubSubClient mqttClient(mqttSecureClient);

bool oledReady = false;
bool wifiConnected = false;
bool discoveryUdpReady = false;
int activeWiFiIndex = -1;

float currentTemp = 0;
float currentHum = 0;
int lastAdminHttpCode = 0;
String lastAdminMessage = "Not sent yet";
String discoveredAdminServer = "";

unsigned long lastReadTime = 0;
unsigned long lastTelemetryTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastWiFiCheckTime = 0;
unsigned long lastOLEDTime = 0;
unsigned long lastMqttReconnectAttempt = 0;

String getChipId() {
  return String(ESP.getChipId(), HEX);
}

bool isPlaceholder(String value) {
  value.trim();
  return value.length() == 0 || value.startsWith("PUT_");
}

bool hasDeviceApiKey() {
  return !isPlaceholder(String(DEVICE_API_KEY));
}

int getPreferredWiFiIndex() {
  if (FORCE_WIFI_INDEX >= 0 && FORCE_WIFI_INDEX < WIFI_NETWORK_COUNT) {
    return FORCE_WIFI_INDEX;
  }
  return ESP.getChipId() % WIFI_NETWORK_COUNT;
}

const char* getActiveWiFiSSID() {
  if (activeWiFiIndex >= 0 && activeWiFiIndex < WIFI_NETWORK_COUNT) {
    return WIFI_SSIDS[activeWiFiIndex];
  }
  return WIFI_SSIDS[getPreferredWiFiIndex()];
}

String jsonEscape(String text) {
  text.replace("\\", "\\\\");
  text.replace("\"", "\\\"");
  text.replace("\n", "\\n");
  text.replace("\r", "\\r");
  return text;
}

void showOLED(String title, String line1, String line2 = "", String line3 = "") {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(title);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  display.setCursor(0, 18); display.println(line1);
  display.setCursor(0, 32); display.println(line2);
  display.setCursor(0, 46); display.println(line3);
  display.display();
}

void showSensorOLED() {
  String adminLine = "Send: ";
  if (lastAdminHttpCode >= 200 && lastAdminHttpCode < 300) {
    adminLine += "OK";
  } else if (WiFi.status() != WL_CONNECTED) {
    adminLine += "WiFi fail";
  } else {
    adminLine += lastAdminMessage;
  }

  showOLED(
    String(TEAM_NAME),
    "Temp: " + String(currentTemp, 1) + " C",
    "Hum: " + String(currentHum, 1) + " %",
    adminLine
  );
}

String normalizeAdminServer(String url) {
  url.trim();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  return url;
}

bool discoverAdminServer() {
  if (String(ADMIN_SERVER) != "auto") {
    discoveredAdminServer = normalizeAdminServer(String(ADMIN_SERVER));
    return discoveredAdminServer.length() > 0;
  }

  if (WiFi.status() != WL_CONNECTED) return false;

  if (!discoveryUdpReady) {
    discoveryUdpReady = discoveryUdp.begin(0);
  }
  if (!discoveryUdpReady) return false;

  Serial.println("Discovering Arena Admin...");
  for (int i = 0; i < DISCOVERY_MESSAGE_COUNT; i++) {
    discoveryUdp.beginPacket(IPAddress(255, 255, 255, 255), DISCOVERY_PORT);
    discoveryUdp.write((const uint8_t*)DISCOVERY_MESSAGES[i], strlen(DISCOVERY_MESSAGES[i]));
    discoveryUdp.endPacket();
    delay(20);
  }

  unsigned long startedAt = millis();
  char packetBuffer[180];

  while (millis() - startedAt < DISCOVERY_TIMEOUT_MS) {
    int packetSize = discoveryUdp.parsePacket();
    if (packetSize <= 0) {
      delay(25);
      continue;
    }

    int len = discoveryUdp.read(packetBuffer, sizeof(packetBuffer) - 1);
    if (len <= 0) continue;

    packetBuffer[len] = '\0';
    String response = String(packetBuffer);
    response.trim();

    for (int i = 0; i < DISCOVERY_MESSAGE_COUNT; i++) {
      const char* prefix = DISCOVERY_RESPONSE_PREFIXES[i];
      if (!response.startsWith(prefix)) continue;

      discoveredAdminServer = normalizeAdminServer(
        response.substring(strlen(prefix))
      );
      Serial.print("Arena Admin found: ");
      Serial.println(discoveredAdminServer);
      return true;
    }
  }

  Serial.println("Arena Admin discovery timed out.");
  return false;
}

String getAdminServerUrl(bool forceRediscover = false) {
  if (forceRediscover) discoveredAdminServer = "";
  if (discoveredAdminServer.length() == 0) discoverAdminServer();
  if (discoveredAdminServer.length() > 0) return discoveredAdminServer;
  return normalizeAdminServer(String(ADMIN_SERVER));
}

bool isHttpsUrl(String url) {
  return url.startsWith("https://");
}

String getUrlHost(String url) {
  int start = url.indexOf("://");
  start = start >= 0 ? start + 3 : 0;
  int end = url.indexOf("/", start);
  if (end < 0) end = url.length();
  String hostPort = url.substring(start, end);
  int colon = hostPort.indexOf(":");
  return colon >= 0 ? hostPort.substring(0, colon) : hostPort;
}

String getUrlPath(String url) {
  int start = url.indexOf("://");
  start = start >= 0 ? start + 3 : 0;
  int pathStart = url.indexOf("/", start);
  return pathStart >= 0 ? url.substring(pathStart) : "/";
}

int getUrlPort(String url) {
  int start = url.indexOf("://");
  start = start >= 0 ? start + 3 : 0;
  int end = url.indexOf("/", start);
  if (end < 0) end = url.length();
  String hostPort = url.substring(start, end);
  int colon = hostPort.indexOf(":");
  if (colon >= 0) return hostPort.substring(colon + 1).toInt();
  return isHttpsUrl(url) ? 443 : 80;
}

bool beginAdminHttp(HTTPClient& http, WiFiClient& client, BearSSL::WiFiClientSecure& secureClient, String url) {
  String host = getUrlHost(url);
  int port = getUrlPort(url);
  String path = getUrlPath(url);

  if (isHttpsUrl(url)) {
    secureClient.setBufferSizes(HTTPS_RX_BUFFER_SIZE, HTTPS_TX_BUFFER_SIZE);
    secureClient.setTimeout(ADMIN_HTTP_TIMEOUT_MS / 1000);
    secureClient.setSSLVersion(BR_TLS12, BR_TLS12);
    secureClient.setInsecure();
    return http.begin(secureClient, host, port, path, true);
  }

  return http.begin(client, host, port, path);
}

String basePayload() {
  String body = "{";
  body += "\"chip_id\":\"" + getChipId() + "\",";
  if (hasDeviceApiKey()) {
    body += "\"device_key\":\"" + jsonEscape(String(DEVICE_API_KEY)) + "\",";
  }
  body += "\"team_name\":\"" + jsonEscape(String(TEAM_NAME)) + "\",";
  body += "\"kit_type\":\"battle_arena\",";
  body += "\"wifi_ssid\":\"" + jsonEscape(String(getActiveWiFiSSID())) + "\",";
  body += "\"ip_address\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"rssi\":" + String(WiFi.RSSI());
  return body;
}

String buildRegistrationPayload() {
  return basePayload() + "}";
}

String buildTelemetryPayload() {
  String body = basePayload() + ",";
  body += "\"temp\":" + String(currentTemp, 1) + ",";
  body += "\"humidity\":" + String(currentHum, 1);
  body += "}";
  return body;
}

String normalizeMqttBaseTopic() {
  String baseTopic = String(MQTT_BASE_TOPIC);
  baseTopic.trim();
  while (baseTopic.startsWith("/")) baseTopic.remove(0, 1);
  while (baseTopic.endsWith("/")) baseTopic.remove(baseTopic.length() - 1);
  return baseTopic;
}

String buildMqttTopic(const char* messageType) {
  return normalizeMqttBaseTopic() + "/" + getChipId() + "/" + String(messageType);
}

bool isMqttConfigured() {
  return String(MQTT_SERVER).length() > 0
      && String(MQTT_USERNAME).length() > 0
      && String(MQTT_PASSWORD).length() > 0
      && String(MQTT_SERVER) != "PUT_HIVEMQ_HOST_HERE"
      && String(MQTT_USERNAME) != "PUT_HIVEMQ_USERNAME_HERE"
      && String(MQTT_PASSWORD) != "PUT_HIVEMQ_PASSWORD_HERE";
}

void setupMqttClient() {
  mqttSecureClient.setBufferSizes(HTTPS_RX_BUFFER_SIZE, HTTPS_TX_BUFFER_SIZE);
  mqttSecureClient.setTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
  mqttSecureClient.setSSLVersion(BR_TLS12, BR_TLS12);
  mqttSecureClient.setInsecure();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setKeepAlive(MQTT_KEEP_ALIVE_SECONDS);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
  mqttClient.setBufferSize(MQTT_MAX_PACKET_SIZE);
}

bool ensureMqttConnected(bool forceAttempt = false) {
  if (!USE_MQTT || !isMqttConfigured()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (mqttClient.connected()) return true;

  unsigned long now = millis();
  if (!forceAttempt && now - lastMqttReconnectAttempt < MQTT_RECONNECT_INTERVAL) {
    return false;
  }
  lastMqttReconnectAttempt = now;

  String clientId = "iot-arena-esp-" + getChipId();
  bool connected = mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD);
  lastAdminMessage = connected ? "MQTT connected" : "MQTT fail";
  return connected;
}

void maintainMqttConnection() {
  if (!USE_MQTT || WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) {
    mqttClient.loop();
  } else {
    ensureMqttConnected();
  }
}

bool publishToMqtt(const char* messageType, String body) {
  if (!ensureMqttConnected(true)) {
    Serial.println("MQTT publish skipped: not connected or MQTT config missing");
    return false;
  }

  String topic = buildMqttTopic(messageType);
  Serial.print("MQTT PUB ");
  Serial.println(topic);
  bool ok = mqttClient.publish(topic.c_str(), body.c_str(), false);
  mqttClient.loop();
  lastAdminHttpCode = ok ? 200 : 0;
  lastAdminMessage = ok ? "MQTT OK" : "MQTT publish fail";
  return ok;
}

bool postToAdmin(String endpoint, String body) {
  if (WiFi.status() != WL_CONNECTED) {
    lastAdminHttpCode = 0;
    lastAdminMessage = "WiFi not connected";
    return false;
  }

  String adminServerUrl = getAdminServerUrl();
  if (adminServerUrl.length() == 0 || adminServerUrl == "auto") {
    lastAdminHttpCode = 0;
    lastAdminMessage = "Admin discovery failed";
    return false;
  }

  WiFiClient client;
  BearSSL::WiFiClientSecure secureClient;
  HTTPClient http;
  String url = adminServerUrl + endpoint;
  Serial.print("HTTP POST ");
  Serial.println(url);

  if (!beginAdminHttp(http, client, secureClient, url)) {
    lastAdminHttpCode = 0;
    lastAdminMessage = "HTTP begin failed";
    getAdminServerUrl(true);
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("ngrok-skip-browser-warning", "true");
  if (hasDeviceApiKey()) {
    http.addHeader("X-Device-Key", DEVICE_API_KEY);
  }
  http.setTimeout(ADMIN_HTTP_TIMEOUT_MS);

  int httpCode = http.POST(body);
  String response = http.getString();
  http.end();
  lastAdminHttpCode = httpCode;

  bool ok = httpCode >= 200 && httpCode < 300;
  lastAdminMessage = ok ? "HTTP OK" : ("HTTP fail " + String(httpCode));
  Serial.print("HTTP ");
  Serial.print(httpCode);
  Serial.print(" ");
  Serial.println(response);
  if (!ok && httpCode < 0) getAdminServerUrl(true);
  return ok;
}

bool sendRegister() {
  String body = buildRegistrationPayload();
  if (USE_MQTT && isMqttConfigured() && publishToMqtt("register", body)) return true;
  if (!USE_MQTT || FALLBACK_TO_HTTP_WHEN_MQTT_MISSING) return postToAdmin("/api/device/register", body);
  return false;
}

bool sendTelemetry() {
  String body = buildTelemetryPayload();
  if (USE_MQTT && isMqttConfigured() && publishToMqtt("telemetry", body)) return true;
  if (!USE_MQTT || FALLBACK_TO_HTTP_WHEN_MQTT_MISSING) return postToAdmin("/api/device/update", body);
  return false;
}

bool connectWiFiByIndex(int networkIndex) {
  Serial.print("Trying SSID: ");
  Serial.println(WIFI_SSIDS[networkIndex]);
  showOLED("WiFi", "Connecting...", String(WIFI_SSIDS[networkIndex]));

  WiFi.disconnect();
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSIDS[networkIndex], WIFI_PASSWORDS[networkIndex]);

  for (int retry = 0; retry < WIFI_RETRY_PER_NETWORK; retry++) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) return false;

  activeWiFiIndex = networkIndex;
  wifiConnected = true;
  showOLED("WiFi Connected", WiFi.localIP().toString(), "RSSI: " + String(WiFi.RSSI()), String(getActiveWiFiSSID()));
  delay(1500);
  return true;
}

void connectWiFi() {
  int preferred = getPreferredWiFiIndex();
  activeWiFiIndex = -1;
  wifiConnected = false;

  for (int attempt = 0; attempt < WIFI_NETWORK_COUNT; attempt++) {
    int idx = (preferred + attempt) % WIFI_NETWORK_COUNT;
    if (connectWiFiByIndex(idx)) return;
  }

  showOLED("WiFi Failed", "All SSIDs failed", "Check config");
  delay(1500);
}

void checkWiFiStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    return;
  }

  wifiConnected = false;
  showOLED("!! DISCONNECTED !!", "WiFi lost!", "Reconnecting...", String(getActiveWiFiSSID()));
  delay(1000);
  connectWiFi();
}

bool readSensor() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("DHT read failed.");
    showOLED("DHT Error", "Check wiring", "D5 / VCC / GND");
    return false;
  }

  currentTemp = temp;
  currentHum = hum;
  return true;
}

void printSensorStatus() {
  Serial.print("Team: "); Serial.print(TEAM_NAME);
  Serial.print(" | ID: "); Serial.print(getChipId());
  Serial.print(" | Temp: "); Serial.print(currentTemp, 1); Serial.print(" C");
  Serial.print(" | Hum: "); Serial.print(currentHum, 1); Serial.print(" %");
  Serial.print(" | RSSI: "); Serial.print(WiFi.RSSI());
  Serial.print(" | Send: "); Serial.println(lastAdminMessage);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=== IoT Elemental Battle Arena ESP8266 ===");
  Serial.print("Team: "); Serial.println(TEAM_NAME);
  Serial.print("Chip ID: "); Serial.println(getChipId());
  Serial.print("Mode: "); Serial.println(USE_MQTT ? "MQTT preferred" : "HTTP");

  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    oledReady = true;
    showOLED("Battle Arena", String(TEAM_NAME), "Chip: " + getChipId(), "Starting...");
  }

  dht.begin();
  delay(1000);

  connectWiFi();
  if (USE_MQTT) setupMqttClient();

  if (WiFi.status() == WL_CONNECTED) {
    sendRegister();
    lastHeartbeatTime = millis();

    if (readSensor()) {
      sendTelemetry();
      lastTelemetryTime = millis();
      showSensorOLED();
    }
  }
}

void loop() {
  unsigned long now = millis();

  if (now - lastWiFiCheckTime >= WIFI_CHECK_INTERVAL) {
    lastWiFiCheckTime = now;
    checkWiFiStatus();
  }

  maintainMqttConnection();

  if (WiFi.status() == WL_CONNECTED && now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    lastHeartbeatTime = now;
    sendRegister();
  }

  if (now - lastReadTime >= TELEMETRY_INTERVAL) {
    lastReadTime = now;
    if (!readSensor()) return;
    printSensorStatus();
  }

  if (WiFi.status() == WL_CONNECTED && now - lastTelemetryTime >= TELEMETRY_INTERVAL) {
    lastTelemetryTime = now;
    sendTelemetry();
  }

  if (now - lastOLEDTime >= OLED_REFRESH_INTERVAL) {
    lastOLEDTime = now;
    if (WiFi.status() != WL_CONNECTED) {
      showOLED("!! DISCONNECTED !!", "No WiFi", "Waiting...", String(getActiveWiFiSSID()));
    } else {
      showSensorOLED();
    }
  }
}
