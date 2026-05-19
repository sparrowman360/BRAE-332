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
const float HEATING_SETPOINT = 25.0;      // Heating target midpoint (°C)
const float COOLING_SETPOINT = 27.0;      // Cooling target midpoint (°C)

// Hysteresis band definitions
//   Heating:  ON when <= HEATING_ON_POINT, OFF above HEATING_OFF_POINT
//   Cooling:  ON when >= COOLING_ON_POINT, OFF below COOLING_OFF_POINT
const float HEATING_ON_POINT = 23.0;      // Heating ON threshold (°C)
const float HEATING_OFF_POINT = 25.0;     // Heating OFF threshold (°C)
const float COOLING_ON_POINT = 27.5;      // Cooling ON threshold (°C)
const float COOLING_OFF_POINT = 27.0;     // Cooling OFF threshold (°C)

// Heating control timings
const long HTIME = 120000;                // Heating duration per cycle (120 seconds in milliseconds)
const long WAIT_AFTER = 120000;           // Rest duration after heating (120 seconds in milliseconds)

// Cooling control timings
const long COOL_TIME = 60000;              // Cooling duration per cycle (60 seconds in milliseconds)
const long COOL_WAIT_AFTER = 60000;        // Rest duration after cooling (60 seconds in milliseconds)

// Light schedule timings
const uint32_t LIGHT_START_DELAY = 3UL * 24UL * 60UL * 60UL; // 3 days in seconds
const uint32_t LIGHT_ON_DURATION = 18UL * 60UL * 60UL;       // 18 hours in seconds
const uint32_t LIGHT_OFF_DURATION = 6UL * 60UL * 60UL;       // 6 hours in seconds

// Data logging interval: fixed interval for periodic logs
const long LOG_INTERVAL = 600000;         // 10 minutes in milliseconds
const long SENSOR_CHECK_INTERVAL = 5000;  // Sensor threshold check interval while idle or waiting

// Sensor error tracking
int sensor_error_count = 0;       // Counter for consecutive sensor read failures
const int MAX_ERROR_ATTEMPTS = 3; // Maximum consecutive errors before logging failure
int retryAttempt = 0;             // Current retry attempt for sensor reading

// Global variables for current cycle data (used by tasks)
DateTime currentNow;
DateTime startTime;
float currentTemp;
float currentHumidity;
bool heatingActive = false;
bool coolingActive = false;
bool heatingWait = false;
bool coolingWait = false;
bool lightsOn = false;

// ========== PERIPHERAL OBJECT INITIALIZATION ==========
RTC_DS3231 rtc;                   // Real-Time Clock object
SHTC3 shtc3;                      // SHTC3 sensor object (SparkFun library)
File dataFile;                    // SD card file object for data logging
Scheduler ts;                     // Task Scheduler for non-blocking operations

// ========== TASK CALLBACK PROTOTYPES ==========
void runCycle();
void turnOffRelay();
void heatWaitComplete();
void turnOffCooling();
void coolWaitComplete();
void logPeriodic();
void sensorRetry();
void reEnableCycle();

// ========== TASK OBJECTS ==========
Task tRunCycle(SENSOR_CHECK_INTERVAL, TASK_FOREVER, &runCycle, &ts, false);
Task tHeatOff(HTIME, TASK_ONCE, &turnOffRelay, &ts, false);
Task tHeatWait(WAIT_AFTER, TASK_ONCE, &heatWaitComplete, &ts, false);
Task tCoolOff(COOL_TIME, TASK_ONCE, &turnOffCooling, &ts, false);
Task tCoolWait(COOL_WAIT_AFTER, TASK_ONCE, &coolWaitComplete, &ts, false);
Task tLogPeriodic(LOG_INTERVAL, TASK_FOREVER, &logPeriodic, &ts, false);
Task tSensorRetry(200, TASK_ONCE, &sensorRetry, &ts, false);
Task tReEnable(LOG_INTERVAL, TASK_ONCE, &reEnableCycle, &ts, false);

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

void startHeatingCycle() {
  if (currentTemp > HEATING_ON_POINT) {
    Serial.print(F("Heating suppressed by hysteresis: "));
    Serial.print(currentTemp);
    Serial.print(F(" > "));
    Serial.println(HEATING_ON_POINT);
    return;
  }

  heatingActive = true;
  heatingWait = false;
  setHeatingRelay(true);
  setPump(false);
  setFan(false);
  Serial.println(F("Heating cycle started."));
  tHeatOff.restart();
}

void startCoolingCycle() {
  if (currentTemp < COOLING_ON_POINT) {
    Serial.print(F("Cooling suppressed by hysteresis: "));
    Serial.print(currentTemp);
    Serial.print(F(" < "));
    Serial.println(COOLING_ON_POINT);
    return;
  }

  coolingActive = true;
  coolingWait = false;
  setHeatingRelay(false);
  setPump(true);
  setFan(true);
  Serial.println(F("Cooling cycle started."));
  tCoolOff.restart();
}

void turnOffCooling() {
  setPump(false);
  setFan(false);
  coolingActive = false;
  coolingWait = true;
  Serial.println(F("Cooling ON time complete, entering cooling rest."));
  tCoolWait.restart();
}

void heatWaitComplete() {
  heatingWait = false;
  Serial.println(F("Heating rest complete, rechecking temperature."));
  runCycle();
}

void coolWaitComplete() {
  coolingWait = false;
  Serial.println(F("Cooling rest complete, rechecking temperature."));
  runCycle();
}

void updateLightState() {
  if (!startTime.unixtime()) {
    return;
  }

  uint32_t elapsedSec = currentNow.unixtime() - startTime.unixtime();
  if (elapsedSec < LIGHT_START_DELAY) {
    if (lightsOn) {
      setLight(false);
      lightsOn = false;
    }
    return;
  }

  uint32_t cycleSeconds = elapsedSec - LIGHT_START_DELAY;
  uint32_t cycleLength = LIGHT_ON_DURATION + LIGHT_OFF_DURATION;
  bool shouldBeOn = (cycleSeconds % cycleLength) < LIGHT_ON_DURATION;

  if (shouldBeOn != lightsOn) {
    setLight(shouldBeOn);
    lightsOn = shouldBeOn;
  }
}

void logPeriodic() {
  currentNow = rtc.now();
  int sensorStatus = shtc3.update();
  float temp = NAN;
  float humidity = NAN;

  if (sensorStatus == SHTC3_Status_Nominal) {
    temp = shtc3.toDegC();
    humidity = shtc3.toPercent();
    currentTemp = temp;
    currentHumidity = humidity;
  } else {
    Serial.print(F("Periodic sensor read failed, status="));
    Serial.println(sensorStatus);
  }

  updateLightState();

  char action[48];
  sprintf(action, "Heat:%s,Cool:%s,Light:%s",
          heatingActive ? "ON" : "OFF",
          coolingActive ? "ON" : "OFF",
          lightsOn ? "ON" : "OFF");
  logToSD(currentNow, temp, humidity, action);
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
  heatingActive = false;
  heatingWait = true;
  Serial.println(F("Heat OFF"));
  tHeatWait.restart();
}

void reEnableCycle() {
  tRunCycle.restart();
  sensor_error_count = 0;
  Serial.println(F("Cycle resumed after sensor pause"));
}

void processSensorData() {
  // ========== HEATING DECISION LOGIC ==========
  // Check if current temperature is below setpoint
  if (heatingActive || coolingActive || heatingWait || coolingWait) {
    Serial.println(F("Cycle busy, no new action taken."));
    return;
  }

  if (currentTemp <= HEATING_ON_POINT) {
    Serial.print(F("Heat ON: "));
    Serial.println(currentTemp);
    startHeatingCycle();
    logToSD(currentNow, currentTemp, currentHumidity, "HEATING_ON");
  } else if (currentTemp >= COOLING_ON_POINT) {
    Serial.print(F("Cooling ON: "));
    Serial.println(currentTemp);
    startCoolingCycle();
    logToSD(currentNow, currentTemp, currentHumidity, "COOLING_ON");
  } else {
    Serial.print(F("Idle: "));
    Serial.println(currentTemp);
    logToSD(currentNow, currentTemp, currentHumidity, "IDLE");
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
      tRunCycle.disable();
      tReEnable.enable();
    }
  } else {
    // Retry again
    retryAttempt++;
    tSensorRetry.restart();
  }
}
void runCycle() {
  // Get current date/time from RTC
  currentNow = rtc.now();
  updateLightState();

  // ========== SENSOR READING ==========
  // Attempt to read temperature and humidity from SHTC3 sensor
  int sensorStatus = shtc3.update();
  if (sensorStatus != SHTC3_Status_Nominal) {
    Serial.print(F("Sensor read failed, status="));
    Serial.println(sensorStatus);
    retryAttempt = 1;
    tSensorRetry.restart();
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
  startTime = rtc.now();
  lightsOn = false;

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
  ts.addTask(tHeatOff);
  ts.addTask(tHeatWait);
  ts.addTask(tCoolOff);
  ts.addTask(tCoolWait);
  ts.addTask(tLogPeriodic);
  ts.addTask(tSensorRetry);
  ts.addTask(tReEnable);

  tRunCycle.restart();
  tLogPeriodic.restart();
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
