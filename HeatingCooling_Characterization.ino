/*
 * HeatingCooling_Characterization.ino
 * ====================================
 * Standalone sketch to characterize chamber heating and evaporative cooling.
 * Uses the same RTC, SHTC3 sensor, SD card, and heating pad pin as the
 * main control sketch. The evaporative cooler pump is on pin 6.
 *
 * The sketch:
 * 1. Heats the chamber until the internal temperature reaches at least 30 °C.
 * 2. Switches to evaporative cooling to reduce chamber temperature as low as
 *    possible.
 * 3. Logs temperature, humidity, and action status throughout the process.
 *
 * Logged CSV format matches the main control sketch:
 * Year,Month,Day,Hour,Minute,Second,Temperature(C),Humidity(%),Action
 */

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include "RTClib.h"
#include "SparkFun_SHTC3.h"

// ========== PIN DEFINITIONS ==========
#define CS_PIN 10                  // SD card chip select pin
#define HEATING_PIN 7              // Heating pad relay control pin
#define PUMP_PIN 6                 // Evaporative cooler pump relay control pin

// Relay logic: set to true for active-HIGH modules, false for active-LOW modules
const bool RELAY_ACTIVE_HIGH = true;
const int HEATING_ON = RELAY_ACTIVE_HIGH ? HIGH : LOW;
const int HEATING_OFF = RELAY_ACTIVE_HIGH ? LOW : HIGH;
const int PUMP_ON = HIGH;          // Assume pump relay is active HIGH
const int PUMP_OFF = LOW;

// Process control parameters
const float HEATING_TARGET_TEMP = 30.0;          // °C
const unsigned long SAMPLE_INTERVAL = 10000UL;   // Data sample interval in milliseconds
const int STABLE_COUNT_REQUIRED = 3;             // Number of upward/warming samples to stop cooling
const float COOLING_STABLE_THRESHOLD = 0.10;     // °C threshold for stall detection

// Data logging variables
DateTime currentNow;
float currentTemp = NAN;
float currentHumidity = NAN;
unsigned long lastSampleTime = 0;

// Cooling detection state
float lowestTempObserved = 1000.0;
int coolingStableCount = 0;

// System state
enum SystemPhase {PHASE_HEATING, PHASE_COOLING, PHASE_COMPLETE};
SystemPhase phase = PHASE_HEATING;

// Peripheral objects
RTC_DS3231 rtc;
SHTC3 shtc3;
File dataFile;

void setHeatingRelay(bool on) {
  digitalWrite(HEATING_PIN, on ? HEATING_ON : HEATING_OFF);
  Serial.print(F("Heating Relay -> "));
  Serial.println(on ? F("ON") : F("OFF"));
}

void setPump(bool on) {
  digitalWrite(PUMP_PIN, on ? PUMP_ON : PUMP_OFF);
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

void logToSD(DateTime n, float t, float h, const char* act) {
  dataFile = SD.open("datalog.csv", FILE_WRITE);

  if (dataFile) {
    printTimestamp(dataFile, n);
    dataFile.print(",");

    if (isnan(t)) {
      dataFile.print("ERROR");
    } else {
      dataFile.print(t);
    }
    dataFile.print(",");

    if (isnan(h)) {
      dataFile.print("ERROR");
    } else {
      dataFile.print(h);
    }
    dataFile.print(",");
    dataFile.println(act);
    dataFile.close();

    Serial.print(F("LOG: "));
    if (!isnan(t)) {
      Serial.print(t);
      Serial.print(F("C "));
    }
    if (!isnan(h)) {
      Serial.print(h);
      Serial.print(F(" % "));
    }
    Serial.print(F("Action="));
    Serial.println(act);
  } else {
    Serial.println(F("Log error"));
  }
}

void initializeSD() {
  if (!SD.begin(CS_PIN)) {
    Serial.println(F("SD fail"));
    while (1);
  }
  Serial.println(F("SD OK"));

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
}

void sampleAndUpdateState() {
  currentNow = rtc.now();
  int sensorStatus = shtc3.update();

  if (sensorStatus != SHTC3_Status_Nominal) {
    Serial.print(F("Sensor read failed, status="));
    Serial.println(sensorStatus);
    logToSD(currentNow, NAN, NAN, (phase == PHASE_HEATING ? "HEATING" : "COOLING"));
    return;
  }

  currentTemp = shtc3.toDegC();
  currentHumidity = shtc3.toPercent();

  if (phase == PHASE_HEATING) {
    if (currentTemp >= HEATING_TARGET_TEMP) {
      phase = PHASE_COOLING;
      setHeatingRelay(false);
      setPump(true);
      lowestTempObserved = currentTemp;
      coolingStableCount = 0;
      Serial.println(F("Target reached. Switching to COOLING."));
    }
    logToSD(currentNow, currentTemp, currentHumidity, "HEATING");
  } else if (phase == PHASE_COOLING) {
    if (currentTemp < lowestTempObserved - 0.05) {
      lowestTempObserved = currentTemp;
      coolingStableCount = 0;
    } else if (currentTemp > lowestTempObserved + COOLING_STABLE_THRESHOLD) {
      coolingStableCount++;
    }

    logToSD(currentNow, currentTemp, currentHumidity, "COOLING");

    if (coolingStableCount >= STABLE_COUNT_REQUIRED) {
      setPump(false);
      phase = PHASE_COMPLETE;
      Serial.println(F("Cooling complete. Pump stopped."));
    }
  } else {
    logToSD(currentNow, currentTemp, currentHumidity, "COOLING");
  }
}

void setup() {
  Serial.begin(4800);
  Wire.begin();

  pinMode(HEATING_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  setHeatingRelay(false);
  setPump(false);

  if (!rtc.begin()) {
    Serial.println(F("RTC fail"));
    while (1);
  }
  Serial.println(F("RTC OK"));

  initializeSD();

  if (shtc3.begin() != SHTC3_Status_Nominal) {
    Serial.println(F("SHTC3 fail"));
    while (1);
  }
  Serial.println(F("SHTC3 OK"));

  setHeatingRelay(true);
  Serial.println(F("Starting heating phase."));
  lastSampleTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastSampleTime >= SAMPLE_INTERVAL) {
    lastSampleTime = currentMillis;
    sampleAndUpdateState();
  }
}
