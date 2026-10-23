
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <math.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>

// =====================================================
// WIFI SETTINGS
// =====================================================

// 192.168.1.100

const char* WIFI_SSID = "OKWY_MTN_4G";
const char* WIFI_PASSWORD = "okwy1234";
const char* API_TOKEN = "RTI_CCL_1234567890";


// Enable only temporarily when debugging locally
const bool PRINT_WIFI_PASSWORD = false;

// FIREBASE TELEMETRY (same project as the LPG level sensor)
const char* FIREBASE_API_KEY = "AIzaSyA_IoO7JFkBfinhLauvFiIRb9iAg2mZWgY";
const char* FIREBASE_DB = "https://lp06-test-default-rtdb.firebaseio.com";
const unsigned long CLOUD_INTERVAL_MS = 5000;
// Cloud commands are fetched asynchronously. Existing motor rules execute in loop().
const unsigned long CLOUD_COMMAND_POLL_MS = 2500;


// =====================================================
// PIN CONFIGURATION
// =====================================================

#define GAS_SENSOR_PIN 0

#define BUZZER_PIN 2

#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5

#define MOTOR_IN1_PIN 6
#define MOTOR_IN2_PIN 7

#define LED_GREEN_PIN 20
#define LED_BLUE_PIN 21
#define LED_RED_PIN 10

// =====================================================
// SENSOR CONFIGURATION
// =====================================================

const float R1 = 15000.0;
const float R2 = 20000.0;
const float DIVIDER_RATIO = R2 / (R1 + R2);

// Experimental threshold. Requires calibration.
const float GAS_ALARM_VOLTAGE = 1.8;

// Bench warm-up time
const unsigned long GAS_WARMUP_MS = 5000;

// =====================================================
// MOTOR CONFIGURATION
// =====================================================

// Timed movement only. No position feedback.
// These durations require physical verification.

const unsigned long MOTOR_CLOSE_TIME_MS = 25000;
const unsigned long MOTOR_OPEN_TIME_MS = 25000;

// =====================================================
// TIMING CONFIGURATION
// =====================================================

const unsigned long SENSOR_INTERVAL_MS = 200;
const unsigned long SERIAL_INTERVAL_MS = 1000;
const unsigned long WIFI_REPORT_INTERVAL_MS = 5000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;

// =====================================================
// OBJECTS
// =====================================================

WebServer server(80);
Adafruit_BMP280 bmp;

// =====================================================
// SYSTEM VARIABLES
// =====================================================

unsigned long bootTime = 0;
unsigned long lastSensorTime = 0;
unsigned long lastSerialTime = 0;
unsigned long lastWifiReportTime = 0;
unsigned long lastWifiRetryTime = 0;
unsigned long motorStartTime = 0;

bool bmpFound = false;
bool gasReady = false;
bool gasDetected = false;
bool alarmLatched = false;
bool wifiPreviouslyConnected = false;

int rawADC = 0;

float adcVoltage = NAN;
float micsVoltage = NAN;
float temperatureC = NAN;
float pressureHpa = NAN;

// =====================================================
// VALVE STATES
// =====================================================

enum ValveState {
  POSITION_UNKNOWN,
  OPENING,
  ESTIMATED_OPEN,
  CLOSING,
  ESTIMATED_CLOSED
};

ValveState valveState = POSITION_UNKNOWN;

String lastEvent = "System started";

// =====================================================
// VALVE STATE STRING
// =====================================================

const char* valveStateName() {

  switch (valveState) {

    case POSITION_UNKNOWN:
      return "UNKNOWN";

    case OPENING:
      return "OPENING";

    case ESTIMATED_OPEN:
      return "ESTIMATED_OPEN";

    case CLOSING:
      return "CLOSING";

    case ESTIMATED_CLOSED:
      return "ESTIMATED_CLOSED";
  }

  return "UNKNOWN";
}

// =====================================================
// WIFI STATUS STRING
// =====================================================

const char* wifiStatusName(wl_status_t status) {

  switch (status) {

    case WL_CONNECTED:
      return "CONNECTED";

    case WL_NO_SSID_AVAIL:
      return "SSID NOT FOUND";

    case WL_CONNECT_FAILED:
      return "CONNECTION FAILED";

    case WL_DISCONNECTED:
      return "DISCONNECTED";

    case WL_IDLE_STATUS:
      return "IDLE";

    case WL_CONNECTION_LOST:
      return "CONNECTION LOST";

    default:
      return "OTHER STATUS";
  }
}

// =====================================================
// PRINT WIFI CONFIGURATION
// =====================================================

void printWifiConfiguration() {

  Serial.println();
  Serial.println("========== WIFI CONFIGURATION ==========");

  Serial.print("Router SSID: ");
  Serial.println(WIFI_SSID);

  Serial.print("Router Password: ");

  if (PRINT_WIFI_PASSWORD) {

    Serial.println(WIFI_PASSWORD);

  } else {

    Serial.println("[HIDDEN]");
  }

  Serial.print("Password Length: ");
  Serial.println(strlen(WIFI_PASSWORD));

  Serial.println("WiFi Mode: STATION");
  Serial.println("Required WiFi Band: 2.4 GHz");

  Serial.print("ESP32 MAC Address: ");
  Serial.println(WiFi.macAddress());

  Serial.println("========================================");
  Serial.println();
}

// =====================================================
// PRINT WIFI STATUS
// =====================================================

void printWifiStatus() {

  Serial.println();
  Serial.println("------------ WIFI STATUS ------------");

  Serial.print("Configured SSID: ");
  Serial.println(WIFI_SSID);

  Serial.print("Status: ");
  Serial.println(wifiStatusName(WiFi.status()));

  Serial.print("Status Code: ");
  Serial.println((int)WiFi.status());

  if (WiFi.status() == WL_CONNECTED) {

    Serial.print("Connected Router: ");
    Serial.println(WiFi.SSID());

    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());

    Serial.print("Subnet Mask: ");
    Serial.println(WiFi.subnetMask());

    Serial.print("Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    Serial.print("Dashboard API: http://");
    Serial.println(WiFi.localIP());

  } else {

    Serial.println("IP Address: NOT ASSIGNED");

    Serial.println(
      "Check SSID, password, signal and 2.4 GHz band."
    );
  }

  Serial.println("-------------------------------------");
}

// =====================================================
// MOTOR CONTROL
// =====================================================

void motorStop() {

  digitalWrite(MOTOR_IN1_PIN, LOW);
  digitalWrite(MOTOR_IN2_PIN, LOW);

  Serial.println("[MOTOR] STOPPED");
}

void startClosing() {

  motorStop();

  valveState = CLOSING;
  motorStartTime = millis();

  digitalWrite(MOTOR_IN1_PIN, HIGH);
  digitalWrite(MOTOR_IN2_PIN, LOW);

  lastEvent = "Closing started";

  Serial.println();
  Serial.println("[MOTOR] CLOSING REGULATOR");
  Serial.println("[MOTOR] IN1 = HIGH");
  Serial.println("[MOTOR] IN2 = LOW");
}

void startOpening() {

  motorStop();

  valveState = OPENING;
  motorStartTime = millis();

  digitalWrite(MOTOR_IN1_PIN, LOW);
  digitalWrite(MOTOR_IN2_PIN, HIGH);

  lastEvent = "Opening started";

  Serial.println();
  Serial.println("[MOTOR] OPENING REGULATOR");
  Serial.println("[MOTOR] IN1 = LOW");
  Serial.println("[MOTOR] IN2 = HIGH");
}

void stopMovement() {

  motorStop();

  valveState = POSITION_UNKNOWN;
  lastEvent = "Manual motor stop";

  Serial.println(
    "[MOTOR] Position now UNKNOWN"
  );
}

// =====================================================
// SENSOR READINGS
// =====================================================

void readSensors() {

  unsigned long now = millis();

  if (now - lastSensorTime < SENSOR_INTERVAL_MS)
    return;

  lastSensorTime = now;

  rawADC = analogRead(GAS_SENSOR_PIN);

  adcVoltage =
    analogReadMilliVolts(GAS_SENSOR_PIN) / 1000.0;

  micsVoltage = adcVoltage / DIVIDER_RATIO;

  gasReady =
    (now - bootTime >= GAS_WARMUP_MS);

  gasDetected =
    gasReady &&
    isfinite(micsVoltage) &&
    micsVoltage >= GAS_ALARM_VOLTAGE;

  if (bmpFound) {

    temperatureC = bmp.readTemperature();

    pressureHpa =
      bmp.readPressure() / 100.0;
  }

  // Automatic alarm closure

  if (gasDetected && !alarmLatched) {

    alarmLatched = true;

    lastEvent = "Gas alarm detected";

    Serial.println();
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("!!! GAS THRESHOLD EXCEEDED !!!");
    Serial.println("!!! AUTOMATIC CLOSE !!!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    if (
      valveState != CLOSING &&
      valveState != ESTIMATED_CLOSED
    ) {

      startClosing();
    }
  }
}

// =====================================================
// MOTOR STATE MACHINE
// =====================================================

void updateMotor() {

  unsigned long elapsed =
    millis() - motorStartTime;

  if (valveState == OPENING) {

    if (gasDetected || alarmLatched) {

      Serial.println(
        "[SAFETY] Opening interrupted by alarm"
      );

      startClosing();

    } else if (elapsed >= MOTOR_OPEN_TIME_MS) {

      motorStop();

      valveState = ESTIMATED_OPEN;

      lastEvent = "Opening time completed";

      Serial.println(
        "[MOTOR] OPENING TIME COMPLETE"
      );

      Serial.println(
        "[MOTOR] Physical position NOT VERIFIED"
      );
    }
  }

  else if (valveState == CLOSING) {

    if (elapsed >= MOTOR_CLOSE_TIME_MS) {

      motorStop();

      valveState = ESTIMATED_CLOSED;

      lastEvent = "Closing time completed";

      Serial.println(
        "[MOTOR] CLOSING TIME COMPLETE"
      );

      Serial.println(
        "[MOTOR] Physical position NOT VERIFIED"
      );
    }
  }
}

// =====================================================
// INDICATORS
// =====================================================

void updateIndicators() {

  digitalWrite(
    BUZZER_PIN,
    alarmLatched ? HIGH : LOW
  );

  digitalWrite(
    LED_GREEN_PIN,
    valveState == ESTIMATED_OPEN &&
    !alarmLatched ? HIGH : LOW
  );

  digitalWrite(
    LED_RED_PIN,
    alarmLatched ||
    valveState == CLOSING ||
    valveState == ESTIMATED_CLOSED
      ? HIGH : LOW
  );

  digitalWrite(
    LED_BLUE_PIN,
    WiFi.status() == WL_CONNECTED
      ? HIGH : LOW
  );
}

// =====================================================
// JSON VALUE HELPER
// =====================================================

String jsonFloat(float value, int digits) {

  if (!isfinite(value))
    return "null";

  return String(value, digits);
}

// =====================================================
// HTTP STATUS ENDPOINT
// =====================================================

void handleStatus() {

  String json = "{";

  json += "\"uptime_s\":";
  json += String(millis() / 1000);
  json += ",";

  json += "\"raw_adc\":";
  json += String(rawADC);
  json += ",";

  json += "\"adc_voltage_v\":";
  json += jsonFloat(adcVoltage, 3);
  json += ",";

  json += "\"sensor_ao_voltage_v\":";
  json += jsonFloat(micsVoltage, 3);
  json += ",";

  json += "\"temperature_c\":";
  json += jsonFloat(temperatureC, 2);
  json += ",";

  json += "\"pressure_hpa\":";
  json += jsonFloat(pressureHpa, 2);
  json += ",";

  json += "\"bmp_found\":";
  json += bmpFound ? "true" : "false";
  json += ",";

  json += "\"gas_ready\":";
  json += gasReady ? "true" : "false";
  json += ",";

  json += "\"gas_detected\":";
  json += gasDetected ? "true" : "false";
  json += ",";

  json += "\"alarm_latched\":";
  json += alarmLatched ? "true" : "false";
  json += ",";

  json += "\"gas_threshold_v\":";
  json += String(GAS_ALARM_VOLTAGE, 2);
  json += ",";

  json += "\"valve_state\":\"";
  json += valveStateName();
  json += "\",";

  json += "\"position_verified\":false,";

  json += "\"wifi_rssi_dbm\":";
  json += String(WiFi.RSSI());
  json += ",";

  json += "\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\",";

  json += "\"event\":\"";
  json += lastEvent;
  json += "\"";

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}

// =====================================================
// AUTHENTICATION
// =====================================================

bool authorized() {

  if (
    server.header("X-API-Key") !=
    String(API_TOKEN)
  ) {

    Serial.println(
      "[HTTP] Unauthorized command"
    );

    server.send(
      401,
      "application/json",
      "{\"ok\":false,\"error\":\"Unauthorized\"}"
    );

    return false;
  }

  return true;
}

// =====================================================
// COMMAND RESPONSE
// =====================================================

void commandResponse(
  bool ok,
  int code,
  String message
) {

  String json = "{\"ok\":";

  json += ok ? "true" : "false";

  json += ",\"message\":\"";
  json += message;
  json += "\"}";

  server.send(
    code,
    "application/json",
    json
  );

  Serial.print("[COMMAND] ");
  Serial.println(message);
}

// =====================================================
// STREAMLIT COMMAND HANDLING
// =====================================================

void handleCommand() {

  if (!authorized())
    return;

  String action = server.arg("action");

  action.toUpperCase();

  Serial.println();
  Serial.println("========== COMMAND RECEIVED ==========");

  Serial.print("Action: ");
  Serial.println(action);

  Serial.print("Client IP: ");
  Serial.println(server.client().remoteIP());

  if (action == "STOP") {

    stopMovement();

    commandResponse(
      true,
      200,
      "Motor stopped"
    );
  }

  else if (action == "CLOSE") {

    if (valveState == CLOSING) {

      commandResponse(
        false,
        409,
        "Already closing"
      );

    } else {

      startClosing();

      commandResponse(
        true,
        200,
        "Closing started"
      );
    }
  }

  else if (action == "RESET_ALARM") {

  Serial.println("[ALARM] Reset requested");

  if (!gasReady || gasDetected || !isfinite(micsVoltage)) {

    commandResponse(
      false,
      409,
      "Reset rejected: gas sensor not ready or gas detected"
    );

  } else if (
    valveState != ESTIMATED_CLOSED
  ) {

    commandResponse(
      false,
      409,
      "Reset rejected: regulator not in closed state"
    );

  } else {

    alarmLatched = false;

    lastEvent = "Alarm acknowledged and reset";

    Serial.println("[ALARM] Alarm reset accepted");
    Serial.println("[ALARM] Buzzer will stop");

    commandResponse(
      true,
      200,
      "Alarm reset. Regulator remains closed."
    );
  }
}

  else if (action == "OPEN") {

    if (
      !gasReady ||
      gasDetected ||
      alarmLatched ||
      !bmpFound
    ) {

      commandResponse(
        false,
        409,
        "Opening blocked by safety interlock"
      );

    }

    else if (valveState != ESTIMATED_CLOSED) {

      commandResponse(
        false,
        409,
        "Opening blocked: close cycle required"
      );
    }

    else {

      startOpening();

      commandResponse(
        true,
        200,
        "Opening started"
      );
    }
  }

  else {

    commandResponse(
      false,
      400,
      "Unknown command"
    );
  }

  Serial.println("======================================");
}

// =====================================================
// SERIAL DIAGNOSTICS
// =====================================================

void printDiagnostics() {

  Serial.println();
  Serial.println("======================================");
  Serial.println("       LIVE SYSTEM DIAGNOSTICS");
  Serial.println("======================================");

  Serial.print("Uptime: ");
  Serial.print(millis() / 1000);
  Serial.println(" seconds");

  Serial.println();
  Serial.println("[WIFI]");

  Serial.print("Router: ");
  Serial.println(WIFI_SSID);

  Serial.print("Status: ");
  Serial.println(wifiStatusName(WiFi.status()));

  if (WiFi.status() == WL_CONNECTED) {

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("Signal: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  }

  Serial.println();
  Serial.println("[MiCS-5524]");

  Serial.print("Raw ADC: ");
  Serial.println(rawADC);

  Serial.print("ESP32 ADC Voltage: ");
  Serial.print(adcVoltage, 3);
  Serial.println(" V");

  Serial.print("MiCS AO Voltage: ");
  Serial.print(micsVoltage, 3);
  Serial.println(" V");

  Serial.print("Threshold: ");
  Serial.print(GAS_ALARM_VOLTAGE, 3);
  Serial.println(" V");

  Serial.print("Monitoring Ready: ");
  Serial.println(gasReady ? "YES" : "NO");

  Serial.print("Gas Threshold Exceeded: ");
  Serial.println(gasDetected ? "YES" : "NO");

  Serial.print("Alarm Latched: ");
  Serial.println(alarmLatched ? "YES" : "NO");

  Serial.println();
  Serial.println("[BMP280]");

  Serial.print("Detected: ");
  Serial.println(bmpFound ? "YES" : "NO");

  if (bmpFound) {

    Serial.print("Temperature: ");
    Serial.print(temperatureC, 2);
    Serial.println(" C");

    Serial.print("Pressure: ");
    Serial.print(pressureHpa, 2);
    Serial.println(" hPa");
  }

  Serial.println();
  Serial.println("[REGULATOR]");

  Serial.print("Valve State: ");
  Serial.println(valveStateName());

  if (
    valveState == OPENING ||
    valveState == CLOSING
  ) {

    Serial.print("Motor Elapsed Time: ");
    Serial.print(
      (millis() - motorStartTime) / 1000
    );
    Serial.println(" seconds");
  }

  Serial.print("Last Event: ");
  Serial.println(lastEvent);

  Serial.println("======================================");
}

// =====================================================
// WIFI CONNECTION MANAGEMENT
// =====================================================

void manageWiFi() {

  unsigned long now = millis();

  bool connected =
    WiFi.status() == WL_CONNECTED;

  if (connected && !wifiPreviouslyConnected) {

    Serial.println();
    Serial.println("******** WIFI CONNECTED ********");

    printWifiStatus();

    wifiPreviouslyConnected = true;
  }

  if (!connected && wifiPreviouslyConnected) {

    Serial.println();
    Serial.println("******** WIFI DISCONNECTED ********");

    wifiPreviouslyConnected = false;
  }

  if (!connected &&
      now - lastWifiRetryTime >= WIFI_RETRY_INTERVAL_MS) {

    lastWifiRetryTime = now;

    Serial.println();
    Serial.println("[WIFI] Retrying connection...");

    Serial.print("[WIFI] SSID: ");
    Serial.println(WIFI_SSID);

    WiFi.reconnect();
  }

  if (now - lastWifiReportTime >= WIFI_REPORT_INTERVAL_MS) {

    lastWifiReportTime = now;

    printWifiStatus();
  }
}

// =====================================================
// FIREBASE CLOUD TELEMETRY TASK
// Runs separately so HTTPS delays do not intentionally
// block the existing sensor/motor/control loop.
// Bench prototype: setInsecure disables TLS peer verification.
// =====================================================

// One-command mailbox. The cloud task only queues requests; loop() alone drives GPIOs.
struct CloudCommand { char id[48]; char action[20]; };
struct CloudAck { char id[48]; char state[20]; char message[110]; };

void publishCloudAck(const CloudAck &ack);

QueueHandle_t cloudCommandQueue = nullptr;
QueueHandle_t cloudAckQueue = nullptr;
Preferences commandPrefs;
String lastHandledCommandId;
static String cloudIdToken, cloudRefreshToken;
static uint32_t cloudExpiryMs = 0;
static uint32_t cloudNextAuthMs = 0;

bool cloudHttp(const String &url, const String &method,
               const String &payload, const char* contentType,
               String &reply, int &code) {
  WiFiClientSecure client;
  client.setInsecure();  // Replace with CA verification for deployment
  HTTPClient http;
  http.setTimeout(4500);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", contentType);
  code = method == "PUT" ? http.PUT(payload) : (method == "GET" ? http.GET() : http.POST(payload));
  reply = http.getString();
  http.end();
  return code >= 200 && code < 300;
}

bool cloudAuthenticate() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String reply;
  int code = 0;
  bool refresh = cloudRefreshToken.length() > 0;
  String url = refresh
    ? String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY
    : String("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=") + FIREBASE_API_KEY;
  String payload = refresh
    ? String("grant_type=refresh_token&refresh_token=") + cloudRefreshToken
    : String("{\"returnSecureToken\":true}");
  bool ok = cloudHttp(url, "POST", payload,
    refresh ? "application/x-www-form-urlencoded" : "application/json",
    reply, code);
  if (!ok) {
    Serial.printf("[CLOUD] Auth HTTP %d: %.120s\n", code, reply.c_str());
    // Retry the existing anonymous identity instead of silently creating new ones.
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, reply)) return false;
  String token = refresh ? doc["id_token"].as<String>() : doc["idToken"].as<String>();
  String newRefresh = refresh ? doc["refresh_token"].as<String>() : doc["refreshToken"].as<String>();
  unsigned long lifetime = (refresh ? doc["expires_in"] : doc["expiresIn"]).as<String>().toInt();
  if (token.isEmpty() || newRefresh.isEmpty()) return false;
  cloudIdToken = token;
  cloudRefreshToken = newRefresh;
  cloudExpiryMs = millis() + (lifetime > 180 ? lifetime - 120 : 60) * 1000UL;
  Serial.println("[CLOUD] Firebase authenticated");
  return true;
}


// Remote requests use the SAME conditions as the original /command handler.
// No GPIO or valve-state changes occur in the cloud task.
void processCloudCommands() {
  CloudCommand cmd;
  if (!cloudCommandQueue || xQueueReceive(cloudCommandQueue, &cmd, 0) != pdTRUE) return;
  bool accepted = false;
  String msg;
  String action(cmd.action);
  if (action == "STOP") {
    stopMovement(); accepted = true; msg = "Motor stopped";
  } else if (action == "CLOSE") {
    if (valveState == CLOSING) msg = "Already closing";
    else { startClosing(); accepted = true; msg = "Closing started"; }
  } else if (action == "RESET_ALARM") {
    if (!gasReady || gasDetected || !isfinite(micsVoltage))
      msg = "Reset rejected: gas sensor not ready or gas detected";
    else if (valveState != ESTIMATED_CLOSED)
      msg = "Reset rejected: regulator not in closed state";
    else { alarmLatched = false; lastEvent = "Alarm acknowledged and reset";
      accepted = true; msg = "Alarm reset. Regulator remains closed."; }
  } else if (action == "OPEN") {
    if (!gasReady || gasDetected || alarmLatched || !bmpFound)
      msg = "Opening blocked by safety interlock";
    else if (valveState != ESTIMATED_CLOSED)
      msg = "Opening blocked: close cycle required";
    else { startOpening(); accepted = true; msg = "Opening started"; }
  } else msg = "Unknown command";
  lastHandledCommandId = String(cmd.id);
  commandPrefs.putString("last_id", lastHandledCommandId);
  CloudAck ack = {};
  strlcpy(ack.id, cmd.id, sizeof(ack.id));
  strlcpy(ack.state, accepted ? "accepted" : "rejected", sizeof(ack.state));
  strlcpy(ack.message, msg.c_str(), sizeof(ack.message));
  xQueueSend(cloudAckQueue, &ack, 0);
  Serial.printf("[REMOTE] %s: %s\n", cmd.action, msg.c_str());
}

// GET /commands/smart_regulator: short-lived commands only.
// Persist last ID across reset so the same request cannot replay on reboot.
void pollCloudCommand() {
  String reply; int code = 0;
  String url = String(FIREBASE_DB) + "/commands/smart_regulator.json?auth=" + cloudIdToken;
  if (!cloudHttp(url, "GET", "", "application/json", reply, code)) {
    if (code == 401) cloudExpiryMs = 0;
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, reply) || !doc.is<JsonObject>()) return;
  String id = doc["id"].as<String>();
  String action = doc["action"].as<String>();
  long long issued = doc["issued_at"].as<long long>();
  time_t now = time(nullptr);
  if (id.isEmpty() || id == lastHandledCommandId ||
      now < 1700000000 || issued <= 0 ||
      (long long)now - issued > 20 || issued - (long long)now > 5) return;
  if (action != "OPEN" && action != "CLOSE" && action != "STOP" && action != "RESET_ALARM") return;
  if (uxQueueMessagesWaiting(cloudCommandQueue) != 0) return;
  CloudCommand cmd = {};
  strlcpy(cmd.id, id.c_str(), sizeof(cmd.id));
  strlcpy(cmd.action, action.c_str(), sizeof(cmd.action));
  if (xQueueSend(cloudCommandQueue, &cmd, 0) == pdTRUE)
    Serial.printf("[REMOTE] Queued %s id=%s\n", cmd.action, cmd.id);
}

void publishCloudAck(const CloudAck &ack) {
  JsonDocument doc;
  doc["id"] = ack.id;
  doc["state"] = ack.state;
  doc["message"] = ack.message;
  doc["valve_state"] = valveStateName();
  doc["timestamp"][".sv"] = "timestamp";
  String payload, reply; serializeJson(doc, payload);
  int code = 0;
  String url = String(FIREBASE_DB) + "/commands/smart_regulator_ack.json?auth=" + cloudIdToken;
  bool ok = cloudHttp(url, "PUT", payload, "application/json", reply, code);
  Serial.printf("[REMOTE] Ack %s (HTTP %d)\n", ok ? "OK" : "FAILED", code);
}

void cloudTask(void* arg) {
  uint32_t lastPoll = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED &&
        strcmp(FIREBASE_API_KEY, "PASTE_YOUR_FIREBASE_WEB_API_KEY") != 0) {
      if (cloudIdToken.isEmpty() || (int32_t)(millis() - cloudExpiryMs) >= 0) {
        if ((int32_t)(millis() - cloudNextAuthMs) >= 0) {
          cloudNextAuthMs = millis() + 10000UL;
          cloudAuthenticate();
        }
      }
      if (!cloudIdToken.isEmpty() && (int32_t)(millis() - cloudExpiryMs) < 0) {
        if (millis() - lastPoll >= CLOUD_COMMAND_POLL_MS) {
          lastPoll = millis();
          pollCloudCommand();
        }
        CloudAck ack;
        if (cloudAckQueue && xQueueReceive(cloudAckQueue, &ack, 0) == pdTRUE)
          publishCloudAck(ack);
        // Snapshot numeric/enum fields. No changes to the original control logic.
        JsonDocument doc;
        doc["raw_adc"] = rawADC;
        if (isfinite(adcVoltage)) doc["adc_voltage_v"] = adcVoltage;
        if (isfinite(micsVoltage)) doc["sensor_ao_voltage_v"] = micsVoltage;
        if (isfinite(temperatureC)) doc["temperature_c"] = temperatureC;
        if (isfinite(pressureHpa)) doc["pressure_hpa"] = pressureHpa;
        doc["bmp_found"] = bmpFound;
        doc["gas_ready"] = gasReady;
        doc["gas_detected"] = gasDetected;
        doc["alarm_latched"] = alarmLatched;
        doc["gas_threshold_v"] = GAS_ALARM_VOLTAGE;
        doc["valve_state"] = valveStateName();
        doc["position_verified"] = false;
        doc["wifi_rssi_dbm"] = WiFi.RSSI();
        doc["uptime_s"] = millis() / 1000UL;
        doc["timestamp"][".sv"] = "timestamp";
        String payload, reply;
        serializeJson(doc, payload);
        int code = 0;
        String url = String(FIREBASE_DB) + "/devices/smart_regulator.json?auth=" + cloudIdToken;
        bool ok = cloudHttp(url, "PUT", payload, "application/json", reply, code);
        Serial.printf("[CLOUD] Upload %s (HTTP %d)\n", ok ? "OK" : "FAILED", code);
        if (code == 401) cloudExpiryMs = 0;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(CLOUD_COMMAND_POLL_MS));
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  delay(1500);

  bootTime = millis();

  Serial.println();
  Serial.println("======================================");
  Serial.println(" SMART LPG REGULATOR TEST CONTROLLER");
  Serial.println(" ESP32-C3 + MiCS-5524 + BMP280");
  Serial.println(" DRV8871 + WIFI + STREAMLIT");
  Serial.println("======================================");

  // ---------------------------------------------------
  // INITIALIZE GAS SENSOR
  // ---------------------------------------------------

  Serial.println("[SETUP] Initializing MiCS-5524");

  pinMode(GAS_SENSOR_PIN, INPUT);

  analogReadResolution(12);

  analogSetPinAttenuation(
    GAS_SENSOR_PIN,
    ADC_11db
  );

  Serial.println("[SETUP] MiCS-5524 configured");

  // ---------------------------------------------------
  // INITIALIZE BUZZER
  // ---------------------------------------------------

  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);

  Serial.println("[SETUP] Buzzer configured");

  // ---------------------------------------------------
  // INITIALIZE MOTOR
  // ---------------------------------------------------

  pinMode(MOTOR_IN1_PIN, OUTPUT);
  pinMode(MOTOR_IN2_PIN, OUTPUT);

  motorStop();

  Serial.println("[SETUP] Motor driver configured");

  // ---------------------------------------------------
  // INITIALIZE LEDS
  // ---------------------------------------------------

  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);

  updateIndicators();

  Serial.println("[SETUP] LEDs configured");

  // ---------------------------------------------------
  // INITIALIZE BMP280
  // ---------------------------------------------------

  Serial.println("[SETUP] Initializing I2C");

  Wire.begin(
    I2C_SDA_PIN,
    I2C_SCL_PIN
  );

  Serial.println(
    "[SETUP] Searching BMP280 at 0x77"
  );

  bmpFound = bmp.begin(0x77);

  if (!bmpFound) {

    Serial.println(
      "[SETUP] Searching BMP280 at 0x76"
    );

    bmpFound = bmp.begin(0x76);
  }

  if (bmpFound) {

    Serial.println("[SETUP] BMP280 FOUND");

  } else {

    Serial.println(
      "[WARNING] BMP280 NOT FOUND"
    );
  }

  // ---------------------------------------------------
  // INITIALIZE WIFI
  // ---------------------------------------------------

  Serial.println();
  Serial.println("[SETUP] Initializing Wi-Fi");

  WiFi.mode(WIFI_STA);

  WiFi.setAutoReconnect(true);

  printWifiConfiguration();

  Serial.println("[WIFI] Starting connection...");

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  lastWifiRetryTime = millis();

  // ---------------------------------------------------
  // INITIALIZE HTTP SERVER
  // ---------------------------------------------------

  const char* headerKeys[] = {
    "X-API-Key"
  };

  server.collectHeaders(headerKeys, 1);

  server.on(
    "/status",
    HTTP_GET,
    handleStatus
  );

  server.on(
    "/command",
    HTTP_POST,
    handleCommand
  );

  server.onNotFound([]() {

    server.send(
      404,
      "application/json",
      "{\"error\":\"Not found\"}"
    );
  });

  server.begin();

  // Network telemetry is isolated from the existing loop.
  commandPrefs.begin("lpg_remote", false);
  lastHandledCommandId = commandPrefs.getString("last_id", "");
  cloudCommandQueue = xQueueCreate(1, sizeof(CloudCommand));
  cloudAckQueue = xQueueCreate(4, sizeof(CloudAck));
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  xTaskCreate(cloudTask, "firebase", 16384, nullptr, 1, nullptr);

  Serial.println("[SETUP] HTTP server configured");

  Serial.println();

  // =====================================================
  // AUTOMATIC VALVE OPENING ON BOOT
  // =====================================================

  Serial.println();
  Serial.println("[STARTUP] Automatic regulator opening");
  Serial.println("[STARTUP] Commanding DRV8871 to OPEN");

  startOpening();

  Serial.println("[STARTUP] Opening command issued");
  Serial.println("[STARTUP] Motor will stop after configured travel time");

  Serial.println("========== SETUP COMPLETE ==========");
  Serial.println("Motor: STOPPED");
  Serial.println("Valve Position: UNKNOWN");
  Serial.println("Serial diagnostics: every 1 second");
  Serial.println("WiFi diagnostics: every 5 seconds");
  Serial.println("====================================");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

  readSensors();

  processCloudCommands();

  updateMotor();

  updateIndicators();

  manageWiFi();

  server.handleClient();

  unsigned long now = millis();

  if (now - lastSerialTime >= SERIAL_INTERVAL_MS) {

    lastSerialTime = now;

    printDiagnostics();
  }

  delay(5);
}