// ======================================================
// SMART GREENHOUSE SYSTEM (FUZZY + AUTO + 2 PUMPS)
// ESP32 + DHT22 + 2 SOIL SENSOR + BLYNK + SUPABASE 
// ======================================================

// ---- LIBRARIES ----
#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "DHT.h"
#include <WiFiManager.h>
#include <math.h>
#include <Arduino.h>
#include "secrets.h"

JsonDocument json; 

// ---- DHT SENSOR ----
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

#define PI 3.1415926535897932384626433832795

// ---- HARDWARE ----
#define SOIL_PIN1 34
#define SOIL_PIN2 35
#define IRRIGATION_PUMP_PIN1 14
#define IRRIGATION_PUMP_PIN2 27
// #define TANK_PUMP_PIN 27
#define TRIG_PIN 5
#define ECHO_PIN 18


// ---- VARIABLES ----

bool isSyncing = true;
float temperature = 0;
float humidity = 0;
float absoluteHumidity = 0;
float soilMoisture1 = 0;
float soilMoisture2 = 0;
float avrgSoilMoisture = 0;
float waterPercent = 0;

float soilWarningThreshold = 25;   // % soil moisture (farmer set) - renamed from dryThreshold
float waterThreshold = 10; // litres remaining before tank pump auto-start

// ---- DUAL IRRIGATION PUMP STATES ----
bool irrigationState1 = false;  // Pump 1 state
bool irrigationState2 = false;  // Pump 2 state
bool irrigationIsAuto1 = false; // Track if Pump 1 was started by auto mode
bool irrigationIsAuto2 = false; // Track if Pump 2 was started by auto mode
int irrigationDuration = 10;    // Manual duration (shared for both pumps)

bool tankPumpState = false;
bool tankPumpIsAuto = false;   // Track if tank pump was started by auto mode

// ---- NOTIFICATION FLAGS (ANTI-SPAM) ----
bool soilWarningAlertSent1 = false;  // Prevent repeated soil alerts for Pump 1
bool soilWarningAlertSent2 = false;  // Prevent repeated soil alerts for Pump 2
bool tempWarningAlertSent = false;   // Prevent repeated temperature alerts
bool humidityWarningAlertSent = false; // Prevent repeated humidity alerts
bool waterLevelAlertSent = false;    // Prevent repeated water level alerts

// ---- PUMP TIMERS (INDEPENDENT) ----
unsigned long irrigationStart1 = 0;  // Pump 1 start time
unsigned long irrigationStart2 = 0;  // Pump 2 start time
unsigned long tankPumpStart = 0;

// ---- PUMP DURATIONS (INDEPENDENT) ----
int irrigationDuration1 = 10;  // Pump 1 duration (will be set by fuzzy logic)
int irrigationDuration2 = 10;  // Pump 2 duration (will be set by fuzzy logic)

unsigned long syncStartTime = 0;

bool autoMode = false;


// Tank dimensions (mm)
const float tankHeight = 0.758;
const float tankRadius = 0.290;

// ---- DURATIONS (seconds) ----
int durationShort  = 10;
int durationMedium = 20;
int durationLong   = 30;
int tankPumpDuration = 10;

// ---- BLYNK VPINS ----
#define VPIN_TEMP V0
#define VPIN_AH V1
// #define VPIN_SOIL V2
#define VPIN_IRRIGATION V3
#define VPIN_MANUAL_DURATION V4
#define VPIN_DRY_THRESHOLD V5
#define VPIN_TANK_PUMP V6
#define VPIN_TANK_DURATION V7
#define VPIN_SOIL_STATE V8
#define VPIN_AUTO_MODE V9
#define VPIN_SHORT V10
#define VPIN_MEDIUM V11
#define VPIN_LONG V12

#define VPIN_SOIL1 V13 // For future use: additional soil sensor
#define VPIN_SOIL2 V14 // For future use: additional soil sensor

#define VPIN_WATER_LEVEL V15 // For future use: water level sensor
#define VPIN_IRRIGATION2 V16 // Pump 2 manual control


BlynkTimer timer;

// ======================================================
// NON-BLOCKING NOTIFICATION QUEUE
// ======================================================
struct NotificationItem {
  char event[40];
  char message[80];
};

const int NOTIFICATION_QUEUE_SIZE = 10;
NotificationItem notificationQueue[NOTIFICATION_QUEUE_SIZE];
int queueHead = 0;
int queueTail = 0;
unsigned long lastNotificationTime = 0;
const unsigned long NOTIFICATION_INTERVAL = 2500; // 2.5 seconds between notifications

// ======================================================
// QUEUE NOTIFICATION FUNCTIONS
// ======================================================

// Add notification to queue (non-blocking)
void queueNotification(const char* event, const char* message) {
  // Check if queue is full
  int nextTail = (queueTail + 1) % NOTIFICATION_QUEUE_SIZE;
  if (nextTail == queueHead) {
    Serial.println("⚠️ Notification queue FULL - dropping oldest notification");
    queueHead = (queueHead + 1) % NOTIFICATION_QUEUE_SIZE;
  }

  // Add to queue
  strncpy(notificationQueue[queueTail].event, event, sizeof(notificationQueue[queueTail].event) - 1);
  notificationQueue[queueTail].event[sizeof(notificationQueue[queueTail].event) - 1] = '\0';
  
  strncpy(notificationQueue[queueTail].message, message, sizeof(notificationQueue[queueTail].message) - 1);
  notificationQueue[queueTail].message[sizeof(notificationQueue[queueTail].message) - 1] = '\0';
  
  queueTail = nextTail;
  Serial.printf("📬 Notification queued: %s | %s (queue size: %d)\n", 
    event, message, (queueTail - queueHead + NOTIFICATION_QUEUE_SIZE) % NOTIFICATION_QUEUE_SIZE);
}

// Process one notification per second (non-blocking, call in loop)
void processNotifications() {
  // Check if queue is empty
  if (queueHead == queueTail) {
    return;
  }

  // Check if 1 second has passed
  unsigned long now = millis();
  if (now - lastNotificationTime < NOTIFICATION_INTERVAL) {
    return;
  }

  // Send the front notification
  Blynk.logEvent(notificationQueue[queueHead].event, notificationQueue[queueHead].message);
  Serial.printf("📤 Notification sent: %s | %s\n", 
    notificationQueue[queueHead].event, notificationQueue[queueHead].message);
  
  // Remove from queue
  queueHead = (queueHead + 1) % NOTIFICATION_QUEUE_SIZE;
  lastNotificationTime = now;
}

// ======================================================
// FIXED SOIL STATE LABELING (RULE-BASED)
// ======================================================
String getSoilState(float m) {
  if (m < 25) return "DRY";
  if (m < 40) return "OK";
  if (m < 55) return "OPTIMAL";
  if (m < 70) return "WET";
  return "TOO_WET";
}

// ======================================================
// ABSOLUTE HUMIDITY CALCULATION (g/m³)
// ======================================================
float computeAbsoluteHumidity(float tempC, float rh) {
  float svp = 6.112 * exp((17.67 * tempC) / (tempC + 243.5)); // saturation vapor pressure
  float avp = svp * (rh / 100.0);                             // actual vapor pressure
  return (216.7 * avp) / (273.15 + tempC);                    // absolute humidity in g/m³
}

// ======================================================
// ULTRASONIC DISTANCE CALCULATION
// ======================================================
float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Add timeout (VERY IMPORTANT)
  long duration = pulseIn(ECHO_PIN, HIGH, 30000); // 30ms timeout

  // If no echo received
  if (duration == 0) {
    return -1; // INVALID
  }

  float distance_cm = duration * 0.0343 / 2;
  return distance_cm / 100.0;  // meters
}

float getWaterPercent() {
  const float tankHeight = 0.9144; // 3 ft
  const float gap = 0.0762;        // 3 inches

  float distance = getDistance();

  // Handle invalid reading (no echo)
  if (distance < 0) {
    return 0; // assume empty
  }

  // Remove gap
  float waterHeight = tankHeight - (distance - gap);

  // Clamp
  if (waterHeight < 0) waterHeight = 0;
  if (waterHeight > tankHeight) waterHeight = tankHeight;

  float percent = (waterHeight / tankHeight) * 100.0;

  // REMOVE forced minimum → allow 0%
  if (percent > 100) percent = 100;

  return percent;
}

// ======================================================
// FUZZY LOGIC DECISION ENGINE (R1–R9)
// ======================================================
int decideIrrigationDuration() {

  String tempState;
  if (temperature >= 32) tempState = "HOT";
  else if (temperature >= 25) tempState = "WARM";
  else tempState = "COOL";

  String airState;
  if (absoluteHumidity < 10) airState = "DRY_AIR";
  else if (absoluteHumidity < 18) airState = "NORMAL";
  else airState = "HUMID";

  Serial.printf("🧠 Fuzzy Input → %s + %s\n",
                tempState.c_str(), airState.c_str());

  // R1–R9
  if (tempState == "HOT"  && airState == "DRY_AIR") return durationLong;
  if (tempState == "HOT"  && airState == "NORMAL")  return durationLong;
  if (tempState == "HOT"  && airState == "HUMID")   return durationMedium;

  if (tempState == "WARM" && airState == "DRY_AIR") return durationMedium;
  if (tempState == "WARM" && airState == "NORMAL")  return durationMedium;
  if (tempState == "WARM" && airState == "HUMID")   return durationShort;

  if (tempState == "COOL" && airState == "DRY_AIR") return durationMedium;
  if (tempState == "COOL" && airState == "NORMAL")  return durationShort;
  if (tempState == "COOL" && airState == "HUMID")   return durationShort;

  return durationShort;
}

// ======================================================
// SUPABASE LOGGER (EVENT-BASED)
// ======================================================
void sendToSupabase() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(SUPABASE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  json.clear(); 

  // json["avrg_soil_moisture"] = avrgSoilMoisture;
  json["soil_moisture1"] = soilMoisture1;
  json["soil_moisture2"] = soilMoisture2;
  json["soil_state"] = getSoilState(avrgSoilMoisture);
  json["temperature"] = temperature;
  json["absolute_humidity"] = absoluteHumidity;
  json["irrigation_pump1_state"] = irrigationState1;
  json["irrigation_pump2_state"] = irrigationState2;
  json["tank_pump_state"] = tankPumpState;
  json["irrigation1_duration"] = irrigationDuration1;
  json["irrigation2_duration"] = irrigationDuration2;
  json["soil_warning_threshold"] = soilWarningThreshold;
  json["auto_mode"] = autoMode;
  json["short_duration"] = durationShort;
  json["medium_duration"] = durationMedium;
  json["long_duration"] = durationLong;
  json["water_level"] = waterPercent;

  String body;
  body.reserve(512);
  serializeJson(json, body);

  int code = http.POST(body);
  Serial.printf("Supabase response: %d\n", code);

  http.end();
}

// ======================================================
// READ SENSORS (EVERY 30 SECONDS)
// ======================================================
void readSensors() {

  temperature = dht.readTemperature();
  humidity = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("⚠️ DHT read failed");
    return;
  }

  absoluteHumidity = computeAbsoluteHumidity(temperature, humidity);

  waterPercent = getWaterPercent();

  int raw1 = analogRead(SOIL_PIN1);
  int raw2 = analogRead(SOIL_PIN2);
  soilMoisture1 = map(raw1, 3010, 1100, 0, 100);
  soilMoisture2 = map(raw2, 2960, 1100, 0, 100);
  soilMoisture1 = constrain(soilMoisture1, 0, 100);
  soilMoisture2 = constrain(soilMoisture2, 0, 100);
  avrgSoilMoisture = (soilMoisture1 + soilMoisture2) / 2.0;

  String soilState = getSoilState(avrgSoilMoisture);

  Serial.printf(
  "Hum %.1f%% | Temp %.1fC | AH %.2f | "
  "Raw1 %d Raw2 %d | Soil1 %.1f%% Soil2 %.1f%% Avg %.1f%% | State %s | Tank %.1f L\n",
  humidity, temperature, absoluteHumidity,
  raw1, raw2,
  soilMoisture1, soilMoisture2, avrgSoilMoisture,
  soilState.c_str(), waterPercent
  );

  Blynk.virtualWrite(VPIN_TEMP, temperature);
  Blynk.virtualWrite(VPIN_AH, absoluteHumidity);
  // Blynk.virtualWrite(VPIN_SOIL, avrgSoilMoisture);
  Blynk.virtualWrite(VPIN_SOIL1, soilMoisture1);
  Blynk.virtualWrite(VPIN_SOIL2, soilMoisture2);
  Blynk.virtualWrite(VPIN_SOIL_STATE, soilState);
  Blynk.virtualWrite(VPIN_WATER_LEVEL, waterPercent);

  // ---- SENSOR WARNINGS (CONSOLIDATED) ----

  // Soil 1 dry warning
  if (!soilWarningAlertSent1 && soilMoisture1 <= soilWarningThreshold) {
    soilWarningAlertSent1 = true;
    queueNotification("sensor_warning", "Soil 1 moisture is low");
    Serial.println("⚠️ Soil 1 low alert sent");
  } else if (soilWarningAlertSent1 && soilMoisture1 > (soilWarningThreshold + 10)) {
    soilWarningAlertSent1 = false;  // Reset when soil recovers
  }

  // Soil 2 dry warning
  if (!soilWarningAlertSent2 && soilMoisture2 <= soilWarningThreshold) {
    soilWarningAlertSent2 = true;
    queueNotification("sensor_warning", "Soil 2 moisture is low");
    Serial.println("⚠️ Soil 2 low alert sent");
  } else if (soilWarningAlertSent2 && soilMoisture2 > (soilWarningThreshold + 10)) {
    soilWarningAlertSent2 = false;  // Reset when soil recovers
  }

  // Temperature warning (too hot)
  if (!tempWarningAlertSent && temperature >= 32) {
    tempWarningAlertSent = true;
    queueNotification("sensor_warning", "Temperature too high");
    Serial.println("⚠️ High temperature alert sent");
  } else if (tempWarningAlertSent && temperature < 30) {
    tempWarningAlertSent = false;  // Reset when temp recovers
  }

  // Humidity warning (out of range)
  if (!humidityWarningAlertSent && (absoluteHumidity < 5 || absoluteHumidity > 25)) {
    humidityWarningAlertSent = true;
    String msg = absoluteHumidity < 5 ? "Humidity too low" : "Humidity too high";
    queueNotification("sensor_warning", msg.c_str());
    Serial.println("⚠️ Humidity alert sent");
  } else if (humidityWarningAlertSent && (absoluteHumidity >= 5 && absoluteHumidity <= 25)) {
    humidityWarningAlertSent = false;  // Reset when humidity recovers
  }

  // Water level warning
  if (!waterLevelAlertSent && waterPercent <= waterThreshold) {
    waterLevelAlertSent = true;
    queueNotification("sensor_warning", "Water level below threshold");
    Serial.println("⚠️ Water level alert sent");
  } else if (waterLevelAlertSent && waterPercent > (waterThreshold + 5)) {
    waterLevelAlertSent = false;  // Reset when level recovers
  }

  // ---- INDEPENDENT AUTO MODE IRRIGATION (PUMP 1) ----
  if (autoMode && !irrigationState1 && soilMoisture1 <= soilWarningThreshold && waterPercent > waterThreshold) {
    irrigationDuration1 = decideIrrigationDuration();
    irrigationState1 = true;
    irrigationIsAuto1 = true;
    irrigationStart1 = millis();

    digitalWrite(IRRIGATION_PUMP_PIN1, HIGH);
    Blynk.virtualWrite(VPIN_IRRIGATION, 1);

    queueNotification("irrigation_auto", "Pump 1 auto irrigation started");

    Serial.println("🌱 Pump 1 auto irrigation started");
    sendToSupabase();
  }

  // ---- INDEPENDENT AUTO MODE IRRIGATION (PUMP 2) ----
  if (autoMode && !irrigationState2 && soilMoisture2 <= soilWarningThreshold && waterPercent > waterThreshold) {
    irrigationDuration2 = decideIrrigationDuration();
    irrigationState2 = true;
    irrigationIsAuto2 = true;
    irrigationStart2 = millis();

    digitalWrite(IRRIGATION_PUMP_PIN2, HIGH);
    Blynk.virtualWrite(VPIN_IRRIGATION2, 1);

    queueNotification("irrigation_auto", "Pump 2 auto irrigation started");

    Serial.println("🌱 Pump 2 auto irrigation started");
    sendToSupabase();
  }

  // ---- AUTO MODE TANK PUMP ----
  // if (autoMode && !tankPumpState && waterPercent <= waterThreshold) {
  //   tankPumpState = true;
  //   tankPumpIsAuto = true;
  //   tankPumpStart = millis();

  //   digitalWrite(TANK_PUMP_PIN, HIGH);
  //   Blynk.virtualWrite(VPIN_TANK_PUMP, 1);

  //   Blynk.logEvent("sensor_warning", "Water level too low - tank pump started");

  //   Serial.println("🚰 Tank pump auto started");
  //   sendToSupabase();
  // }
}

// ======================================================
// BLYNK HANDLERS
// ======================================================

// Called every time the hardware connects (or reconnects) to the Blynk
// server.  Use this opportunity to pull down any virtual-pin values that may
// have been changed via the mobile app while the device was offline.
BLYNK_CONNECTED() {
  Blynk.syncAll();
  syncStartTime = millis();
}

BLYNK_WRITE(VPIN_IRRIGATION) {
  if (isSyncing) return;
  if (autoMode) return;  // Ignore manual control in auto mode

  bool turnOn = param.asInt();
  bool wasOn = irrigationState1;
  
  // Control PUMP 1 only
  irrigationState1 = turnOn;
  irrigationIsAuto1 = false;  // Mark as manual
  digitalWrite(IRRIGATION_PUMP_PIN1, turnOn ? HIGH : LOW);

  if (turnOn) {
    irrigationStart1 = millis();
    queueNotification("irrigation_manual", "Manual Pump 1 ON");
    Serial.println("💧 Manual: Pump 1 started");
  } else if (wasOn) {
    queueNotification("irrigation_manual", "Manual Pump 1 OFF");
    Serial.println("⏹️ Manual: Pump 1 stopped");
  }
  sendToSupabase();
}

BLYNK_WRITE(VPIN_IRRIGATION2) {
  if (isSyncing) return;
  if (autoMode) return;  // Ignore manual control in auto mode

  bool turnOn = param.asInt();
  bool wasOn = irrigationState2;
  
  // Control PUMP 2 only
  irrigationState2 = turnOn;
  irrigationIsAuto2 = false;  // Mark as manual
  digitalWrite(IRRIGATION_PUMP_PIN2, turnOn ? HIGH : LOW);

  if (turnOn) {
    irrigationStart2 = millis();
    queueNotification("irrigation_manual", "Manual Pump 2 ON");
    Serial.println("💧 Manual: Pump 2 started");
  } else if (wasOn) {
    queueNotification("irrigation_manual", "Manual Pump 2 OFF");
    Serial.println("⏹️ Manual: Pump 2 stopped");
  }
  sendToSupabase();
}


BLYNK_WRITE(VPIN_AUTO_MODE) {
  if (isSyncing) {  
    autoMode = param.asInt();  // still update state
    return;
  }
  autoMode = param.asInt();

  // Safety: turn off pumps when auto mode is disabled
  if (!autoMode) {
    if (irrigationState1) {
      irrigationState1 = false;
      digitalWrite(IRRIGATION_PUMP_PIN1, LOW);
      Serial.println("⛔ Auto mode OFF → irrigation pump 1 stopped");
    }
    if (irrigationState2) {
      irrigationState2 = false;
      digitalWrite(IRRIGATION_PUMP_PIN2, LOW);
      Serial.println("⛔ Auto mode OFF → irrigation pump 2 stopped");
    }
    // if (tankPumpState) {
    //   tankPumpState = false;
    //   digitalWrite(TANK_PUMP_PIN, LOW);
    //   Serial.println("⛔ Auto mode OFF → tank pump stopped");
    // }
    // Blynk.virtualWrite(VPIN_IRRIGATION, 0);
    // Blynk.virtualWrite(VPIN_TANK_PUMP, 0);
    sendToSupabase();
  }
}

BLYNK_WRITE(VPIN_DRY_THRESHOLD)  { soilWarningThreshold = param.asFloat(); }
BLYNK_WRITE(VPIN_MANUAL_DURATION) { irrigationDuration = param.asInt(); }
BLYNK_WRITE(VPIN_SHORT)  { durationShort  = param.asInt(); }
BLYNK_WRITE(VPIN_MEDIUM) { durationMedium = param.asInt(); }
BLYNK_WRITE(VPIN_LONG)   { durationLong   = param.asInt(); }

// BLYNK_WRITE(VPIN_TANK_PUMP) {
//   if (isSyncing) return;
//   if (autoMode) return;

//   bool wasOn = tankPumpState;
//   tankPumpState = param.asInt();
//   tankPumpIsAuto = false;  // Mark as manual
//   digitalWrite(TANK_PUMP_PIN, tankPumpState ? HIGH : LOW);

//   if (tankPumpState) {
//     tankPumpStart = millis();
//     Blynk.logEvent("sensor_warning", "Manual: tank pump ON");
//     Serial.println("💧 Manual tank pump started");
//   } else if (wasOn) {
//     Blynk.logEvent("sensor_warning", "Manual: tank pump OFF");
//     Serial.println("⏹️ Manual tank pump stopped");
//   }
//   sendToSupabase();
// }

BLYNK_WRITE(VPIN_TANK_DURATION) {
  tankPumpDuration = param.asInt();
}

// ======================================================
// CHECK PUMPS (EVERY LOOP)
// ======================================================
void checkPumps() {

  // Stop irrigation pump 1 after duration (AUTO MODE: uses fuzzy duration, MANUAL MODE: uses manual duration)
  if (irrigationState1) {
    unsigned long elapsedTime = millis() - irrigationStart1;
    unsigned long maxDuration = irrigationIsAuto1 ? (unsigned long)irrigationDuration1 * 1000UL : (unsigned long)irrigationDuration * 1000UL;
    
    if (elapsedTime >= maxDuration) {
      irrigationState1 = false;
      digitalWrite(IRRIGATION_PUMP_PIN1, LOW);
      
      if (irrigationIsAuto1) {
        queueNotification("irrigation_auto", "Pump 1 auto irrigation timer finished");
        Serial.println("⏹️ Pump 1 auto irrigation stopped (timer)");
      } else {
        queueNotification("irrigation_manual", "Pump 1 manual irrigation timer finished");
        Serial.println("⏹️ Pump 1 manual irrigation stopped (timer)");
      }
      
      // Update UI only if both pumps are off
      if (!irrigationState2) {
        Blynk.virtualWrite(VPIN_IRRIGATION, 0);
        Blynk.virtualWrite(VPIN_IRRIGATION2, 0);
      }
      
      sendToSupabase();
    }
  }

  // Stop irrigation pump 2 after duration (AUTO MODE: uses fuzzy duration, MANUAL MODE: uses manual duration)
  if (irrigationState2) {
    unsigned long elapsedTime = millis() - irrigationStart2;
    unsigned long maxDuration = irrigationIsAuto2 ? (unsigned long)irrigationDuration2 * 1000UL : (unsigned long)irrigationDuration * 1000UL;
    
    if (elapsedTime >= maxDuration) {
      irrigationState2 = false;
      digitalWrite(IRRIGATION_PUMP_PIN2, LOW);
      
      if (irrigationIsAuto2) {
        queueNotification("irrigation_auto", "Pump 2 auto irrigation timer finished");
        Serial.println("⏹️ Pump 2 auto irrigation stopped (timer)");
      } else {
        queueNotification("irrigation_manual", "Pump 2 manual irrigation timer finished");
        Serial.println("⏹️ Pump 2 manual irrigation stopped (timer)");
      }
      
      // Update UI only if both pumps are off
      if (!irrigationState1) {
        Blynk.virtualWrite(VPIN_IRRIGATION, 0);
        Blynk.virtualWrite(VPIN_IRRIGATION2, 0);
      }
      
      sendToSupabase();
    }
  }

  // Stop tank pump after duration
  // if (tankPumpState &&
  //     millis() - tankPumpStart >= tankPumpDuration * 1000UL) {

  //   tankPumpState = false;
  //   digitalWrite(TANK_PUMP_PIN, LOW);
  //   Blynk.virtualWrite(VPIN_TANK_PUMP, 0);

  //   if (tankPumpIsAuto) {
  //     Blynk.logEvent("sensor_warning", "Tank pump auto timer finished");
  //     Serial.println("⏹️ Tank pump auto stopped (timer)");
  //   } else {
  //     Blynk.logEvent("sensor_warning", "Tank pump manual timer finished");
  //     Serial.println("⏹️ Tank pump manual stopped (timer)");
  //   }
    
  //   sendToSupabase();
  // }
}

// ======================================================
// SETUP
// ======================================================
void setup() {
  Serial.begin(9600);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(IRRIGATION_PUMP_PIN1, OUTPUT);
  pinMode(IRRIGATION_PUMP_PIN2, OUTPUT);
  // pinMode(TANK_PUMP_PIN, OUTPUT);
  digitalWrite(IRRIGATION_PUMP_PIN1, LOW);
  digitalWrite(IRRIGATION_PUMP_PIN2, LOW);
  // digitalWrite(TANK_PUMP_PIN, LOW);

  dht.begin();

  WiFiManager wm;
  wm.autoConnect("Greenhouse_Config");

  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();

  timer.setInterval(30000L, readSensors); // 30 seconds
}

// ======================================================
// LOOP
// ======================================================
void loop() {

  if (isSyncing && millis() - syncStartTime > 3000) {
    isSyncing = false;
    Serial.println("✅ Sync complete");
  }

  Blynk.run();
  timer.run();
  checkPumps();
  processNotifications();  // Process one notification per second (non-blocking)
}