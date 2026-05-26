/*
 * BRAE-332: Heat Control System with Data Logging (millis scheduler)
 * ================================================================
 *
 * Hardware:
 * - Arduino microcontroller
 * - DS3231 RTC via I2C
 * - SHTC3 temperature/humidity sensor via I2C
 * - SD card module via SPI, CS pin 10
 * - Relay modules for heating, fan, pump, and light
  *
  * Libraries (READ ME):
  * - SPI.h for SD card communication
  * - SD.h for SD card file handling
  * - Wire.h for I2C communication
  * - RTClib.h for RTC handling
  * - SparkFun_SHTC3.h for SHTC3 sensor handling
  * Libraries can be installed via Arduino Library Manager in the Arduino IDE.
  * Go to Sketch -> Include Library -> Manage Libraries, then search for the above listed libraries and install them. 
  * The program will not work without  these libraries installed. 
 */

#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include "RTClib.h"
#include "SparkFun_SHTC3.h"

// ========== PIN DEFINITIONS ==========
#define CS_PIN 10
#define HEATING_PIN 7
#define LIGHT_PIN 9
#define FAN_PIN 8
#define PUMP_PIN 6

const bool RELAY_ACTIVE_HIGH = true;
const int HEATING_ON = RELAY_ACTIVE_HIGH ? HIGH : LOW;
const int HEATING_OFF = RELAY_ACTIVE_HIGH ? LOW : HIGH;

// ========== SYSTEM PARAMETERS ==========
const float HEATING_SETPOINT = 25.0;      // Heating threshold (°C)
const float COOLING_SETPOINT = 27.0;      // Cooling threshold (°C)

const long HTIME = 30000;                // Heating ON duration 30sec
const long WAIT_AFTER = 60000;           // Heating rest duration 1min
const long COOL_TIME = 60000;             // Cooling ON duration 1min
const long COOL_WAIT_AFTER = 60000;       // Cooling rest duration 1min

const uint32_t LIGHT_START_DELAY = 3UL * 24UL * 60UL * 60UL; // 3 days in seconds
const uint32_t LIGHT_ON_DURATION = 18UL * 60UL * 60UL;       // 18 hours in seconds
const uint32_t LIGHT_OFF_DURATION = 6UL * 60UL * 60UL;       // 6 hours in seconds

const long LOG_INTERVAL = 600000;         // 10 minutes in milliseconds
const long SENSOR_CHECK_INTERVAL = 5000;  // Sensor check interval while idle/waiting

const int MAX_ERROR_ATTEMPTS = 3;

// ========== STATE VARIABLES ==========
RTC_DS3231 rtc;
SHTC3 shtc3;
File dataFile;

DateTime currentNow;
DateTime startTime;

float currentTemp = NAN;
float currentHumidity = NAN;

bool heatingActive = false;
bool coolingActive = false;
bool heatingWait = false;
bool coolingWait = false;
bool lightsOn = false;
int sensor_error_count = 0;
int retryAttempt = 0;

unsigned long nextSensorCheckTime = 0;
unsigned long nextLogTime = 0;
unsigned long nextSerialPrintTime = 0;
unsigned long heatEndTime = 0;
unsigned long heatWaitEndTime = 0;
unsigned long coolEndTime = 0;
unsigned long coolWaitEndTime = 0;

// ========== HELPER FUNCTIONS ==========
void setHeatingRelay(bool on) {
  digitalWrite(HEATING_PIN, on ? HEATING_ON : HEATING_OFF);
  Serial.print(F("Relay -> "));
  Serial.println(on ? F("ON") : F("OFF"));
}

void setLight(bool on) {
  digitalWrite(LIGHT_PIN, on ? HIGH : LOW);
  Serial.print(F("Light -> "));
  Serial.println(on ? F("ON") : F("OFF"));
}

void setFan(bool on) {
  digitalWrite(FAN_PIN, on ? HIGH : LOW);
  Serial.print(F("Fan -> "));
  Serial.println(on ? F("ON") : F("OFF"));
}

void setPump(bool on) {
  digitalWrite(PUMP_PIN, on ? HIGH : LOW);
  Serial.print(F("Pump -> "));
  Serial.println(on ? F("ON") : F("OFF"));
}

void printTimestamp(Print &out, const DateTime &n) {
  out.print(n.year()); out.print(",");
  out.print(n.month()); out.print(",");
  out.print(n.day()); out.print(",");
  out.print(n.hour()); out.print(",");
  out.print(n.minute()); out.print(",");
  out.print(n.second());
}

void logToSD(DateTime n, float t, float h, const char* act) {
  dataFile = SD.open("datalog.csv", FILE_WRITE);
  if (!dataFile) {
    Serial.println(F("Log error"));
    return;
  }

  printTimestamp(dataFile, n);
  dataFile.print(",");
  if (isnan(t)) dataFile.print(F("ERROR")); else dataFile.print(t);
  dataFile.print(",");
  if (isnan(h)) dataFile.print(F("ERROR")); else dataFile.print(h);
  dataFile.print(",");
  dataFile.println(act);
  dataFile.close();

  Serial.print(F("LOG: "));
  if (!isnan(t)) {
    Serial.print(t);
    Serial.print(F("C "));
  }
  Serial.println(act);
}

void updateLightState() {
  if (!startTime.unixtime()) return;
  currentNow = rtc.now();
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

void turnOffRelay() {
  setHeatingRelay(false);
  heatingActive = false;
  heatingWait = true;
  heatWaitEndTime = millis() + WAIT_AFTER;
  Serial.println(F("Heat OFF"));
}

void turnOffCooling() {
  setPump(false);
  setFan(false);
  coolingActive = false;
  coolingWait = true;
  coolWaitEndTime = millis() + COOL_WAIT_AFTER;
  Serial.println(F("Cooling ON time complete, entering cooling rest."));
}

void startHeatingCycle() {
  heatingActive = true;
  heatingWait = false;
  setHeatingRelay(true);
  setPump(false);
  setFan(false);
  heatEndTime = millis() + HTIME;
  Serial.println(F("Heating cycle started."));
}

void startCoolingCycle() {
  coolingActive = true;
  coolingWait = false;
  setHeatingRelay(false);
  setPump(true);
  setFan(true);
  coolEndTime = millis() + COOL_TIME;
  Serial.println(F("Cooling cycle started."));
}

void processSensorData() {
  if (heatingActive || coolingActive || heatingWait || coolingWait) {
    Serial.println(F("Cycle busy, no new action taken."));
    return;
  }

  currentNow = rtc.now();
  int sensorStatus = shtc3.update();
  if (sensorStatus != SHTC3_Status_Nominal) {
    Serial.print(F("Sensor read failed, status="));
    Serial.println(sensorStatus);
    retryAttempt = 1;
    sensor_error_count++;
    nextSensorCheckTime = millis() + 200;
    return;
  }

  sensor_error_count = 0;
  currentTemp = shtc3.toDegC();
  currentHumidity = shtc3.toPercent();

  if (currentTemp < HEATING_SETPOINT) {
    Serial.print(F("Heat ON: "));
    Serial.println(currentTemp);
    startHeatingCycle();
  } else if (currentTemp > COOLING_SETPOINT) {
    Serial.print(F("Cooling ON: "));
    Serial.println(currentTemp);
    startCoolingCycle();
  } else {
    Serial.print(F("Idle: "));
    Serial.println(currentTemp);
  }

  Serial.println(F("Cycle done"));
}

void printSensorStatus() {
  currentNow = rtc.now();
  int sensorStatus = shtc3.update();

  if (sensorStatus == SHTC3_Status_Nominal) {
    currentTemp = shtc3.toDegC();
    currentHumidity = shtc3.toPercent();
    Serial.print(F("Temp: "));
    Serial.print(currentTemp);
    Serial.print(F(" C Humidity: "));
    Serial.print(currentHumidity);
    Serial.print(F(" %"));
  } else {
    Serial.print(F("Sensor read failed, status="));
    Serial.print(sensorStatus);
  }

  Serial.print(F(" Heat:"));
  Serial.print(heatingActive ? F("ON") : F("OFF"));
  Serial.print(F(" Cool:"));
  Serial.print(coolingActive ? F("ON") : F("OFF"));
  Serial.print(F(" Light:"));
  Serial.println(lightsOn ? F("ON") : F("OFF"));
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

void setup() {
  Serial.begin(4800);
  Wire.begin();

  pinMode(HEATING_PIN, OUTPUT);
  setHeatingRelay(false);
  pinMode(LIGHT_PIN, OUTPUT);
  setLight(false);
  pinMode(FAN_PIN, OUTPUT);
  setFan(false);
  pinMode(PUMP_PIN, OUTPUT);
  setPump(false);

  if (!rtc.begin()) {
    Serial.println(F("RTC fail"));
    while (1);
  }
  Serial.println(F("RTC OK"));
  startTime = rtc.now();
  lightsOn = false;

  if (!SD.begin(CS_PIN)) {
    Serial.println(F("SD fail"));
    while (1);
  }
  Serial.println(F("SD OK"));

  if (shtc3.begin() != SHTC3_Status_Nominal) {
    Serial.println(F("SHTC3 fail"));
    while (1);
  }
  Serial.println(F("SHTC3 OK"));

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
  nextSensorCheckTime = millis();
  nextSerialPrintTime = millis();
  nextLogTime = millis();
}

void loop() {
  unsigned long now = millis();

  updateLightState();

  if (heatingActive && now >= heatEndTime) {
    turnOffRelay();
  }

  if (coolingActive && now >= coolEndTime) {
    turnOffCooling();
  }

  if (heatingWait && now >= heatWaitEndTime) {
    heatingWait = false;
    Serial.println(F("Heating rest complete, rechecking temperature."));
    processSensorData();
  }

  if (coolingWait && now >= coolWaitEndTime) {
    coolingWait = false;
    Serial.println(F("Cooling rest complete, rechecking temperature."));
    processSensorData();
  }

  if (!heatingActive && !coolingActive && !heatingWait && !coolingWait && now >= nextSensorCheckTime) {
    processSensorData();
    nextSensorCheckTime = now + SENSOR_CHECK_INTERVAL;
  }

  if (now >= nextSerialPrintTime) {
    printSensorStatus();
    nextSerialPrintTime = now + SENSOR_CHECK_INTERVAL;
  }

  if (now >= nextLogTime) {
    logPeriodic();
    nextLogTime = now + LOG_INTERVAL;
  }
}
