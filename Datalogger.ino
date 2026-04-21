#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include "RTClib.h" // DS3231 library
#include "DHT.h"
#include "SparkFun_SHTC3.h" // SHTC3 library

// DHT11 sensor pin and type.
#define DHTPIN 2        // Digital pin connected to the DHT sensor
#define DHTTYPE DHT11   // Sensor type: DHT11

// SD card chip select pin. Change this if your SD module uses a different CS pin.
#define CS_PIN 10       // SD card chip select pin

// File name for CSV logging on the SD card.
const char DATA_FILE_NAME[] = "datalog.csv";

// Delay between log writes: 15 minutes in milliseconds.
const unsigned long LOG_INTERVAL_MS = 15UL * 60UL * 1000UL;

// Create sensor and peripheral objects.
DHT dht(DHTPIN, DHTTYPE);
RTC_DS3231 rtc;
SHTC3 shtc3;
File dataFile;
unsigned long lastLogMillis = 0; // Tracks the last time a log was written.

void setup() {
  Serial.begin(9600); // Initialize serial output for troubleshooting.
  Wire.begin();       // Initialize I2C for the RTC and SHTC3.

  Serial.println("Starting data logger...");

  // Initialize RTC. Halt if the module is not found.
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC!");
    while (1) delay(10);
  }

  // Initialize SD card. Halt if initialization fails.
  if (!SD.begin(CS_PIN)) {
    Serial.println("SD card initialization failed!");
    while (1) delay(10);
  }

  dht.begin(); // Start the DHT11 sensor.

  // Initialize SHTC3. Halt if the sensor is not detected.
  if (shtc3.begin() != SHTC3_Status_Nominal) {
    Serial.println("SHTC3 not detected!");
    while (1) delay(10);
  }

  // Create the CSV file header if the file does not already exist.
  if (!SD.exists(DATA_FILE_NAME)) {
    dataFile = SD.open(DATA_FILE_NAME, FILE_WRITE);
    if (dataFile) {
      dataFile.println("Year,Month,Day,Hour,Minute,Second,DHT_Temperature(C),DHT_Humidity(%),SHTC3_Temperature(C),SHTC3_Humidity(%)");
      dataFile.close();
      Serial.println("Header written to datalog.csv");
    } else {
      Serial.println("Error opening datalog.csv for header write");
      while (1) delay(10);
    }
  }

  // Start the timing interval from setup completion.
  lastLogMillis = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // Wait until 15 minutes have passed before logging again.
  if (currentMillis - lastLogMillis < LOG_INTERVAL_MS) {
    delay(1000); // Sleep 1 second to avoid busy waiting.
    return;
  }

  lastLogMillis = currentMillis;
  logSensorValues(); // Read sensors and append a row to the CSV file.
}

void logSensorValues() {
  DateTime now = rtc.now(); // Get the current date/time from the RTC.

  // Read DHT11 values.
  float dhtHumidity = dht.readHumidity();
  float dhtTemperature = dht.readTemperature();

  // Check for invalid DHT readings.
  if (isnan(dhtHumidity) || isnan(dhtTemperature)) {
    Serial.println("Failed to read from DHT sensor!");
    return;
  }

  // Read SHTC3 values using the library's update() return value.
  if (shtc3.update() != SHTC3_Status_Nominal) {
    Serial.println("Error reading SHTC3 sensor!");
    return;
  }

  float shtc3Humidity = shtc3.toPercent();
  float shtc3Temperature = shtc3.toDegC();

  // Open the CSV file for appending. FILE_WRITE opens the file and writes at the end.
  dataFile = SD.open(DATA_FILE_NAME, FILE_WRITE);
  if (dataFile) {
    // Write date/time and sensor values separated by commas.
    dataFile.print(now.year(), DEC);
    dataFile.print(",");
    dataFile.print(now.month(), DEC);
    dataFile.print(",");
    dataFile.print(now.day(), DEC);
    dataFile.print(",");
    dataFile.print(now.hour(), DEC);
    dataFile.print(",");
    dataFile.print(now.minute(), DEC);
    dataFile.print(",");
    dataFile.print(now.second(), DEC);
    dataFile.print(",");
    dataFile.print(dhtTemperature);
    dataFile.print(",");
    dataFile.print(dhtHumidity);
    dataFile.print(",");
    dataFile.print(shtc3Temperature);
    dataFile.print(",");
    dataFile.println(shtc3Humidity);
    dataFile.close();

    // Print the same values to Serial for troubleshooting.
    Serial.print(now.year(), DEC);
    Serial.print("/");
    Serial.print(now.month(), DEC);
    Serial.print("/");
    Serial.print(now.day(), DEC);
    Serial.print(" ");
    Serial.print(now.hour(), DEC);
    Serial.print(":");
    Serial.print(now.minute(), DEC);
    Serial.print(":");
    Serial.print(now.second(), DEC);
    Serial.print(" DHT Temp: ");
    Serial.print(dhtTemperature);
    Serial.print("C Hum: ");
    Serial.print(dhtHumidity);
    Serial.print("% | SHTC3 Temp: ");
    Serial.print(shtc3Temperature);
    Serial.print("C Hum: ");
    Serial.print(shtc3Humidity);
    Serial.println("%");
  } else {
    Serial.println("Error opening datalog.csv");
  }
}
