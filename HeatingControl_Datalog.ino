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
#include "Adafruit_SHTC3.h"        // SHTC3 temperature/humidity sensor library

// ========== PIN DEFINITIONS ==========
#define CS_PIN 10                  // SD card chip select pin (from Datalogger branch)
#define RELAY_PIN 7                // TODO: CHANGE THIS PIN - Active low relay control pin -- changed to pin 7
                                   // (HIGH = relay OFF, LOW = relay ON)

// ========== SYSTEM PARAMETERS ==========
// Temperature setpoint: system will heat when temperature drops below this value
float SETPOINT = 25.0;            // Target temperature (°C)

// Heating control timings
long HTIME = 30000;               // Heating duration per cycle (30 seconds in milliseconds)
long WAIT_AFTER = 120000;         // Rest/cooling duration after heating (2 minutes in milliseconds)

// Data logging interval: total time between each cycle start
long LOG_INTERVAL = 300000;       // Total cycle interval (5 minutes in milliseconds)

// Sensor error tracking
int sensor_error_count = 0;       // Counter for consecutive sensor read failures
const int MAX_ERROR_ATTEMPTS = 3; // Maximum consecutive errors before logging failure

// ========== PERIPHERAL OBJECT INITIALIZATION ==========
RTC_DS3231 rtc;                   // Real-Time Clock object
Adafruit_SHTC3 shtc3 = Adafruit_SHTC3();  // SHTC3 sensor object
File dataFile;                    // SD card file object for data logging

// ========== SETUP FUNCTION ==========
// Runs once when Arduino powered on or reset
void setup() {
  // Initialize serial communication for debugging output
  Serial.begin(9600);
  
  // Initialize I2C communication for RTC and SHTC3 sensors
  Wire.begin();

  // Initialize relay pin as output and set to HIGH (relay OFF - active low)
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);  // Ensure relay is OFF at startup

  // ========== INITIALIZE RTC ==========
  if (!rtc.begin()) {
    Serial.println("ERROR: No RTC found! Check I2C connections.");
    while (1);  // Halt if RTC not detected
  }
  Serial.println("RTC initialized successfully.");

  // ========== INITIALIZE SD CARD ==========
  if (!SD.begin(CS_PIN)) {
    Serial.println("ERROR: SD card initialization failed! Check CS pin and connections.");
    while (1);  // Halt if SD card not detected
  }
  Serial.println("SD card initialized successfully.");

  // ========== INITIALIZE SHTC3 SENSOR ==========
  if (!shtc3.begin()) {
    Serial.println("ERROR: SHTC3 sensor not found! Check I2C connections.");
    while (1);  // Halt if SHTC3 not detected
  }
  Serial.println("SHTC3 sensor initialized successfully.");

  // ========== CREATE CSV HEADER ==========
  // Check if datalog file already exists; if not, create it with header row
  if (!SD.exists("datalog.csv")) {
    dataFile = SD.open("datalog.csv", FILE_WRITE);
    if (dataFile) {
      // CSV header: Time and sensor data with action taken
      dataFile.println("Hour:Minute:Second,Temperature(C),Humidity(%),Action");
      dataFile.close();
      Serial.println("Created new datalog.csv with header.");
    } else {
      Serial.println("ERROR: Could not create datalog.csv!");
      while (1);  // Halt if CSV creation fails
    }
  } else {
    Serial.println("Using existing datalog.csv file.");
  }

  Serial.println("Setup complete. Starting heat control system...");
  Serial.println("---");
}

// ========== MAIN LOOP ==========
// Runs repeatedly at each cycle interval
void loop() {
  // Get current date/time from RTC
  DateTime now = rtc.now();

  // ========== SENSOR READING ==========
  // Attempt to read temperature and humidity from SHTC3 sensor
  sensors_event_t humidity, temp;
  shtc3.getEvent(&humidity, &temp);

  float temperature = temp.temperature;
  float humidity_value = humidity.relative_humidity;

  // ========== ERROR CHECKING ==========
  // Check if sensor reading is valid (isnan = "is not a number")
  if (isnan(temperature) || isnan(humidity_value)) {
    sensor_error_count++;
    Serial.print("WARNING: Sensor read failed (attempt ");
    Serial.print(sensor_error_count);
    Serial.println(")");
    
    // Log sensor failure to CSV
    logToSD(now, NAN, NAN, "SENSOR_ERROR");
    
    // If sensor fails too many times, halt the system
    if (sensor_error_count >= MAX_ERROR_ATTEMPTS) {
      Serial.println("CRITICAL: Sensor failures exceeded max attempts. System paused.");
      // Wait before retrying (prevents rapid error spam)
      delay(LOG_INTERVAL);
      sensor_error_count = 0;  // Reset counter after long wait
      return;
    }
    return;  // Skip heating cycle and try again next iteration
  }

  // Reset error counter on successful read
  sensor_error_count = 0;

  // ========== LOG INITIAL STATE ==========
  // Record the current temperature/humidity before any heating action
  logToSD(now, temperature, humidity_value, "CYCLE_START");

  // ========== HEATING DECISION LOGIC ==========
  // Check if current temperature is below setpoint
  if (temperature < SETPOINT) {
    Serial.print("Temperature ");
    Serial.print(temperature);
    Serial.println("C is below setpoint. Activating heater...");

    // Turn on relay (LOW = active)
    digitalWrite(RELAY_PIN, LOW);
    delay(HTIME);

    // Turn off relay after heating time (HIGH = inactive)
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("Heating complete. Starting cooling rest period...");

    // Log heating action
    logToSD(now, temperature, humidity_value, "HEATING_ACTIVE");

    // Wait for cooling/rest period
    delay(WAIT_AFTER);

  } else {
    // Temperature is above setpoint, no heating needed
    Serial.print("Temperature ");
    Serial.print(temperature);
    Serial.println("C is at or above setpoint. Skipping heating.");
    
    // Log that heating was not needed
    logToSD(now, temperature, humidity_value, "HEATING_SKIP");
  }

  // ========== CYCLE COMPLETION ==========
  // Wait for remainder of LOG_INTERVAL to complete the cycle
  // (total cycle time = heating + rest + logging)
  Serial.println("Cycle complete. Waiting for next interval...");
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
 *   String act  - Description of action taken (e.g., "HEATING_ACTIVE", "SENSOR_ERROR")
 */
void logToSD(DateTime n, float t, float h, String act) {
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
    Serial.print("LOGGED: ");
    if (!isnan(t)) {
      Serial.print(t);
      Serial.print("C - ");
    }
    Serial.println(act);

  } else {
    // If file open fails, report error via serial
    Serial.println("ERROR: Could not open datalog.csv for writing!");
  }
}
