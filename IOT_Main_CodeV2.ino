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
// STUDENT ZONE — แก้แค่ชื่อทีม
// =====================================================

#define TEAM_NAME "Admin Team"

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
const char* DISCOVERY_MESSAGE = "HEAT_CHALLENGE_DISCOVER";
const char* DISCOVERY_RESPONSE_PREFIX = "HEAT_CHALLENGE_ADMIN ";
const unsigned long DISCOVERY_TIMEOUT_MS = 2500;

// ต้องตรงกับ security.deviceApiKey ใน config.json ของ Admin System
const char* DEVICE_API_KEY = "8531be1897bc4a5c6f779373f64ed5aaa15cfc0f0735a077";

// =====================================================
// MQTT CLOUD BRIDGE CONFIG
// =====================================================

// true  = ส่งข้อมูลผ่าน HiveMQ Cloud
// false = ส่งข้อมูลเข้า Admin System ด้วย HTTP/LAN/tunnel แบบเดิม
const bool USE_MQTT = true;

const char* MQTT_SERVER = "964e3b99339c4543bae0651a089d1b1e.s1.eu.hivemq.cloud";
const int   MQTT_PORT   = 8883;

// ใส่ MQTT credential ที่สร้างใน HiveMQ Cloud > Access Management
const char* MQTT_USERNAME = "Tanawat";
const char* MQTT_PASSWORD = "Tanawat0910";

const char* MQTT_BASE_TOPIC = "heat-challenge-iot/classroom-01";
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const int MQTT_KEEP_ALIVE_SECONDS = 30;
const int MQTT_SOCKET_TIMEOUT_SECONDS = 10;

const int HTTPS_RX_BUFFER_SIZE = 4096;
const int HTTPS_TX_BUFFER_SIZE = 512;
const int ADMIN_HTTP_TIMEOUT_MS = 15000;

// Admin System กำหนด deviceOfflineSeconds = 30 วินาที
// HEARTBEAT_INTERVAL ต้องน้อยกว่า 30 วินาที เพื่อให้บอร์ดไม่ถูกมาร์คว่า offline
const unsigned long ADMIN_POST_INTERVAL = 2000;
const unsigned long HEARTBEAT_INTERVAL = 10000;

// =====================================================
// PIN & HARDWARE CONFIG
// =====================================================

#define OLED_SCL D1
#define OLED_SDA D2
#define DHTPIN   D5
#define DHTTYPE  DHT22

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiUDP discoveryUdp;
DHT dht(DHTPIN, DHTTYPE);
BearSSL::WiFiClientSecure mqttSecureClient;
PubSubClient mqttClient(mqttSecureClient);

// =====================================================
// SYSTEM STATE
// =====================================================

bool oledReady         = false;
bool wifiConnected     = false;
bool discoveryUdpReady = false;
int  activeWiFiIndex   = -1;

float baseTemp   = 0;
float maxTemp    = -100;
bool  baseSet    = false;

float currentTemp  = 0;
float currentHum   = 0;
float currentScore = 0;
float bestScore    = 0;

int    lastAdminHttpCode = 0;
String lastAdminMessage  = "Not sent yet";
String discoveredAdminServer = "";

unsigned long lastReadTime      = 0;
unsigned long lastAdminPostTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastWiFiCheckTime = 0;
unsigned long lastOLEDTime      = 0;
unsigned long lastMqttReconnectAttempt = 0;

const unsigned long READ_INTERVAL         = 2000;
const unsigned long WIFI_CHECK_INTERVAL   = 5000;
const unsigned long OLED_REFRESH_INTERVAL = 1000;

// =====================================================
// UTILITIES
// =====================================================

String getChipId() {
  return String(ESP.getChipId(), HEX);
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

// =====================================================
// OLED
// =====================================================

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
  String adminLine = "Admin: ";
  if (lastAdminHttpCode >= 200 && lastAdminHttpCode < 300) {
    adminLine += "OK";
  } else if (WiFi.status() != WL_CONNECTED) {
    adminLine += "WiFi FAIL";
  } else {
    adminLine += "FAIL";
  }

  showOLED(
    String(TEAM_NAME),
    "Temp: " + String(currentTemp, 1) + " C",
    "Best: +" + String(bestScore, 1) + " C",
    adminLine
  );
}

// =====================================================
// LAN DISCOVERY
// =====================================================

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

  Serial.println("Discovering Admin System...");

  discoveryUdp.beginPacket(IPAddress(255, 255, 255, 255), DISCOVERY_PORT);
  discoveryUdp.write((const uint8_t*)DISCOVERY_MESSAGE, strlen(DISCOVERY_MESSAGE));
  discoveryUdp.endPacket();

  unsigned long startedAt = millis();
  char packetBuffer[160];

  while (millis() - startedAt < DISCOVERY_TIMEOUT_MS) {
    int packetSize = discoveryUdp.parsePacket();
    if (packetSize <= 0) { delay(25); continue; }

    int len = discoveryUdp.read(packetBuffer, sizeof(packetBuffer) - 1);
    if (len <= 0) continue;

    packetBuffer[len] = '\0';
    String response = String(packetBuffer);
    response.trim();

    if (!response.startsWith(DISCOVERY_RESPONSE_PREFIX)) continue;

    discoveredAdminServer = normalizeAdminServer(
      response.substring(strlen(DISCOVERY_RESPONSE_PREFIX))
    );

    Serial.print("Admin found: ");
    Serial.println(discoveredAdminServer);
    return true;
  }

  Serial.println("Admin discovery timed out.");
  return false;
}

String getAdminServerUrl(bool forceRediscover = false) {
  if (forceRediscover) discoveredAdminServer = "";
  if (discoveredAdminServer.length() == 0) discoverAdminServer();
  if (discoveredAdminServer.length() > 0) return discoveredAdminServer;
  return normalizeAdminServer(String(ADMIN_SERVER));
}

// =====================================================
// HTTP HELPERS
// =====================================================

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

bool beginAdminHttp(HTTPClient& http, WiFiClient& client,
                    BearSSL::WiFiClientSecure& secureClient, String url) {
  String host = getUrlHost(url);
  int port    = getUrlPort(url);
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

// =====================================================
// ADMIN PAYLOADS
// =====================================================

String buildAdminPayload() {
  String body = "{";
  body += "\"chip_id\":\"" + getChipId() + "\",";
  body += "\"device_key\":\"" + jsonEscape(String(DEVICE_API_KEY)) + "\",";
  body += "\"team_name\":\"" + jsonEscape(String(TEAM_NAME)) + "\",";
  body += "\"temp\":" + String(currentTemp, 1) + ",";
  body += "\"humidity\":" + String(currentHum, 1);
  body += "}";
  return body;
}

String buildRegistrationPayload() {
  String body = "{";
  body += "\"chip_id\":\"" + getChipId() + "\",";
  body += "\"device_key\":\"" + jsonEscape(String(DEVICE_API_KEY)) + "\",";
  body += "\"team_name\":\"" + jsonEscape(String(TEAM_NAME)) + "\",";
  body += "\"wifi_ssid\":\"" + jsonEscape(String(getActiveWiFiSSID())) + "\",";
  body += "\"ip_address\":\"" + WiFi.localIP().toString() + "\",";
  body += "\"rssi\":" + String(WiFi.RSSI());
  body += "}";
  return body;
}

// =====================================================
// MQTT HELPERS
// =====================================================

String normalizeMqttBaseTopic() {
  String baseTopic = String(MQTT_BASE_TOPIC);
  baseTopic.trim();

  while (baseTopic.startsWith("/")) {
    baseTopic.remove(0, 1);
  }

  while (baseTopic.endsWith("/")) {
    baseTopic.remove(baseTopic.length() - 1);
  }

  return baseTopic;
}

String buildMqttTopic(const char* messageType) {
  return normalizeMqttBaseTopic() + "/" + getChipId() + "/" + String(messageType);
}

bool isMqttConfigured() {
  return String(MQTT_SERVER).length() > 0
      && String(MQTT_USERNAME).length() > 0
      && String(MQTT_PASSWORD).length() > 0
      && String(MQTT_USERNAME) != "PUT_HIVEMQ_USERNAME_HERE"
      && String(MQTT_PASSWORD) != "PUT_HIVEMQ_PASSWORD_HERE";
}

void setupMqttClient() {
  mqttSecureClient.setBufferSizes(HTTPS_RX_BUFFER_SIZE, HTTPS_TX_BUFFER_SIZE);
  mqttSecureClient.setTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
  mqttSecureClient.setSSLVersion(BR_TLS12, BR_TLS12);

  // ใช้สำหรับ PoC กับ HiveMQ Cloud ให้ต่อ TLS ได้ง่ายบน ESP8266
  // ถ้านำไป production ค่อยเปลี่ยนเป็น CA certificate pinning
  mqttSecureClient.setInsecure();

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setKeepAlive(MQTT_KEEP_ALIVE_SECONDS);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
  mqttClient.setBufferSize(MQTT_MAX_PACKET_SIZE);
}

bool ensureMqttConnected(bool forceAttempt = false) {
  if (!USE_MQTT) return false;

  if (WiFi.status() != WL_CONNECTED) {
    lastAdminHttpCode = 0;
    lastAdminMessage  = "WiFi not connected";
    return false;
  }

  if (!isMqttConfigured()) {
    lastAdminHttpCode = 0;
    lastAdminMessage  = "MQTT config missing";
    return false;
  }

  if (mqttClient.connected()) {
    return true;
  }

  unsigned long now = millis();
  if (!forceAttempt && now - lastMqttReconnectAttempt < MQTT_RECONNECT_INTERVAL) {
    return false;
  }
  lastMqttReconnectAttempt = now;

  String clientId = "heat-challenge-esp-" + getChipId();

  Serial.print("MQTT connecting to ");
  Serial.print(MQTT_SERVER);
  Serial.print(":");
  Serial.print(MQTT_PORT);
  Serial.print(" as ");
  Serial.println(clientId);

  bool connected = mqttClient.connect(
    clientId.c_str(),
    MQTT_USERNAME,
    MQTT_PASSWORD
  );

  if (connected) {
    Serial.println("MQTT connected");
    lastAdminMessage = "MQTT connected";
    return true;
  }

  Serial.print("MQTT connect failed, state=");
  Serial.println(mqttClient.state());
  lastAdminHttpCode = 0;
  lastAdminMessage  = "MQTT fail: " + String(mqttClient.state());
  return false;
}

void maintainMqttConnection() {
  if (!USE_MQTT || WiFi.status() != WL_CONNECTED) return;

  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  ensureMqttConnected();
}

bool publishToMqtt(const char* messageType, String body) {
  if (!ensureMqttConnected(true)) {
    return false;
  }

  String topic = buildMqttTopic(messageType);

  Serial.print("MQTT PUB ");
  Serial.print(topic);
  Serial.print(" | heap: ");
  Serial.println(ESP.getFreeHeap());

  bool ok = mqttClient.publish(topic.c_str(), body.c_str(), false);
  mqttClient.loop();

  lastAdminHttpCode = ok ? 200 : 0;
  lastAdminMessage  = ok ? "MQTT OK" : "MQTT publish fail";

  if (!ok) {
    Serial.println("MQTT publish failed");
  }

  return ok;
}

// =====================================================
// SEND TO ADMIN SYSTEM
// =====================================================

bool postToAdmin(String endpoint, String body) {
  if (WiFi.status() != WL_CONNECTED) {
    lastAdminHttpCode = 0;
    lastAdminMessage  = "WiFi not connected";
    return false;
  }

  String adminServerUrl = getAdminServerUrl();
  if (adminServerUrl.length() == 0 || adminServerUrl == "auto") {
    lastAdminHttpCode = 0;
    lastAdminMessage  = "Admin discovery failed";
    return false;
  }

  WiFiClient client;
  BearSSL::WiFiClientSecure secureClient;
  HTTPClient http;

  String url = adminServerUrl + endpoint;

  Serial.print("POST ");
  Serial.print(url);
  Serial.print(" | heap: ");
  Serial.println(ESP.getFreeHeap());

  if (!beginAdminHttp(http, client, secureClient, url)) {
    lastAdminHttpCode = 0;
    lastAdminMessage  = "HTTP begin failed";
    getAdminServerUrl(true);
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("ngrok-skip-browser-warning", "true");
  if (String(DEVICE_API_KEY).length() > 0) {
    http.addHeader("X-Device-Key", DEVICE_API_KEY);
  }
  http.setTimeout(ADMIN_HTTP_TIMEOUT_MS);

  int httpCode = http.POST(body);
  String response = http.getString();
  http.end();

  lastAdminHttpCode = httpCode;

  Serial.print("HTTP ");
  Serial.print(httpCode);
  Serial.print(" | ");
  Serial.println(response);

  if (httpCode < 0) {
    getAdminServerUrl(true);
    lastAdminMessage = "HTTP error";
    return false;
  }

  bool ok = (httpCode >= 200 && httpCode < 300);
  lastAdminMessage = ok ? "Admin OK" : ("Admin fail: " + String(httpCode));
  return ok;
}

bool sendRegisterToAdminSystem() {
  if (USE_MQTT) {
    return publishToMqtt("register", buildRegistrationPayload());
  }

  return postToAdmin("/api/device/register", buildRegistrationPayload());
}

bool sendToAdminSystem() {
  if (USE_MQTT) {
    return publishToMqtt("telemetry", buildAdminPayload());
  }

  return postToAdmin("/api/device/update", buildAdminPayload());
}

// =====================================================
// WIFI
// =====================================================

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
  wifiConnected   = true;

  Serial.print("Connected | IP: ");
  Serial.print(WiFi.localIP());
  Serial.print(" | RSSI: ");
  Serial.println(WiFi.RSSI());

  showOLED("WiFi Connected", WiFi.localIP().toString(),
           "RSSI: " + String(WiFi.RSSI()),
           String(getActiveWiFiSSID()));
  delay(2000);
  return true;
}

void connectWiFi() {
  Serial.println("Connecting WiFi...");

  int preferred = getPreferredWiFiIndex();
  activeWiFiIndex = -1;
  wifiConnected   = false;

  for (int attempt = 0; attempt < WIFI_NETWORK_COUNT; attempt++) {
    int idx = (preferred + attempt) % WIFI_NETWORK_COUNT;
    if (connectWiFiByIndex(idx)) return;
  }

  Serial.println("WiFi failed on all SSIDs!");
  showOLED("WiFi Failed", "All SSID failed", "Sensor still runs");
  delay(2000);
}

void checkWiFiStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      Serial.print("WiFi Reconnected | IP: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  if (wifiConnected) {
    wifiConnected = false;
    Serial.println("WiFi Disconnected!");
  }

  connectWiFi();
}

// =====================================================
// SENSOR
// =====================================================

bool readSensor() {
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("DHT read failed!");
    showOLED("DHT Error", "Check D5", "VCC/GND/OUT");
    return false;
  }

  currentTemp = temp;
  currentHum  = hum;
  return true;
}

void updateLocalScore() {
  if (!baseSet) {
    baseTemp = currentTemp;
    maxTemp  = currentTemp;
    baseSet  = true;
  }

  if (currentTemp > maxTemp) maxTemp = currentTemp;

  currentScore = currentTemp - baseTemp;
  bestScore    = maxTemp - baseTemp;
}

void printSensorStatus() {
  Serial.print("Team: ");    Serial.print(TEAM_NAME);
  Serial.print(" | ID: ");   Serial.print(getChipId());
  Serial.print(" | Temp: "); Serial.print(currentTemp, 1); Serial.print(" C");
  Serial.print(" | Hum: ");  Serial.print(currentHum, 1);  Serial.print(" %");
  Serial.print(" | Best: +");Serial.print(bestScore, 1);   Serial.print(" C");
  Serial.print(" | IP: ");   Serial.print(WiFi.localIP());
  Serial.print(" | Admin: ");Serial.println(lastAdminMessage);
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("=== ESP8266 Heat Challenge ===");
  Serial.print("Team: ");   Serial.println(TEAM_NAME);
  Serial.print("Chip ID: ");Serial.println(getChipId());
  Serial.print("Mode: ");   Serial.println(USE_MQTT ? "MQTT HiveMQ Cloud" : "HTTP Admin System");
  Serial.print("Admin: ");  Serial.println(USE_MQTT ? MQTT_SERVER : ADMIN_SERVER);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    oledReady = true;
    showOLED("Heat Challenge", String(TEAM_NAME), "Chip: " + getChipId(), "Starting...");
  } else {
    Serial.println("OLED not found! Try 0x3D or check wiring.");
  }

  delay(1500);

  dht.begin();
  Serial.println("DHT22 Started");
  showOLED("DHT22 Started", "Reading sensor...", "Please wait");
  delay(1500);

  connectWiFi();
  if (USE_MQTT) setupMqttClient();

  if (WiFi.status() == WL_CONNECTED) {
    sendRegisterToAdminSystem();
    lastHeartbeatTime = millis();

    if (readSensor()) {
      updateLocalScore();
      sendToAdminSystem();
      lastAdminPostTime = millis();

      printSensorStatus();
      showSensorOLED();
      lastOLEDTime = millis();
    }
  }
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  unsigned long now = millis();

  if (now - lastWiFiCheckTime >= WIFI_CHECK_INTERVAL) {
    lastWiFiCheckTime = now;
    checkWiFiStatus();
  }

  maintainMqttConnection();

  if (WiFi.status() == WL_CONNECTED && now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
    lastHeartbeatTime = now;
    sendRegisterToAdminSystem();
  }

  if (now - lastReadTime >= READ_INTERVAL) {
    lastReadTime = now;
    if (!readSensor()) return;
    updateLocalScore();
    printSensorStatus();
  }

  if (baseSet && now - lastAdminPostTime >= ADMIN_POST_INTERVAL) {
    lastAdminPostTime = now;
    sendToAdminSystem();
  }

  if (baseSet && now - lastOLEDTime >= OLED_REFRESH_INTERVAL) {
    lastOLEDTime = now;
    showSensorOLED();
  }
}
