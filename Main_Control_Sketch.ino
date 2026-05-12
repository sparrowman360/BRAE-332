/*
 * BRAE-332: Heat Control System with Data Logging
 * ================================================
 * This program controls a heating system using an active-HIGH relay and logs
 * temperature/humidity data from an SHTC3 sensor to an SD card via CSV.
 * 
 * Hardware:
 * - Arduino microcontroller
 * - DS3231 Real-Time Clock (RTC) via I2C
 * - SHTC3 Temperature/Humidity Sensor via I2C
 * - SD Card Module (CS pin 10, SPI interface)
 * - Relay module (active high: HIGH = ON, LOW = OFF)
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
#include <TaskScheduler.h>         // Task Scheduler library for non-blocking timing

// ========== PIN DEFINITIONS ==========
#define CS_PIN 10                  // SD card chip select pin (from Datalogger branch)
#define HEATING_PIN 7              // Heating relay control pin
#define LIGHT_PIN 9                // Light relay control pin (TBD)
#define FAN_PIN 8                  // Fan relay control pin 
#define PUMP_PIN 6                // Pump relay control pin 

// Relay logic: set to true for active-HIGH modules, false for active-LOW modules
const bool RELAY_ACTIVE_HIGH = true;
const int HEATING_ON = RELAY_ACTIVE_HIGH ? HIGH : LOW;
const int HEATING_OFF = RELAY_ACTIVE_HIGH ? LOW : HIGH;

// ========== SYSTEM PARAMETERS ==========
// Temperature setpoint: system will heat when temperature drops below this value
const float SETPOINT = 25.0;      // Target temperature (°C)

// Heating control timings
const long HTIME = 120000;         // Heating duration per cycle (60 seconds in milliseconds)
const long WAIT_AFTER = 120000;   // Rest/cooling duration after heating (2 minutes in milliseconds)

// Data logging interval: total time between each cycle start
const long LOG_INTERVAL = 300000; // Total cycle interval (5 minutes in milliseconds)

// Sensor error tracking
int sensor_error_count = 0;       // Counter for consecutive sensor read failures
const int MAX_ERROR_ATTEMPTS = 3; // Maximum consecutive errors before logging failure
int retryAttempt = 0;             // Current retry attempt for sensor reading

// Global variables for current cycle data (used by tasks)
DateTime currentNow;
float currentTemp;
float currentHumidity;

// ========== PERIPHERAL OBJECT INITIALIZATION ==========
RTC_DS3231 rtc;                   // Real-Time Clock object
SHTC3 shtc3;                      // SHTC3 sensor object (SparkFun library)
File dataFile;                    // SD card file object for data logging
Scheduler ts;                     // Task Scheduler for non-blocking operations

// ========== TASK OBJECTS ==========
Task tRunCycle(0, TASK_FOREVER, &runCycle, &ts, false);
Task tTurnOff(0, TASK_ONCE, &turnOffRelay, &ts, false);
Task tLogHeat(0, TASK_ONCE, &logHeatCycle, &ts, false);
Task tReEnable(0, TASK_ONCE, &reEnableCycle, &ts, false);
Task tSensorRetry(0, TASK_ONCE, &sensorRetry, &ts, false);

// ========== HELPER FUNCTIONS ==========
void setHeatingRelay(bool on) {
  digitalWrite(HEATING_PIN, on ? HEATING_ON : HEATING_OFF);
  Serial.print(F("Relay -> "));
  Serial.println(on ? F("ON") : F("OFF"));
}

void setLight(bool on) {
  digitalWrite(LIGHT_PIN, on ? HIGH : LOW);  // Assume active-HIGH; adjust if needed
  Serial.print(F("Light -> "));
  Serial.println(on ? F("ON") : F("OFF"));
}

void setFan(bool on) {
  digitalWrite(FAN_PIN, on ? HIGH : LOW);  // Assume active-HIGH; adjust if needed
  Serial.print(F("Fan -> "));
  Serial.println(on ? F("ON") : F("OFF"));
}

void setPump(bool on) {
  digitalWrite(PUMP_PIN, on ? HIGH : LOW);  // Assume active-HIGH; adjust if needed
  Serial.print(F("Pump -> "));
  Serial.println(on ? F("ON") : F("OFF"));
}

void printTimestamp(Print &out, const DateTime &n) {
  out.print(n.year());
  out.print(",");
  out.print(n.month());
  out.print(",");
  out.print(n.day());
  out.print(",");
  out.print(n.hour());
  out.print(",");
  out.print(n.minute());
  out.print(",");
  out.print(n.second());
}

// ========== TASK FUNCTIONS FOR SCHEDULER ==========
void turnOffRelay() {
  setHeatingRelay(false);
  Serial.println(F("Heat OFF"));
}

void logHeatCycle() {
  logToSD(currentNow, currentTemp, currentHumidity, "HEAT_CYCLE");
}

void reEnableCycle() {
  ts.enableTask(tRunCycle);
  sensor_error_count = 0;
  Serial.println(F("Cycle resumed after sensor pause"));
}

void processSensorData() {
  // ========== HEATING DECISION LOGIC ==========
  // Check if current temperature is below setpoint
  if (currentTemp < SETPOINT) {
    Serial.print(F("Heat ON: "));
    Serial.println(currentTemp);

    // Turn on relay for heating
    setHeatingRelay(true);

    // Schedule to turn off relay after HTIME
    ts.enableDelayedTask(tTurnOff, HTIME);

    // Schedule to log heat cycle after HTIME + WAIT_AFTER
    ts.enableDelayedTask(tLogHeat, HTIME + WAIT_AFTER);

  } else {
    // Temperature is above setpoint, no heating needed
    Serial.print(F("OK: "));
    Serial.println(currentTemp);
    
    // Log that heating was not needed
    logToSD(currentNow, currentTemp, currentHumidity, "OK");
  }

  // ========== CYCLE COMPLETION ==========
  Serial.println(F("Cycle done"));
}

void sensorRetry() {
  int sensorStatus = shtc3.update();
  Serial.print(F("Retry "));
  Serial.print(retryAttempt);
  Serial.print(F(" status="));
  Serial.println(sensorStatus);

  if (sensorStatus == SHTC3_Status_Nominal) {
    // Success
    sensor_error_count = 0;
    currentTemp = shtc3.toDegC();
    currentHumidity = shtc3.toPercent();
    processSensorData();
  } else if (retryAttempt >= MAX_ERROR_ATTEMPTS) {
    // Max retries reached
    sensor_error_count++;
    Serial.print(F("Sensor error "));
    Serial.println(sensor_error_count);
    logToSD(currentNow, NAN, NAN, "ERROR");

    if (sensor_error_count >= MAX_ERROR_ATTEMPTS) {
      Serial.println(F("Sensor fail - paused"));
      ts.disableTask(tRunCycle);
      ts.enableDelayedTask(tReEnable, LOG_INTERVAL);
    }
  } else {
    // Retry again
    retryAttempt++;
    ts.enableDelayedTask(tSensorRetry, 200);
  }
}
void runCycle() {
  // Get current date/time from RTC
  currentNow = rtc.now();

  // ========== SENSOR READING ==========
  // Attempt to read temperature and humidity from SHTC3 sensor
  int sensorStatus = shtc3.update();
  if (sensorStatus != SHTC3_Status_Nominal) {
    Serial.print(F("Sensor read failed, status="));
    Serial.println(sensorStatus);
    retryAttempt = 1;
    ts.enableDelayedTask(tSensorRetry, 200);
    return;
  }

  // Successful read
  sensor_error_count = 0;
  currentTemp = shtc3.toDegC();
  currentHumidity = shtc3.toPercent();
  processSensorData();
}

// ========== SETUP FUNCTION ==========
// Runs once when Arduino powered on or reset
void setup() {
  // Initialize serial communication for debugging output
  Serial.begin(4800);  // Reduced baud rate to save memory
  
  // Initialize I2C communication for RTC and SHTC3 sensors
  Wire.begin();

  // Initialize relay pin as output and set to inactive state on startup
  pinMode(HEATING_PIN, OUTPUT);
  setHeatingRelay(false);

  // Initialize additional relay pins (TBD - set to inactive)
  pinMode(LIGHT_PIN, OUTPUT);
  setLight(false);
  pinMode(FAN_PIN, OUTPUT);
  setFan(false);
  pinMode(PUMP_PIN, OUTPUT);
  setPump(false);

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
      dataFile.println(F("Year,Month,Day,Hour,Minute,Second,Temperature(C),Humidity(%),Action"));
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

  // ========== SETUP TASK SCHEDULER ==========
  ts.addTask(tRunCycle);
  ts.addTask(tTurnOff);
  ts.addTask(tLogHeat);
  ts.addTask(tReEnable);
  ts.addTask(tSensorRetry);
  ts.enablePeriodicTask(tRunCycle, LOG_INTERVAL);
}

// ========== MAIN LOOP ==========
// Runs repeatedly to execute scheduled tasks
void loop() {
  ts.execute();
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
    // Format: Year,Month,Day,Hour,Minute,Second
    printTimestamp(dataFile, n);
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
