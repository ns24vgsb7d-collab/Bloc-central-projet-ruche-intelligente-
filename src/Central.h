#ifndef CENTRAL_H
#define CENTRAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <HardwareSerial.h>

// États du point d'accès WiFi — reflétés sur la LED
enum wifi_stage_t {
  WIFI_STAGE_BOOT = 0,        // démarrage
  WIFI_STAGE_STARTING_AP,     // création du AP en cours
  WIFI_STAGE_AP_READY,        // AP actif, aucun client connecté
  WIFI_STAGE_CLIENT_CONNECTED,// au moins un ESP client connecté
  WIFI_STAGE_ERROR            // échec critique
};

// États du module LoRa Wio-E5
enum lora_stage_t {
  LORA_STAGE_INIT = 0,        // non initialisé
  LORA_STAGE_CONFIGURING,     // séquence AT en cours
  LORA_STAGE_READY,           // prêt à transmettre
  LORA_STAGE_ERROR            // module non répondant
};

/*
 * CentralWifi — nœud central du système ruche connectée.
 *
 * Rôle : créer un point d'accès WiFi auquel se connectent les ESP clients
 * (capteurs), recevoir leurs données via TCP, agréger la trame et la
 * transmettre en LoRa P2P vers la passerelle serveur.
 *
 * Clients attendus :
 *   ESP1 → "ESP1:temp;hum;poids\n"       (température, humidité, poids)
 *   ESP2 → "ESP2:SoC\n"                  (état de charge batterie, INA219)
 *   ESP3 → "ESP3:lat;lon\n"              (GPS — optionnel, actif en continu)
 *   ESP4 → "ESP4:freq\n"                 (fréquence acoustique de la ruche)
 *
 * Trame LoRa émise : "temp;hum;freq;poids;SoC[;lat;lon]"
 * Format ASCII, encodé en hex pour la commande AT+TEST=TXLRPKT du Wio-E5.
 *
 * Pour ajouter un nouveau capteur : créer un nouveau préfixe ESPx dans
 * tcpTask(), ajouter _dataESPx/_hasESPx, et modifier tryAggregate().
 */
class CentralWifi {
public:
  // ledPin    : LED témoin d'état (LED_BUILTIN recommandé)
  // ssid/pass : identifiants du point d'accès WiFi créé par ce central
  // tcpPort   : port d'écoute TCP (défaut 5000)
  CentralWifi(gpio_num_t ledPin,
              const char* ssid,
              const char* password,
              uint16_t tcpPort = 5000,
              uint8_t channel = 1,
              bool hidden = false,
              uint8_t maxConn = 4);

  // Lance les tâches FreeRTOS (à appeler dans setup())
  void begin();

  // Doit être appelé après Serial.begin() pour activer les logs
  void setSerialReady(bool ready);

  // Associe l'UART connecté au module LoRa Wio-E5
  void setupLora(HardwareSerial* loraSerial);

  // État courant du WiFi et du LoRa (utile pour le heartbeat dans loop())
  wifi_stage_t stage() const;
  lora_stage_t loraStage() const;
  uint8_t connectedStations() const;

private:
  // --- Paramètres WiFi ---
  gpio_num_t _ledPin;
  const char* _ssid;
  const char* _password;
  uint16_t _tcpPort;
  uint8_t _channel;
  bool _hidden;
  uint8_t _maxConn;

  volatile wifi_stage_t _stage;
  volatile uint8_t _staCount;
  volatile bool _serialReady;

  WiFiServer* _tcpServer = nullptr;

  // --- Handles des tâches FreeRTOS ---
  TaskHandle_t _wifiTaskHandle = nullptr;
  TaskHandle_t _ledTaskHandle  = nullptr;
  TaskHandle_t _tcpTaskHandle  = nullptr;
  TaskHandle_t _loraTaskHandle = nullptr;
  TaskHandle_t _loraTxTaskHandle = nullptr;

  // Trampolines statiques requis par xTaskCreatePinnedToCore
  static void wifiTaskTrampoline(void* arg);
  static void ledTaskTrampoline(void* arg);
  static void tcpTaskTrampoline(void* arg);
  static void loraTaskTrampoline(void* arg);
  static void loraTxTaskTrampoline(void* arg);

  // --- Tâches ---
  void wifiTask();   // crée le AP et surveille les connexions
  void ledTask();    // clignote selon l'état du système
  void tcpTask();    // reçoit les trames des ESP clients
  void loraTask();   // lit les réponses brutes du module Wio-E5
  void loraTxTask(); // dépile la queue et appelle sendP2P()

  // --- LoRa Wio-E5 ---
  HardwareSerial* _loraSerial = nullptr;
  volatile lora_stage_t _loraStage;
  SemaphoreHandle_t _loraMutex = nullptr; // protège l'accès concurrent à _loraSerial

  bool initializeLoraP2P();                                               // séquence AT de démarrage
  bool sendATCommand(const String& cmd, const String& expectedResp, uint32_t timeoutMs);
  String readLoraResponse(uint32_t timeoutMs);
  String toHex(const uint8_t* data, size_t len); // encode ASCII → HEX pour TXLRPKT
  bool sendP2P(const String& ascii);             // construit et envoie la commande AT

  void setStage(wifi_stage_t s);
  void setLoraStage(lora_stage_t s);

  // Queue inter-tâches : tcpTask produit, loraTxTask consomme (char* alloués sur le tas)
  QueueHandle_t _tcpToLoraQ = nullptr;

  // --- Données reçues des ESP clients ---
  // ESP3 est optionnel : la trame LoRa est envoyée même sans position GPS
  String _dataESP1; // temp;hum;poids
  String _dataESP2; // SoC
  String _dataESP3; // lat;lon (GPS)
  String _dataESP4; // fréquence acoustique
  bool   _hasESP1 = false;
  bool   _hasESP2 = false;
  bool   _hasESP3 = false;
  bool   _hasESP4 = false;

  SemaphoreHandle_t _aggMutex = nullptr; // protège les variables _dataESPx/_hasESPx
  void tryAggregate(); // appelé après chaque réception, envoie si ESP1+ESP2+ESP4 complets
};

#endif
