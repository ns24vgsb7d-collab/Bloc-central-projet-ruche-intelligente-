// ========================= main.cpp =========================
#include <Arduino.h>
#include <HardwareSerial.h>
#include "central.h"

static constexpr gpio_num_t LED_PIN = (gpio_num_t)LED_BUILTIN;
static const char* AP_SSID = "Ruche Wifi";
static const char* AP_PASS = "Abeilles"; // >=8

CentralWifi central(LED_PIN, AP_SSID, AP_PASS, 5000);

HardwareSerial LoraSerial(1);


void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 5000) delay(10);

  Serial.println("\n=== SYSTEM BOOT ===");

  // UART Wio-E5 sur pins RX/TX sérigraphiées de la Feather
  LoraSerial.begin(9600, SERIAL_8N1, RX, TX);
  Serial.println("LoRa UART started @115200");

  central.setupLora(&LoraSerial);

  central.setSerialReady(true);
  central.begin();

  Serial.println("=== SYSTEM STARTED ===");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 5000) {
    last = millis();
    Serial.printf("[HB] WiFi=%d Clients=%d LoRa=%d\n",
                  (int)central.stage(),
                  (int)central.connectedStations(),
                  (int)central.loraStage());
  }
  delay(500);
}
