
# Smart Greenhouse Drip Irrigation Control System 🌱

## ESP32 + Fuzzy Logic + Blynk + Supabase
![Platform](https://img.shields.io/badge/Platform-ESP32-blue)
![Language](https://img.shields.io/badge/Language-C%2B%2B-green)
![Framework](https://img.shields.io/badge/Framework-Arduino-orange)
![Cloud](https://img.shields.io/badge/Cloud-Supabase-purple)
![IoT](https://img.shields.io/badge/IoT-Blynk-red)


---
### Overview

- This project is an IoT-based Smart Greenhouse Monitoring and Irrigation System designed to automate irrigation and water tank management using ESP32, environmental sensors, fuzzy logic decision-making, cloud logging, and mobile monitoring. 


<table border="0">
  <tr>
    <td align="center">
      <sub><b>Figure 1:</b> System Architecture</sub>
      <br>
      <br>
      <img src="/images/BLYNK (3).png" width="400" alt="Description of first image">
    </td>
    <td align="center">
      <sub><b>Figure 2:</b> Blynk Dashboard</sub>
      <br>
      <img src="/images/dashboard2.png" width="250" alt="Description of second image">
    </td>
  </tr>
</table>


### 🌡️The system continuously measures:
- Soil moisture (2 sensors)
- Temperature
- Humidity
- Water tank level

### 💧 Based on sensor readings, it automatically controls:
- Irrigation pump
- Tank refill pump

### 🤖 The system supports both:
- Manual mode (mobile app controlled via Blynk)
- Automatic mode (fuzzy logic controlled)
  - Irrigation Pump Activates when:
    - Soil moisture ≤ dry threshold
    - Water tank level > minimum threshold
   
  - Water Tank Pump Activates when:
    - Water tank level ≤ minimum threshold


### ✨ Features 
- Soil Sensor Monitoring
- Temperature and humidity monitoring using DHT22
- Ultrasonic water level detection
- Auto Irrigation Control
  
### 🧠 Fuzzy Irrigation Rules

TEMPERATURE | AIR HUMIDITY
------|-----
HOT  (≥ 32°C) | DRY_AIR  (< 10 g/m³)
WARM (25–31°C) | NORMAL (10–17 g/m³)
COOL (< 25°C) | HUMID (≥ 18 g/m³)

|       Temp	| Air	| Output Duration |
|-----------|------------|------------|
|HOT	| DRY	| Long |
HOT	| NORMAL | Long
HOT	| HUMID	| Medium
WARM |	DRY	| Medium
WARM	| NORMAL	| Medium
WARM	| HUMID	| Short
COOL	| DRY |	Medium
COOL	| NORMAL	| Short
COOL	| HUMID |	Short

### ☁️ Cloud Integration
- Sensor and pump events logged to Supabase:
  - Soil moisture values
  - Temperature
  - Absolute humidity
  - Pump states
  - Water level
  - Irrigation duration
  - Threshold settings

- Mobile Monitoring with Blynk, Real-time dashboard control and monitoring:
  - View sensor values
  - Enable/disable auto mode
  - Manual pump control
  - Set thresholds
  - Configure durations

### 🔧 Hardware Used
#### Microcontroller
  - ESP32
#### Sensors
  - DHT22 Temperature & Humidity Sensor
  - 2x Capacitive Soil Moisture Sensors
  - HC-SR04 Ultrasonic Sensor

#### Actuators
  - Irrigation Pump
  - Water Tank Pump

#### 💻Software Stack
- Embedded Development
  - PlatformIO
  - VS Code
  - Arduino Framework

- Cloud
  - Supabase REST API

- IoT Dashboard
  - Blynk
  - WiFi Provisioning
  - WiFiManager

Example:
  HOT + DRY_AIR → Long irrigation
  WARM + NORMAL → Medium irrigation
  COOL + HUMID → Short irrigation

### 📌 Pin Configuration
- Sensors
  - DHT22 → GPIO 4
  - Soil Sensor 1 → GPIO 34
  - Soil Sensor 2 → GPIO 35
  - Ultrasonic TRIG → GPIO 5
  - Ultrasonic ECHO → GPIO 18
  
- Pumps
  - Irrigation Pump → GPIO 14
  - Tank Pump → GPIO 27
 
    
