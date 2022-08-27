#include <math.h>
#include <SPI.h>
#include <WiFiNINA.h>

#include "secrets.h"
#include "telemetry.h"

// Change if you connect the sensor to a diferent input
const int pinTempSensor = A0;

//Temperature sensor configurations
//Change according with the temperature sensor you have.
//Beta value of the thermistor
const int B = 4250;
//Value of the resistor RT0 at 25C = 100k ohm
const int RT0 = 100000;
const int VOLTAGE_DIVIDER_KNOWN_RESISTANCE_VALUE = 100000;

//Consts
const float KELVIN_CONVERSION_FACTOR = 273.15;
const float REFERENCE_TEMPERATURE_K = 25.0 + KELVIN_CONVERSION_FACTOR;

static void connect_to_wifi();
static float read_temperature();

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(LEDR, OUTPUT);
  pinMode(LEDG, OUTPUT);
  pinMode(LEDB, OUTPUT);
  digitalWrite(LEDG, LOW);
  digitalWrite(LEDR, LOW);
  digitalWrite(LEDB, LOW);
  Serial.begin(9600);

  //Connect to wifi network
  connect_to_wifi();
  sync_ntp();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connect_to_wifi();
  }

  float temperature = read_temperature();

  Serial.print("temperature = ");
  Serial.println(temperature);
  upload_metric("temperature", temperature);

  telemetry_poll();

  delay(1000);
}

static float read_temperature() {
  int analogReadValue = analogRead(pinTempSensor);

  // Forces the division to be done with floats, otherwise it may result in 0
  float rThermistor = VOLTAGE_DIVIDER_KNOWN_RESISTANCE_VALUE * (1023.0 / analogReadValue - 1);
  float temperature = 1.0 / (log(rThermistor / RT0) / B + 1 / REFERENCE_TEMPERATURE_K);
  //Convert to Celsius
  return temperature - KELVIN_CONVERSION_FACTOR;
}

/**
Checks if the Wifi connection is active. If it isn't tries to connect to the network
*/
static void connect_to_wifi() {
  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed! Stop everything");
    while (1) {
      digitalWrite(LEDR, HIGH);
      delay(500);
      digitalWrite(LEDR, LOW);
      delay(500);
    }
  }

  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade the firmware");
  }

  // attempt to connect to WiFi network:
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print("Attempting to connect to WPA SSID: ");
    Serial.println(WIFI_SSID);
    // Connect to WPA/WPA2 network:
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    // wait 10 seconds for connection:
    delay(10000);
  }

  Serial.print("You're connected to the network with the following Ip address: ");
  Serial.println(WiFi.localIP());
}

void sync_ntp() {
  while (WiFi.getTime() == 0) {
    delay(500);
    Serial.println("Trying to initialize time from ntp");
  }
}