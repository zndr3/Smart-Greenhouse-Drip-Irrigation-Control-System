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
#define IRRIGATION_PUMP_PIN 14
#define TANK_PUMP_PIN 27
#define TRIG_PIN 5
#define ECHO_PIN 18


// ---- VARIABLES ----
float temperature = 0;
float humidity = 0;
float absoluteHumidity = 0;
float soilMoisture1 = 0;
float soilMoisture2 = 0;
float avrgSoilMoisture = 0;
float waterPercent = 0;

float dryThreshold = 25;   // % soil moisture (farmer set)
float waterThreshold = 10; // litres remaining before tank pump auto-start

bool irrigationState = false;
bool tankPumpState = false;
bool autoMode = false;
bool irrigationIsAuto = false;  // Track if irrigation was started by auto mode
bool tankPumpIsAuto = false;   // Track if tank pump was started by auto mode
bool waterLevelAlertSent = false;  // Prevent repeated alerts for same threshold

unsigned long irrigationStart = 0;
unsigned long tankPumpStart = 0;


// Tank dimensions (mm)
const float tankHeight = 0.758;
const float tankRadius = 0.290;

// ---- DURATIONS (seconds) ----
int durationShort  = 10;
int durationMedium = 20;
int durationLong   = 30;
int irrigationDuration = 10;
int tankPumpDuration = 10;

// ---- BLYNK VPINS ----
#define VPIN_TEMP V0
#define VPIN_AH V1
#define VPIN_SOIL V2
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


BlynkTimer timer;

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

  long duration = pulseIn(ECHO_PIN, HIGH);

  float distance_cm = duration * 0.0343 / 2;  
  return distance_cm / 100.0;  // convert to meters
}

float getWaterPercent() {
  // Tank height in meters
  const float tankHeight = 0.9144; // 914.4 mm = 3 ft
  const float gap = 0.0762;        // 3 inches = 76.2 mm

  float distance = getDistance();  // distance from sensor to water surface

  // Subtract the gap to get actual water height
  float waterHeight = tankHeight - (distance - gap);

  // Clamp water height
  if (waterHeight < 0) waterHeight = 0;
  if (waterHeight > tankHeight) waterHeight = tankHeight;

  float percent = (waterHeight / tankHeight) * 100.0;

  // Optional safety limits
  if (percent < 1) percent = 1;
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

  json["avrg_soil_moisture"] = avrgSoilMoisture;
  json["soil_moisture1"] = soilMoisture1;
  json["soil_moisture2"] = soilMoisture2;
  json["soil_state"] = getSoilState(avrgSoilMoisture);
  json["temperature"] = temperature;
  json["absolute_humidity"] = absoluteHumidity;
  json["irrigation_pump_state"] = irrigationState;
  json["tank_pump_state"] = tankPumpState;
  json["irrigation_duration"] = irrigationDuration;
  json["dry_threshold"] = dryThreshold;
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
// READ SENSORS (EVERY 2 MINUTES)
// ======================================================
void readSensors() {

  temperature = dht.readTemperature();
  humidity = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("⚠️ DHT read failed");
    return;
  }

  absoluteHumidity = computeAbsoluteHumidity(temperature, humidity);

  float waterPercent = getWaterPercent();

  int raw1 = analogRead(SOIL_PIN1);
  int raw2 = analogRead(SOIL_PIN2);
  soilMoisture1 = map(raw1, 3500, 1200, 0, 100);
  soilMoisture2 = map(raw2, 3500, 1200, 0, 100);
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
  Blynk.virtualWrite(VPIN_SOIL, avrgSoilMoisture);
  Blynk.virtualWrite(VPIN_SOIL1, soilMoisture1);
  Blynk.virtualWrite(VPIN_SOIL2, soilMoisture2);
  Blynk.virtualWrite(VPIN_SOIL_STATE, soilState);
  Blynk.virtualWrite(VPIN_WATER_LEVEL, waterPercent);

  // ---- AUTO MODE IRRIGATION ----
  if (autoMode && !irrigationState && avrgSoilMoisture <= dryThreshold && waterPercent > waterThreshold) {

    irrigationDuration = decideIrrigationDuration();
    irrigationState = true;
    irrigationIsAuto = true;
    irrigationStart = millis();

    digitalWrite(IRRIGATION_PUMP_PIN, HIGH);
    Blynk.virtualWrite(VPIN_IRRIGATION, 1);

    Blynk.logEvent("irrigation_auto",
      "Auto irrigation activated");

    Serial.println("Auto irrigation started");
    sendToSupabase();
  }

  // ---- AUTO MODE TANK PUMP ----
  if (autoMode && !tankPumpState && waterPercent <= waterThreshold) {
    // turn pump on for configured duration
    tankPumpState = true;
    tankPumpIsAuto = true;
    tankPumpStart = millis();

    digitalWrite(TANK_PUMP_PIN, HIGH);
    Blynk.virtualWrite(VPIN_TANK_PUMP, 1);

    Blynk.logEvent("tank_auto",
      "Auto tank pump activated");

    Serial.println("Tank pump auto started");
    sendToSupabase();
  }

  // ---- WATER LEVEL LOW THRESHOLD ALERT ----
  if (!waterLevelAlertSent && waterPercent <= waterThreshold) {
    waterLevelAlertSent = true;
    Blynk.logEvent("sensor_threshold",
      "Water level below threshold");
    Serial.println("⚠️ Water level alert sent");
  } else if (waterLevelAlertSent && waterPercent > (waterThreshold + 5)) {
    waterLevelAlertSent = false;  // Reset when level recovers
  }
}

// ======================================================
// BLYNK HANDLERS
// ======================================================

// Called every time the hardware connects (or reconnects) to the Blynk
// server.  Use this opportunity to pull down any virtual-pin values that may
// have been changed via the mobile app while the device was offline.
BLYNK_CONNECTED() {
  Blynk.syncAll();

}

BLYNK_WRITE(VPIN_IRRIGATION) {
  if (autoMode) return;  // Ignore manual control in auto mode

  bool wasOn = irrigationState;
  irrigationState = param.asInt();
  irrigationIsAuto = false;  // Mark as manual
  digitalWrite(IRRIGATION_PUMP_PIN, irrigationState ? HIGH : LOW);

  if (irrigationState) {
    irrigationStart = millis();
    Blynk.logEvent("irrigation_manual", "Manual irrigation started");
  } else if (wasOn) {
    Blynk.logEvent("irrigation_manual", "Manual irrigation stopped");
  }
  sendToSupabase();
}


BLYNK_WRITE(VPIN_AUTO_MODE) {
  autoMode = param.asInt();

  // Safety: turn off pumps when auto mode is disabled
  if (!autoMode) {
    if (irrigationState) {
      irrigationState = false;
      digitalWrite(IRRIGATION_PUMP_PIN, LOW);
      Blynk.virtualWrite(VPIN_IRRIGATION, 0);
      Serial.println("⛔ Auto mode OFF → irrigation stopped");
    }
    if (tankPumpState) {
      tankPumpState = false;
      digitalWrite(TANK_PUMP_PIN, LOW);
      Blynk.virtualWrite(VPIN_TANK_PUMP, 0);
      Serial.println("⛔ Auto mode OFF → tank pump stopped");
    }
    sendToSupabase();
  }
}

BLYNK_WRITE(VPIN_DRY_THRESHOLD) { dryThreshold = param.asFloat(); }
BLYNK_WRITE(VPIN_SHORT)  { durationShort  = param.asInt(); }
BLYNK_WRITE(VPIN_MEDIUM) { durationMedium = param.asInt(); }
BLYNK_WRITE(VPIN_LONG)   { durationLong   = param.asInt(); }
BLYNK_WRITE(VPIN_MANUAL_DURATION) { irrigationDuration = param.asInt(); }

BLYNK_WRITE(VPIN_TANK_PUMP) {
  bool wasOn = tankPumpState;
  tankPumpState = param.asInt();
  tankPumpIsAuto = false;  // Mark as manual
  digitalWrite(TANK_PUMP_PIN, tankPumpState ? HIGH : LOW);

  if (tankPumpState) {
    tankPumpStart = millis();
    Blynk.logEvent("tank_manual", "Manual tank pump started");
  } else if (wasOn) {
    Blynk.logEvent("tank_manual", "Manual tank pump stopped");
  }
  sendToSupabase();
}

BLYNK_WRITE(VPIN_TANK_DURATION) {
  tankPumpDuration = param.asInt();
}

// ======================================================
// CHECK PUMPS (EVERY LOOP)
// ======================================================
void checkPumps() {

  // Stop irrigation after duration
  if (irrigationState &&
      millis() - irrigationStart >= irrigationDuration * 1000UL) {

    irrigationState = false;
    
    digitalWrite(IRRIGATION_PUMP_PIN, LOW);
    Blynk.virtualWrite(VPIN_IRRIGATION, 0);

    if (irrigationIsAuto) {
      Blynk.logEvent("irrigation_auto", "Auto irrigation stopped (timer)");
    } else {
      Blynk.logEvent("irrigation_manual", "Manual irrigation stopped (timer)");
    }
    Serial.println("⏹️ Irrigation stopped");
    sendToSupabase();
  }

  // Stop tank pump after duration
  if (tankPumpState &&
      millis() - tankPumpStart >= tankPumpDuration * 1000UL) {

    tankPumpState = false;
    digitalWrite(TANK_PUMP_PIN, LOW);
    Blynk.virtualWrite(VPIN_TANK_PUMP, 0);

    if (tankPumpIsAuto) {
      Blynk.logEvent("tank_auto", "Auto tank pump stopped (timer)");
    } else {
      Blynk.logEvent("tank_manual", "Manual tank pump stopped (timer)");
    }
    Serial.println("⏹️ Tank pump stopped");
    sendToSupabase();
  }
}

// ======================================================
// SETUP
// ======================================================
void setup() {
  Serial.begin(9600);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(IRRIGATION_PUMP_PIN, OUTPUT);
  pinMode(TANK_PUMP_PIN, OUTPUT);
  digitalWrite(IRRIGATION_PUMP_PIN, LOW);
  digitalWrite(TANK_PUMP_PIN, LOW);

  dht.begin();

  WiFiManager wm;
  wm.autoConnect("Greenhouse_Config");

  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();


  timer.setInterval(120000L, readSensors); // 2 minutes
}

// ======================================================
// LOOP
// ======================================================
void loop() {
  Blynk.run();
  timer.run();
  checkPumps();
  
}
 