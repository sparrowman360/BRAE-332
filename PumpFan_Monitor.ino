/*
 * PumpFan_Monitor.ino
  * ===================
   * Simple monitoring program that keeps pump and fans running constantly
    * while displaying temperature and humidity readings every 2 seconds
     * on the serial monitor.
      *
       * Uses the same hardware setup as other programs:
        * - SHTC3 Temperature/Humidity Sensor via I2C
         * - DS3231 Real-Time Clock (RTC) via I2C
          * - Pump relay on pin 6
           * - Fan relay on pin 8
            */
            
            #include <Wire.h>
            #include "RTClib.h"
            #include "SparkFun_SHTC3.h"
            
            // ========== PIN DEFINITIONS ==========
            #define FAN_PIN 8                  // Fan relay control pin
            #define PUMP_PIN 6                 // Pump relay control pin
            
            // Relay logic: active-HIGH (HIGH = ON, LOW = OFF)
            const int RELAY_ON = HIGH;
            const int RELAY_OFF = LOW;
            
            // ========== MONITORING PARAMETERS ==========
            const unsigned long READ_INTERVAL = 2000UL;  // Read sensor every 2 seconds
            
            // ========== PERIPHERAL OBJECTS ==========
            RTC_DS3231 rtc;
            SHTC3 shtc3;
            
            // ========== GLOBAL VARIABLES ==========
            unsigned long lastReadTime = 0;
            
            void setFan(bool on) {
              digitalWrite(FAN_PIN, on ? RELAY_ON : RELAY_OFF);
                Serial.print(F("Fan -> "));
                  Serial.println(on ? F("ON") : F("OFF"));
                  }
                  
                  void setPump(bool on) {
                    digitalWrite(PUMP_PIN, on ? RELAY_ON : RELAY_OFF);
                      Serial.print(F("Pump -> "));
                        Serial.println(on ? F("ON") : F("OFF"));
                        }
                        
                        void setup() {
                          // Initialize serial communication
                            Serial.begin(4800);
                            
                              // Initialize I2C communication
                                Wire.begin();
                                
                                  // Initialize relay pins as outputs
                                    pinMode(FAN_PIN, OUTPUT);
                                      pinMode(PUMP_PIN, OUTPUT);
                                      
                                        // Turn on pump and fan constantly
                                          setPump(true);
                                            setFan(true);
                                            
                                              // Initialize RTC
                                                if (!rtc.begin()) {
                                                    Serial.println(F("RTC fail"));
                                                        while (1);
                                                          }
                                                            Serial.println(F("RTC OK"));
                                                            
                                                              // Initialize SHTC3 sensor
                                                                if (shtc3.begin() != SHTC3_Status_Nominal) {
                                                                    Serial.println(F("SHTC3 fail"));
                                                                        while (1);
                                                                          }
                                                                            Serial.println(F("SHTC3 OK"));
                                                                            
                                                                              Serial.println(F("Pump and Fan activated. Starting monitoring..."));
                                                                                lastReadTime = millis();
                                                                                }
                                                                                
                                                                                void loop() {
                                                                                  unsigned long currentMillis = millis();
                                                                                  
                                                                                    // Check if it's time to read the sensor
                                                                                      if (currentMillis - lastReadTime >= READ_INTERVAL) {
                                                                                          lastReadTime = currentMillis;
                                                                                          
                                                                                              // Read sensor
                                                                                                  int sensorStatus = shtc3.update();
                                                                                                      if (sensorStatus != SHTC3_Status_Nominal) {
                                                                                                            Serial.print(F("Sensor read failed, status="));
                                                                                                                  Serial.println(sensorStatus);
                                                                                                                        return;
                                                                                                                            }
                                                                                                                            
                                                                                                                                // Get temperature and humidity
                                                                                                                                    float temp = shtc3.toDegC();
                                                                                                                                        float humidity = shtc3.toPercent();
                                                                                                                                        
                                                                                                                                            // Print to serial monitor
                                                                                                                                                Serial.print(F("Temp: "));
                                                                                                                                                    Serial.print(temp);
                                                                                                                                                        Serial.print(F(" °C, Humidity: "));
                                                                                                                                                            Serial.print(humidity);
                                                                                                                                                                Serial.println(F(" %"));
                                                                                                                                                                  }
                                                                                                                                                                  }*/