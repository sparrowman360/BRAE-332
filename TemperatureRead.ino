#include <Wire.h>
#include "DHT.h"
#include <SparkFun_SHTC3.h>

#define DHTPIN 2      // Digital pin connected to the DHT sensor
#define DHTTYPE DHT11 // DHT 11

DHT dht(DHTPIN, DHTTYPE);
SHTC3 shtc3;

void setup() {
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for some boards.
  }

  Serial.println(F("Temperature Readings Starting"));

  dht.begin();

  Wire.begin();
  if (shtc3.begin() != SHTC3_Status_Nominal) {
    Serial.println(F("SHTC3 sensor not found on I2C bus."));
  } else {
    Serial.println(F("SHTC3 initialized successfully."));
  }
}

void loop() {
  delay(2000); // Delay between readings.

  // Read DHT11 temperature and humidity
  float dhtHum = dht.readHumidity();
  float dhtTemp = dht.readTemperature();

  if (isnan(dhtHum) || isnan(dhtTemp)) {
    Serial.println(F("DHT11: Failed to read from DHT sensor!"));
  } else {
    Serial.print(F("DHT11 Temperature: "));
    Serial.print(dhtTemp);
    Serial.println(F(" °C"));
    Serial.print(F("DHT11 Humidity: "));
    Serial.print(dhtHum);
    Serial.println(F(" %"));
  }

  // Read SHTC3 temperature and humidity
  if (shtc3.update() == SHTC3_Status_Nominal) {
    float shtcTemp = shtc3.toDegC();
    float shtcHum = shtc3.toPercent();
    Serial.print(F("SHTC3 Temperature: "));
    Serial.print(shtcTemp);
    Serial.println(F(" °C"));
    Serial.print(F("SHTC3 Humidity: "));
    Serial.print(shtcHum);
    Serial.println(F(" %"));
  } else {
    Serial.println(F("SHTC3: Failed to read from SHTC3 sensor!"));
  }

  Serial.println();
}
