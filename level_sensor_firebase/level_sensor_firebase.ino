
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ============================================
// WIFI AND FIREBASE CONFIGURATION
// ============================================

#define WIFI_SSID "OKWY_MTN_4G"
#define WIFI_PASSWORD "okwy1234"

#define FIREBASE_API_KEY "AIzaSyA_IoO7JFkBfinhLauvFiIRb9iAg2mZWgY"

#define FIREBASE_DATABASE_URL \
  "https://lp06-test-default-rtdb.firebaseio.com"

#define FIREBASE_PATH "/lpg_sensor.json"

// ============================================
// L062MTW SENSOR CONFIGURATION
// ============================================

#define SENSOR_RX 4
#define SENSOR_TX 6

#define SENSOR_BAUD 115200

HardwareSerial LPGSerial(1);

const unsigned long MEASURE_INTERVAL = 2000;
const unsigned long SENSOR_TIMEOUT = 1200;
const unsigned long WIFI_RETRY_INTERVAL = 10000;
const unsigned long AUTH_RETRY_INTERVAL = 10000;

// ============================================
// FIREBASE AUTHENTICATION
// ============================================

String idToken = "";
String refreshToken = "";
String firebaseUID = "";

unsigned long tokenExpiry = 0;
unsigned long lastAuthAttempt = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastMeasurement = 0;

// ============================================
// SENSOR DATA
// ============================================

struct SensorData {
  uint16_t level = 0;
  float temperature = 0;
  uint16_t signal = 0;
  float tilt = 0;

  bool valid = false;
  bool received = false;

  unsigned long timestamp = 0;
};

SensorData sensor;

// ============================================
// HTTPS CLIENT
// ============================================

// Prototype only: certificate verification is
// disabled. Use CA validation for deployment.

bool httpRequest(
  const String &url,
  const String &method,
  const String &payload,
  String &response,
  int &status,
  const char *contentType
) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(7000);

  if (!http.begin(client, url)) {
    Serial.println("[HTTP] Begin failed");
    return false;
  }

  http.addHeader("Content-Type", contentType);

  if (method == "POST") {
    status = http.POST(payload);
  } else if (method == "PUT") {
    status = http.PUT(payload);
  } else {
    http.end();
    return false;
  }

  response = http.getString();
  http.end();

  return status >= 200 && status < 300;
}

// ============================================
// WIFI CONNECTION
// ============================================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastWiFiAttempt <
      WIFI_RETRY_INTERVAL &&
      lastWiFiAttempt != 0) {
    return;
  }

  lastWiFiAttempt = millis();

  Serial.println("[WiFi] Connecting...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void checkWiFi() {
  static bool wasConnected = false;

  bool connected =
    WiFi.status() == WL_CONNECTED;

  if (connected && !wasConnected) {
    Serial.println("[WiFi] Connected");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WiFi] RSSI: ");
    Serial.println(WiFi.RSSI());
  }

  if (!connected && wasConnected) {
    Serial.println("[WiFi] Disconnected");
  }

  wasConnected = connected;

  if (!connected) {
    connectWiFi();
  }
}

// ============================================
// ANONYMOUS FIREBASE SIGN-IN
// ============================================

bool firebaseSignIn() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  Serial.println("[Firebase] Signing in...");

  String url =
    "https://identitytoolkit.googleapis.com/"
    "v1/accounts:signUp?key=" +
    String(FIREBASE_API_KEY);

  String response;
  int status = 0;

  bool success = httpRequest(
    url,
    "POST",
    "{\"returnSecureToken\":true}",
    response,
    status,
    "application/json"
  );

  if (!success) {
    Serial.printf(
      "[Firebase] Sign-in failed: %d\n",
      status
    );
    Serial.println(response);
    return false;
  }

  JsonDocument doc;

  if (deserializeJson(doc, response)) {
    Serial.println("[Firebase] JSON error");
    return false;
  }

  idToken = doc["idToken"].as<String>();
  refreshToken =
    doc["refreshToken"].as<String>();
  firebaseUID = doc["localId"].as<String>();

  unsigned long expires =
    doc["expiresIn"].as<String>().toInt();

  if (idToken.isEmpty() ||
      refreshToken.isEmpty()) {
    Serial.println("[Firebase] Missing tokens");
    return false;
  }

  tokenExpiry =
    millis() + (expires > 120
      ? expires - 120 : 60) * 1000UL;

  Serial.println("[Firebase] Authenticated");
  Serial.print("[Firebase] UID: ");
  Serial.println(firebaseUID);

  return true;
}

// ============================================
// FIREBASE TOKEN REFRESH
// ============================================

bool firebaseRefreshToken() {
  if (refreshToken.isEmpty()) {
    return firebaseSignIn();
  }

  Serial.println("[Firebase] Refreshing token...");

  String url =
    "https://securetoken.googleapis.com/"
    "v1/token?key=" +
    String(FIREBASE_API_KEY);

  String payload =
    "grant_type=refresh_token&refresh_token=" +
    refreshToken;

  String response;
  int status = 0;

  bool success = httpRequest(
    url,
    "POST",
    payload,
    response,
    status,
    "application/x-www-form-urlencoded"
  );

  if (!success) {
    Serial.printf(
      "[Firebase] Refresh failed: %d\n",
      status
    );
    Serial.println(response);
    return false;
  }

  JsonDocument doc;

  if (deserializeJson(doc, response)) {
    return false;
  }

  String newToken =
    doc["id_token"].as<String>();

  String newRefresh =
    doc["refresh_token"].as<String>();

  unsigned long expires =
    doc["expires_in"].as<String>().toInt();

  if (newToken.isEmpty()) {
    return false;
  }

  idToken = newToken;

  if (!newRefresh.isEmpty()) {
    refreshToken = newRefresh;
  }

  tokenExpiry =
    millis() + (expires > 120
      ? expires - 120 : 60) * 1000UL;

  Serial.println("[Firebase] Token renewed");
  return true;
}

bool ensureFirebaseAuth() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  bool needsAuth =
    idToken.isEmpty() ||
    (int32_t)(millis() - tokenExpiry) >= 0;

  if (!needsAuth) {
    return true;
  }

  if (millis() - lastAuthAttempt <
      AUTH_RETRY_INTERVAL &&
      lastAuthAttempt != 0) {
    return false;
  }

  lastAuthAttempt = millis();

  if (refreshToken.isEmpty()) {
    return firebaseSignIn();
  }

  return firebaseRefreshToken();
}

// ============================================
// DECODE L062MTW SENSOR FRAME
// ============================================

bool decodeFrame(uint8_t *d) {
  uint8_t checksum = 0;

  for (int i = 0; i < 9; i++) {
    checksum += d[i];
  }

  Serial.print("[Sensor] RAW: ");

  for (int i = 0; i < 10; i++) {
    Serial.printf("%02X ", d[i]);
  }

  Serial.println();

  if (d[0] != 0xFF ||
      checksum != d[9]) {
    Serial.println(
      "[Sensor] Invalid checksum"
    );
    return false;
  }

  uint16_t level =
    (d[1] << 8) | d[2];

  int16_t temp =
    (d[3] << 8) | d[4];

  uint16_t signal =
    (d[5] << 8) | d[6];

  uint16_t angle =
    (d[7] << 8) | d[8];

  sensor.received = true;
  sensor.timestamp = millis();

  sensor.temperature = temp / 10.0f;
  sensor.signal = signal;
  sensor.tilt = angle / 10.0f;

  sensor.valid =
    level != 0xFFFD &&
    level != 0xFFFF &&
    level != 0xFFFE &&
    level >= 30 &&
    level <= 1000;

  if (sensor.valid) {
    sensor.level = level;

    Serial.printf(
      "[Sensor] Height: %u mm\n",
      sensor.level
    );
  } else {
    sensor.level = 0;
    Serial.println(
      "[Sensor] No valid liquid reading"
    );
  }

  Serial.printf(
    "[Sensor] Temperature: %.1f C\n",
    sensor.temperature
  );

  Serial.printf(
    "[Sensor] Signal: %u\n",
    sensor.signal
  );

  Serial.printf(
    "[Sensor] Tilt: %.1f degrees\n",
    sensor.tilt
  );

  return true;
}

// ============================================
// REQUEST SENSOR MEASUREMENT
// ============================================

void readSensor() {
  Serial.println();
  Serial.println("==========================");
  Serial.println("[Sensor] Requesting reading");

  sensor.received = false;
  sensor.valid = false;
  sensor.level = 0;

  while (LPGSerial.available()) {
    LPGSerial.read();
  }

  // Existing working trigger
  LPGSerial.write((uint8_t)0x00);
  LPGSerial.flush();

  uint8_t frame[10];
  int frameIndex = 0;

  unsigned long start = millis();

  while (millis() - start < SENSOR_TIMEOUT) {
    while (LPGSerial.available()) {
      uint8_t b = LPGSerial.read();

      if (frameIndex == 0 && b != 0xFF) {
        continue;
      }

      frame[frameIndex++] = b;

      if (frameIndex == 10) {
        if (decodeFrame(frame)) {
          return;
        }

        frameIndex = 0;
      }
    }

    delay(1);
  }

  Serial.println(
    "[Sensor] Measurement timeout"
  );
}

// ============================================
// UPLOAD DATA TO FIREBASE
// ============================================

bool uploadToFirebase() {
  if (!ensureFirebaseAuth()) {
    Serial.println(
      "[Firebase] Authentication unavailable"
    );
    return false;
  }

  JsonDocument doc;

  doc["valid"] = sensor.valid;
  doc["status"] =
    !sensor.received ? "timeout" :
    sensor.valid ? "ok" : "no_liquid";

  if (sensor.valid) {
    doc["level_mm"] = sensor.level;
  } else {
    doc["level_mm"] = nullptr;
  }

  if (sensor.received) {
    doc["temperature_c"] =
      sensor.temperature;
    doc["signal"] = sensor.signal;
    doc["tilt_deg"] = sensor.tilt;
  } else {
    doc["temperature_c"] = nullptr;
    doc["signal"] = nullptr;
    doc["tilt_deg"] = nullptr;
  }

  doc["uptime_ms"] = millis();
  doc["wifi_rssi"] = WiFi.RSSI();

  // Firebase server timestamp
  JsonObject ts =
    doc["timestamp"].to<JsonObject>();
  ts[".sv"] = "timestamp";

  String payload;
  serializeJson(doc, payload);

  String url =
    String(FIREBASE_DATABASE_URL) +
    FIREBASE_PATH +
    "?auth=" + idToken;

  String response;
  int status = 0;

  Serial.println("[Firebase] Uploading...");

  bool success = httpRequest(
    url,
    "PUT",
    payload,
    response,
    status,
    "application/json"
  );

  if (success) {
    Serial.println("[Firebase] Upload OK");
    return true;
  }

  Serial.printf(
    "[Firebase] Upload failed: HTTP %d\n",
    status
  );

  Serial.println(response);

  if (status == 401) {
    tokenExpiry = 0;
  }

  return false;
}

// ============================================
// SETUP
// ============================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("==========================");
  Serial.println("L062MTW FIREBASE MONITOR");
  Serial.println("==========================");

  LPGSerial.begin(
    SENSOR_BAUD,
    SERIAL_8N1,
    SENSOR_RX,
    SENSOR_TX
  );

  Serial.println("[Sensor] UART ready");

  WiFi.mode(WIFI_STA);
  connectWiFi();
}

// ============================================
// MAIN LOOP
// ============================================

void loop() {
  checkWiFi();

  if (millis() - lastMeasurement >=
      MEASURE_INTERVAL ||
      lastMeasurement == 0) {

    lastMeasurement = millis();

    readSensor();

    if (WiFi.status() == WL_CONNECTED) {
      uploadToFirebase();
    } else {
      Serial.println(
        "[Firebase] Offline, upload skipped"
      );
    }
  }

  delay(10);
}