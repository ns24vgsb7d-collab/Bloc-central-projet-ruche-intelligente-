#include "central.h"

// Commande de configuration radio du Wio-E5 en mode P2P (TEST)
// Paramètres : fréquence 915 MHz, SF12, BW125, CR4/8, puissance 14 dBm
// Modifier ici pour changer la bande (ex: 868 MHz pour l'Europe)
static const char* P2P_RFCFG = "AT+TEST=RFCFG,915000000,12,125,8,14,ON,OFF,OFF";

// Nombre maximum de clients TCP simultanés (un par ESP client)
static const uint8_t MAX_TCP_CLIENTS = 4;

// Taille du buffer de réception par ligne TCP
static const size_t TCP_LINE_BUFFER_SIZE = 256;
static const size_t TCP_MAX_LINE_LENGTH  = 512;

// Limite du payload LoRa en bytes (contrainte du Wio-E5 en mode TEST)
static const size_t LORA_MAX_PAYLOAD = 100;

// Capacité de la queue entre tcpTask et loraTxTask
static const size_t LORA_QUEUE_SIZE = 50;

// ─────────────────────────────────────────────────────────────────────────────
// Constructeur
// ─────────────────────────────────────────────────────────────────────────────
CentralWifi::CentralWifi(gpio_num_t ledPin,
                         const char* ssid,
                         const char* password,
                         uint16_t tcpPort,
                         uint8_t channel,
                         bool hidden,
                         uint8_t maxConn)
: _ledPin(ledPin),
  _ssid(ssid),
  _password(password),
  _tcpPort(tcpPort),
  _channel(channel),
  _hidden(hidden),
  _maxConn(maxConn),
  _stage(WIFI_STAGE_BOOT),
  _staCount(0),
  _serialReady(false),
  _loraStage(LORA_STAGE_INIT),
  _tcpToLoraQ(nullptr),
  _wifiTaskHandle(nullptr),
  _ledTaskHandle(nullptr),
  _tcpTaskHandle(nullptr),
  _loraTaskHandle(nullptr),
  _loraTxTaskHandle(nullptr),
  _tcpServer(nullptr),
  _loraSerial(nullptr),
  _hasESP1(false),
  _hasESP2(false),
  _hasESP3(false) {

  _loraMutex = xSemaphoreCreateMutex();
  if (_loraMutex == nullptr) {
    if (_serialReady) Serial.println("ERROR: Failed to create LoRa mutex");
  }

  _aggMutex = xSemaphoreCreateMutex();
  if (_aggMutex == nullptr) {
    if (_serialReady) Serial.println("ERROR: Failed to create aggregation mutex");
  }

  // La queue transporte des pointeurs char* alloués dynamiquement dans tcpTask
  // et libérés dans loraTxTask après envoi
  _tcpToLoraQ = xQueueCreate(LORA_QUEUE_SIZE, sizeof(char*));
  if (_tcpToLoraQ == nullptr) {
    if (_serialReady) Serial.println("ERROR: Failed to create TCP->LoRa queue");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Accesseurs publics
// ─────────────────────────────────────────────────────────────────────────────
void CentralWifi::setSerialReady(bool ready) { _serialReady = ready; }
wifi_stage_t CentralWifi::stage() const { return _stage; }
lora_stage_t CentralWifi::loraStage() const { return _loraStage; }
uint8_t CentralWifi::connectedStations() const { return _staCount; }
void CentralWifi::setStage(wifi_stage_t s) { _stage = s; }
void CentralWifi::setLoraStage(lora_stage_t s) { _loraStage = s; }

void CentralWifi::setupLora(HardwareSerial* loraSerial) {
  _loraSerial = loraSerial;
}

// ─────────────────────────────────────────────────────────────────────────────
// begin() — lance toutes les tâches FreeRTOS
// ─────────────────────────────────────────────────────────────────────────────
void CentralWifi::begin() {
  pinMode((int)_ledPin, OUTPUT);
  digitalWrite((int)_ledPin, LOW);

  // Core 0 : tâches réseau et LoRa (libèrent le core 1 à Arduino)
  xTaskCreatePinnedToCore(CentralWifi::wifiTaskTrampoline, "wifiTask", 4096, this, 2, &_wifiTaskHandle, 0);
  xTaskCreatePinnedToCore(CentralWifi::ledTaskTrampoline,  "ledTask",  2048, this, 1, &_ledTaskHandle,  1);

  if (_loraSerial != nullptr) {
    xTaskCreatePinnedToCore(CentralWifi::loraTaskTrampoline, "loraTask", 4096, this, 2, &_loraTaskHandle, 0);
  }
}

// Trampolines : xTaskCreatePinnedToCore exige une fonction statique ;
// chacun redirige vers la méthode d'instance correspondante
void CentralWifi::loraTxTaskTrampoline(void* arg) { static_cast<CentralWifi*>(arg)->loraTxTask(); }
void CentralWifi::wifiTaskTrampoline(void* arg)   { static_cast<CentralWifi*>(arg)->wifiTask(); }
void CentralWifi::ledTaskTrampoline(void* arg)    { static_cast<CentralWifi*>(arg)->ledTask(); }
void CentralWifi::tcpTaskTrampoline(void* arg)    { static_cast<CentralWifi*>(arg)->tcpTask(); }
void CentralWifi::loraTaskTrampoline(void* arg)   { static_cast<CentralWifi*>(arg)->loraTask(); }

// ─────────────────────────────────────────────────────────────────────────────
// wifiTask — crée le point d'accès et surveille les connexions clients
// ─────────────────────────────────────────────────────────────────────────────
void CentralWifi::wifiTask() {
  setStage(WIFI_STAGE_BOOT);

  // Attendre que Serial soit prêt avant de loguer (max 8s)
  uint32_t t0 = millis();
  while (!_serialReady && (millis() - t0) < 8000) vTaskDelay(pdMS_TO_TICKS(50));
  if (_serialReady) Serial.println("wifiTask started");

  setStage(WIFI_STAGE_STARTING_AP);

  WiFi.mode(WIFI_MODE_AP);
  bool ok = WiFi.softAP(_ssid, _password, _channel, _hidden, _maxConn);
  if (!ok) {
    setStage(WIFI_STAGE_ERROR);
    if (_serialReady) Serial.println("ERROR: WiFi.softAP failed (password>=8?)");
    vTaskDelete(nullptr);
    return;
  }

  setStage(WIFI_STAGE_AP_READY);

  // Lancer loraTxTask seulement quand le AP et la queue sont prêts
  if (_tcpToLoraQ != nullptr && _loraSerial != nullptr && _loraTxTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(CentralWifi::loraTxTaskTrampoline,
                            "loraTx", 4096, this, 2, &_loraTxTaskHandle, 0);
  }

  if (_serialReady) {
    Serial.println("AP started");
    Serial.print("AP SSID: "); Serial.println(WiFi.softAPSSID());
    Serial.print("AP IP:   "); Serial.println(WiFi.softAPIP());
    Serial.print("TCP port: "); Serial.println(_tcpPort);
  }

  if (_tcpTaskHandle == nullptr) {
    xTaskCreatePinnedToCore(CentralWifi::tcpTaskTrampoline, "tcpTask", 4096, this, 2, &_tcpTaskHandle, 0);
  }

  // Boucle de supervision : mise à jour du compteur de stations toutes les 5s
  for (;;) {
    _staCount = WiFi.softAPgetStationNum();
    setStage((_staCount > 0) ? WIFI_STAGE_CLIENT_CONNECTED : WIFI_STAGE_AP_READY);

    if (_serialReady) {
      Serial.printf("WiFi Stage=%d  Clients=%d  LoRa Stage=%d\n",
                    (int)_stage, (int)_staCount, (int)_loraStage);
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// tryAggregate — assemble la trame et la pousse dans la queue LoRa
//
// Appelé sous _aggMutex après chaque réception d'un ESP client.
// Ne fait rien tant que ESP1, ESP2 et ESP4 n'ont pas tous envoyé leurs données.
// ESP3 (GPS) est optionnel : la position est incluse si disponible.
//
// Trame produite : "temp;hum;freq;poids;SoC[;lat;lon]"
// ─────────────────────────────────────────────────────────────────────────────
void CentralWifi::tryAggregate() {
  if (!_hasESP1 || !_hasESP2 || !_hasESP4) return;

  // ESP1 envoie "temp;hum;poids" et ESP4 envoie "freq"
  // On insère freq entre hum et poids pour respecter le format attendu côté serveur
  int sep = _dataESP1.lastIndexOf(';');
  String esp1_head = _dataESP1.substring(0, sep);   // "temp;hum"
  String esp1_tail = _dataESP1.substring(sep + 1);  // "poids"
  String full = esp1_head + ";" + _dataESP4 + ";" + esp1_tail + ";" + _dataESP2;
  if (_hasESP3) full += ";" + _dataESP3;

  if (_serialReady) {
    Serial.print("AGG -> LoRa: ");
    Serial.println(full);
  }

  // Allouer dynamiquement : loraTxTask libère ce pointeur après envoi
  if (_tcpToLoraQ != nullptr) {
    char* msg = (char*)malloc(full.length() + 1);
    if (msg != nullptr) {
      strcpy(msg, full.c_str());
      if (xQueueSend(_tcpToLoraQ, &msg, 0) != pdTRUE) {
        free(msg);
        if (_serialReady) Serial.println("WARN: LoRa queue full, aggregated message dropped");
      }
    } else {
      if (_serialReady) Serial.println("ERROR: Failed to allocate memory for aggregated message");
    }
  }

  // Réinitialiser les flags pour le prochain cycle de collecte
  _hasESP1 = _hasESP2 = _hasESP3 = _hasESP4 = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// tcpTask — reçoit les données des ESP clients via TCP
//
// Chaque ESP envoie une ligne terminée par '\n' avec son préfixe :
//   ESP1:temp;hum;poids
//   ESP2:SoC
//   ESP3:lat;lon      (GPS, optionnel)
//   ESP4:freq
//
// Pour ajouter un ESP supplémentaire : dupliquer un bloc "else if" et
// ajouter les variables _dataESPx/_hasESPx correspondantes dans Central.h
// ─────────────────────────────────────────────────────────────────────────────
void CentralWifi::tcpTask() {
  _tcpServer = new WiFiServer(_tcpPort);
  if (_tcpServer == nullptr) {
    if (_serialReady) Serial.println("ERROR: Failed to create TCP server");
    setStage(WIFI_STAGE_ERROR);
    vTaskDelete(nullptr);
    return;
  }

  _tcpServer->begin();
  _tcpServer->setNoDelay(true);

  if (_serialReady) {
    Serial.print("TCP server listening on port ");
    Serial.println(_tcpPort);
  }

  WiFiClient clients[MAX_TCP_CLIENTS];
  String rxLine[MAX_TCP_CLIENTS];

  for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
    rxLine[i].reserve(TCP_LINE_BUFFER_SIZE);
  }

  for (;;) {
    // Accepter les nouvelles connexions entrantes
    WiFiClient incoming = _tcpServer->available();
    while (incoming) {
      int slot = -1;
      for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (!clients[i] || !clients[i].connected()) { slot = i; break; }
      }

      if (slot >= 0) {
        clients[slot].stop();
        clients[slot] = incoming;
        clients[slot].setNoDelay(true);
        rxLine[slot] = "";
        if (_serialReady) {
          Serial.print("TCP client["); Serial.print(slot);
          Serial.print("] connected from ");
          Serial.println(clients[slot].remoteIP());
        }
      } else {
        if (_serialReady) Serial.println("TCP: server full, rejecting client");
        incoming.stop();
      }
      incoming = _tcpServer->available();
    }

    // Lire et traiter les données de chaque client connecté
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
      if (!clients[i] || !clients[i].connected()) continue;

      while (clients[i].available()) {
        char c = (char)clients[i].read();

        if (c == '\n') {
          rxLine[i].trim();
          if (rxLine[i].length() > 0) {
            if (_serialReady) {
              Serial.print("TCP RX ["); Serial.print(i); Serial.print("]: ");
              Serial.println(rxLine[i]);
            }

            // Dispatcher selon le préfixe de l'ESP source
            if (rxLine[i].startsWith("ESP1:")) {
              if (_aggMutex && xSemaphoreTake(_aggMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                _dataESP1 = rxLine[i].substring(5);
                _hasESP1 = true;
                tryAggregate();
                xSemaphoreGive(_aggMutex);
              }
            } else if (rxLine[i].startsWith("ESP2:")) {
              if (_aggMutex && xSemaphoreTake(_aggMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                _dataESP2 = rxLine[i].substring(5);
                _hasESP2 = true;
                tryAggregate();
                xSemaphoreGive(_aggMutex);
              }
            } else if (rxLine[i].startsWith("ESP3:")) {
              if (_aggMutex && xSemaphoreTake(_aggMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                _dataESP3 = rxLine[i].substring(5);
                _hasESP3 = true;
                tryAggregate();
                xSemaphoreGive(_aggMutex);
              }
            } else if (rxLine[i].startsWith("ESP4:")) {
              if (_aggMutex && xSemaphoreTake(_aggMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                _dataESP4 = rxLine[i].substring(5);
                _hasESP4 = true;
                tryAggregate();
                xSemaphoreGive(_aggMutex);
              }
            } else {
              if (_serialReady) Serial.println("WARN: Unknown ESP source, message ignored");
            }
          }
          rxLine[i] = "";
        } else if (c != '\r') {
          if (rxLine[i].length() < TCP_MAX_LINE_LENGTH) {
            rxLine[i] += c;
          } else {
            if (_serialReady) Serial.println("WARN: TCP line too long, buffer cleared");
            rxLine[i] = "";
          }
        }
      }

      if (!clients[i].connected()) {
        clients[i].stop();
        if (_serialReady) {
          Serial.print("TCP client["); Serial.print(i); Serial.println("] disconnected");
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers LoRa Wio-E5
// ─────────────────────────────────────────────────────────────────────────────

// Lit la réponse du module sur l'UART pendant timeoutMs millisecondes
String CentralWifi::readLoraResponse(uint32_t timeoutMs) {
  String resp;
  resp.reserve(256);
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (_loraSerial && _loraSerial->available()) {
      char c = (char)_loraSerial->read();
      if (resp.length() < 512) resp += c;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return resp;
}

// Envoie une commande AT et vérifie la présence de expectedResp dans la réponse.
// Utilise _loraMutex pour éviter les collisions entre loraTask et loraTxTask.
bool CentralWifi::sendATCommand(const String& cmd, const String& expectedResp, uint32_t timeoutMs) {
  if (_loraSerial == nullptr) {
    if (_serialReady) Serial.println("ERROR: LoRa serial not initialized");
    return false;
  }

  if (_loraMutex == nullptr || xSemaphoreTake(_loraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    if (_serialReady) Serial.println("ERROR: Failed to acquire LoRa mutex");
    return false;
  }

  while (_loraSerial->available()) _loraSerial->read(); // vider le buffer avant envoi

  _loraSerial->print(cmd);
  _loraSerial->print("\r\n");

  if (_serialReady) { Serial.print("LoRa TX: "); Serial.println(cmd); }

  String resp = readLoraResponse(timeoutMs);

  if (_serialReady) { Serial.print("LoRa RX: "); Serial.println(resp); }

  xSemaphoreGive(_loraMutex);

  if (expectedResp.length() == 0) return resp.length() > 0;
  return resp.indexOf(expectedResp) >= 0;
}

// Séquence d'initialisation : AT → ATE0 → MODE=TEST → RFCFG
// À relancer si le module est remplacé ou si la bande fréquence change
bool CentralWifi::initializeLoraP2P() {
  if (_serialReady) Serial.println("Initializing LoRa P2P (TEST mode)...");
  setLoraStage(LORA_STAGE_CONFIGURING);

  if (!sendATCommand("AT", "+AT: OK", 2000)) {
    if (_serialReady) Serial.println("ERROR: LoRa module not responding to AT");
    setLoraStage(LORA_STAGE_ERROR);
    return false;
  }

  sendATCommand("ATE0", "", 800); // désactive l'écho du module

  if (!sendATCommand("AT+MODE=TEST", "", 1500)) {
    if (_serialReady) Serial.println("ERROR: Failed to enter TEST mode");
    setLoraStage(LORA_STAGE_ERROR);
    return false;
  }

  if (!sendATCommand(P2P_RFCFG, "", 1500)) {
    if (_serialReady) Serial.println("ERROR: RFCFG configuration failed");
    setLoraStage(LORA_STAGE_ERROR);
    return false;
  }

  setLoraStage(LORA_STAGE_READY);
  if (_serialReady) Serial.println("LoRa P2P READY (915 MHz)");
  return true;
}

// Encode un tableau d'octets en chaîne hexadécimale (ex: "AB" → "4142")
// Requis par la commande AT+TEST=TXLRPKT qui attend le payload en hex
String CentralWifi::toHex(const uint8_t* data, size_t len) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += hex[(data[i] >> 4) & 0x0F];
    out += hex[data[i] & 0x0F];
  }
  return out;
}

// Envoie un message texte via LoRa P2P (Wio-E5 en mode TEST)
// Le payload est tronqué à LORA_MAX_PAYLOAD si nécessaire
bool CentralWifi::sendP2P(const String& ascii) {
  if (_loraSerial == nullptr) {
    if (_serialReady) Serial.println("ERROR: LoRa serial not initialized");
    return false;
  }

  String payload = ascii;
  if (payload.length() > LORA_MAX_PAYLOAD) {
    payload = payload.substring(0, LORA_MAX_PAYLOAD);
    if (_serialReady) Serial.println("WARN: Payload truncated to 100 bytes");
  }

  String hexData = toHex((const uint8_t*)payload.c_str(), payload.length());
  String cmd = "AT+TEST=TXLRPKT,\"" + hexData + "\"";

  bool ok = sendATCommand(cmd, "", 3000);

  if (_serialReady) {
    Serial.print("LoRa P2P TX: "); Serial.println(payload);
    if (!ok) Serial.println("WARN: LoRa TX may have failed");
  }

  return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// loraTask — initialise le module puis surveille les réponses non sollicitées
// (ex: confirmations de TX, trames reçues en mode RX si activé plus tard)
// ─────────────────────────────────────────────────────────────────────────────
void CentralWifi::loraTask() {
  vTaskDelay(pdMS_TO_TICKS(3000)); // laisser le temps au AP de démarrer

  if (!initializeLoraP2P()) {
    if (_serialReady) Serial.println("ERROR: LoRa initialization failed!");
    setLoraStage(LORA_STAGE_ERROR);
    vTaskDelete(nullptr);
    return;
  }

  for (;;) {
    if (_loraMutex && xSemaphoreTake(_loraMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      while (_loraSerial && _loraSerial->available()) {
        char c = (char)_loraSerial->read();
        if (_serialReady) Serial.write(c);
      }
      xSemaphoreGive(_loraMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// loraTxTask — consommateur de la queue _tcpToLoraQ
// Bloque sur xQueueReceive et envoie chaque message dès qu'il est disponible
// ─────────────────────────────────────────────────────────────────────────────
void CentralWifi::loraTxTask() {
  if (_serialReady) Serial.println("LoRa TX task started");

  for (;;) {
    char* msg = nullptr;
    if (_tcpToLoraQ && xQueueReceive(_tcpToLoraQ, &msg, portMAX_DELAY) == pdTRUE) {
      if (msg != nullptr) {
        if (_loraStage == LORA_STAGE_READY) {
          sendP2P(String(msg));
        } else {
          if (_serialReady) {
            Serial.print("WARN: LoRa not ready (stage=");
            Serial.print((int)_loraStage);
            Serial.println("), message dropped");
          }
        }
        free(msg); // libérer la mémoire allouée dans tryAggregate()
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// ledTask — indicateur visuel de l'état du système via la LED intégrée
//
// BOOT             : clignotement rapide irrégulier
// STARTING_AP      : clignotement rapide régulier
// AP_READY         : flash court toutes les secondes (en attente de clients)
// CLIENT_CONNECTED : LED fixe allumée
// ERROR            : clignotement très rapide
// ─────────────────────────────────────────────────────────────────────────────
void CentralWifi::ledTask() {
  for (;;) {
    switch (_stage) {
      case WIFI_STAGE_BOOT:
        digitalWrite((int)_ledPin, HIGH); vTaskDelay(pdMS_TO_TICKS(80));
        digitalWrite((int)_ledPin, LOW);  vTaskDelay(pdMS_TO_TICKS(200));
        break;
      case WIFI_STAGE_STARTING_AP:
        digitalWrite((int)_ledPin, HIGH); vTaskDelay(pdMS_TO_TICKS(150));
        digitalWrite((int)_ledPin, LOW);  vTaskDelay(pdMS_TO_TICKS(150));
        break;
      case WIFI_STAGE_AP_READY:
        digitalWrite((int)_ledPin, HIGH); vTaskDelay(pdMS_TO_TICKS(100));
        digitalWrite((int)_ledPin, LOW);  vTaskDelay(pdMS_TO_TICKS(900));
        break;
      case WIFI_STAGE_CLIENT_CONNECTED:
        digitalWrite((int)_ledPin, HIGH);
        vTaskDelay(pdMS_TO_TICKS(500));
        break;
      case WIFI_STAGE_ERROR:
      default:
        digitalWrite((int)_ledPin, HIGH); vTaskDelay(pdMS_TO_TICKS(50));
        digitalWrite((int)_ledPin, LOW);  vTaskDelay(pdMS_TO_TICKS(50));
        break;
    }
  }
}
