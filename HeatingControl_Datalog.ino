/*
 * BRAE-332: Heat Control System with Data Logging
 * ================================================
 * This program controls a heating system using an active-low relay and logs
 * temperature/humidity data from an SHTC3 sensor to an SD card via CSV.
 * 
 * Hardware:
 * - Arduino microcontroller
 * - DS3231 Real-Time Clock (RTC) via I2C
 * - SHTC3 Temperature/Humidity Sensor via I2C
 * - SD Card Module (CS pin 10, SPI interface)
 * - Relay module (active low: LOW = ON, HIGH = OFF)
 * 
 * Cycle Operation:
 * 1. Check current temperature against setpoint
 * 2. If below setpoint, activate relay for HTIME duration
 * 3. Wait for WAIT_AFTER duration (cooling/rest period)
 * 4. Log all actions and sensor readings to SD card
 * 5. Repeat cycle every LOG_INTERVAL
 */

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include "RTClib.h"                // DS3231 RTC library
#include "SparkFun_SHTC3.h"        // More memory-efficient SHTC3 library

// ========== PIN DEFINITIONS ==========
#define CS_PIN 10                  // SD card chip select pin (from Datalogger branch)
#define RELAY_PIN 7                // TODO: CHANGE THIS PIN - Active low relay control pin -- changed to pin 7
                                   // (HIGH = relay OFF, LOW = relay ON)

// ========== SYSTEM PARAMETERS ==========
// Temperature setpoint: system will heat when temperature drops below this value
const float SETPOINT = 25.0;      // Target temperature (°C)

// Heating control timings
const long HTIME = 30000;         // Heating duration per cycle (30 seconds in milliseconds)
const long WAIT_AFTER = 120000;   // Rest/cooling duration after heating (2 minutes in milliseconds)

// Data logging interval: total time between each cycle start
const long LOG_INTERVAL = 300000; // Total cycle interval (5 minutes in milliseconds)

// Sensor error tracking
int sensor_error_count = 0;       // Counter for consecutive sensor read failures
const int MAX_ERROR_ATTEMPTS = 3; // Maximum consecutive errors before logging failure

// ========== PERIPHERAL OBJECT INITIALIZATION ==========
RTC_DS3231 rtc;                   // Real-Time Clock object
SHTC3 shtc3;                      // SHTC3 sensor object (SparkFun library)
File dataFile;                    // SD card file object for data logging

// ========== SETUP FUNCTION ==========
// Runs once when Arduino powered on or reset
void setup() {
  // Initialize serial communication for debugging output
  Serial.begin(4800);  // Reduced baud rate to save memory
  
  // Initialize I2C communication for RTC and SHTC3 sensors
  Wire.begin();

  // Initialize relay pin as output and set to HIGH (relay OFF - active low)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);  // Ensure relay is OFF at startup

  // ========== INITIALIZE RTC ==========
  if (!rtc.begin()) {
    Serial.println(F("RTC fail"));
    while (1);
  }
  Serial.println(F("RTC OK"));

  // ========== INITIALIZE SD CARD ==========
  if (!SD.begin(CS_PIN)) {
    Serial.println(F("SD fail"));
    while (1);
  }
  Serial.println(F("SD OK"));

  // ========== INITIALIZE SHTC3 SENSOR ==========
  if (shtc3.begin() != SHTC3_Status_Nominal) {
    Serial.println(F("SHTC3 fail"));
    while (1);
  }
  Serial.println(F("SHTC3 OK"));

  // ========== CREATE CSV HEADER ==========
  // Check if datalog file already exists; if not, create it with header row
  if (!SD.exists("datalog.csv")) {
    dataFile = SD.open("datalog.csv", FILE_WRITE);
    if (dataFile) {
      dataFile.println(F("Hour:Minute:Second,Temperature(C),Humidity(%),Action"));
      dataFile.close();
      Serial.println(F("CSV created"));
    } else {
      Serial.println(F("CSV error"));
      while (1);
    }
  } else {
    Serial.println(F("Using existing CSV"));
  }

  Serial.println(F("Setup complete."));
}

// ========== MAIN LOOP ==========
// Runs repeatedly at each cycle interval
void loop() {
  // Get current date/time from RTC
  DateTime now = rtc.now();

  // ========== SENSOR READING ==========
  // Attempt to read temperature and humidity from SHTC3 sensor
  if (shtc3.update() != SHTC3_Status_Nominal) {
    sensor_error_count++;
    Serial.print(F("Sensor error "));
    Serial.println(sensor_error_count);
    
    // Log sensor failure to CSV
    logToSD(now, NAN, NAN, "ERROR");
    
    // If sensor fails too many times, halt the system
    if (sensor_error_count >= MAX_ERROR_ATTEMPTS) {
      Serial.println(F("Sensor fail - paused"));
      delay(LOG_INTERVAL);
      sensor_error_count = 0;
      return;
    }
    return;
  }

  float temperature = shtc3.toDegC();
  float humidity_value = shtc3.toPercent();

  // Reset error counter on successful read
  sensor_error_count = 0;

  // ========== HEATING DECISION LOGIC ==========
  // Check if current temperature is below setpoint
  if (temperature < SETPOINT) {
    Serial.print(F("Heat ON: "));
    Serial.println(temperature);

    // Turn on relay (LOW = active)
    digitalWrite(RELAY_PIN, LOW);
    delay(HTIME);

    // Turn off relay after heating time (HIGH = inactive)
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println(F("Heat OFF"));

    // Log heating action
    logToSD(now, temperature, humidity_value, "HEAT_ON");

    // Wait for cooling/rest period
    delay(WAIT_AFTER);

  } else {
    // Temperature is above setpoint, no heating needed
    Serial.print(F("OK: "));
    Serial.println(temperature);
    
    // Log that heating was not needed
    logToSD(now, temperature, humidity_value, "OK");
  }

  // ========== CYCLE COMPLETION ==========
  Serial.println(F("Cycle done"));
  delay(LOG_INTERVAL);
}

// ========== DATA LOGGING FUNCTION ==========
/**
 * logToSD()
 * ---------
 * Writes a row to the SD card CSV file with timestamp, sensor values, and action.
 * 
 * Parameters:
 *   DateTime n  - Current date/time from RTC
 *   float t     - Temperature reading (°C) or NAN if sensor error
 *   float h     - Humidity reading (%) or NAN if sensor error
 *   const char* act  - Description of action taken (e.g., "HEATING_ACTIVE", "SENSOR_ERROR")
 */
void logToSD(DateTime n, float t, float h, const char* act) {
  // Attempt to open CSV file in append mode
  dataFile = SD.open("datalog.csv", FILE_WRITE);

  if (dataFile) {
    // ========== WRITE TIMESTAMP ==========
    // Format: HH:MM:SS
    dataFile.print(n.hour());
    dataFile.print(":");
    // Pad single-digit minutes with leading zero
    if (n.minute() < 10) dataFile.print("0");
    dataFile.print(n.minute());
    dataFile.print(":");
    // Pad single-digit seconds with leading zero
    if (n.second() < 10) dataFile.print("0");
    dataFile.print(n.second());
    dataFile.print(",");

    // ========== WRITE SENSOR VALUES ==========
    // Temperature (handle NAN from sensor errors)
    if (isnan(t)) {
      dataFile.print("ERROR");
    } else {
      dataFile.print(t);
    }
    dataFile.print(",");

    // Humidity (handle NAN from sensor errors)
    if (isnan(h)) {
      dataFile.print("ERROR");
    } else {
      dataFile.print(h);
    }
    dataFile.print(",");

    // ========== WRITE ACTION LOG ==========
    dataFile.println(act);

    // Close file to ensure data is written to SD card
    dataFile.close();

    // ========== SERIAL OUTPUT FOR DEBUGGING ==========
    // Mirror the logged data to Serial monitor for live monitoring
    Serial.print(F("LOG: "));
    if (!isnan(t)) {
      Serial.print(t);
      Serial.print(F("C "));
    }
    Serial.println(act);

  } else {
    // If file open fails, report error via serial
    Serial.println(F("Log error"));
  }
}
