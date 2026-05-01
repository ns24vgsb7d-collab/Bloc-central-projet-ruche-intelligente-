# Bloc Central — Ruche Connectée

Nœud central du système de surveillance de ruche connectée (PFE).  
Développé sur **Adafruit Feather ESP32-S3** avec PlatformIO.

---

## Rôle

Le bloc central agit comme **point de convergence** entre les capteurs de la ruche et la passerelle serveur :

1. Crée un **point d'accès WiFi** auquel se connectent les ESP32 clients
2. Reçoit leurs données via **TCP** et les agrège
3. Transmet la trame consolidée via **LoRa P2P** vers une passerelle reliée au serveur

---

## Architecture du système

```
ESP1 (temp, hum, poids)  ┐
ESP2 (SoC batterie)      ├──── WiFi TCP ────► B_central ──── LoRa 915 MHz ────► Passerelle ──► Serveur
ESP3 (GPS, optionnel)    │                  ESP32-S3 +
ESP4 (fréquence acous.)  ┘                  Wio-E5
```

---

## Clients ESP32

| Client | Préfixe | Données envoyées         | Obligatoire |
|--------|---------|--------------------------|-------------|
| ESP1   | `ESP1:` | `temp;hum;poids`         | Oui         |
| ESP2   | `ESP2:` | `SoC`                    | Oui         |
| ESP3   | `ESP3:` | `lat;lon`                | Non (GPS)   |
| ESP4   | `ESP4:` | `freq`                   | Oui         |

Chaque client envoie une ligne texte terminée par `\n` :
```
ESP1:24.5;60.2;8.3
ESP2:87.50
ESP3:45.5023;-73.6142
ESP4:432.1
```

---

## Trame LoRa émise

```
temp;hum;freq;poids;SoC[;lat;lon]
```

- La position GPS est incluse uniquement si ESP3 est connecté et actif
- La trame est encodée en hexadécimal pour la commande `AT+TEST=TXLRPKT` du Wio-E5
- Payload limité à **100 bytes**

---

## Configuration réseau

| Paramètre   | Valeur          |
|-------------|-----------------|
| SSID        | `Ruche Wifi`    |
| Mot de passe| `Abeilles`      |
| IP du AP    | `192.168.4.1`   |
| Port TCP    | `5000`          |

---

## Configuration LoRa (Wio-E5)

| Paramètre    | Valeur     |
|--------------|------------|
| Fréquence    | 915 MHz    |
| Mode         | P2P (TEST) |
| Spreading Factor | SF12   |
| Bandwidth    | 125 kHz    |
| Coding Rate  | 4/8        |
| Puissance TX | 14 dBm     |
| UART         | 9600 baud, pins RX/TX de la Feather |

> Pour l'Europe, changer la fréquence à 868 MHz dans `P2P_RFCFG` — `Central.cpp` ligne 4.

---

## LED témoin d'état

| État                  | Comportement LED          |
|-----------------------|---------------------------|
| Démarrage             | Clignotement rapide irrégulier |
| Création du AP        | Clignotement rapide régulier   |
| AP prêt, sans client  | Flash court toutes les secondes|
| Client(s) connecté(s) | LED fixe allumée               |
| Erreur critique       | Clignotement très rapide       |

---

## Architecture logicielle (FreeRTOS)

| Tâche        | Core | Rôle                                              |
|--------------|------|---------------------------------------------------|
| `wifiTask`   | 0    | Crée le AP, surveille les connexions              |
| `tcpTask`    | 0    | Reçoit et dispatche les trames des ESP clients    |
| `loraTask`   | 0    | Initialise le Wio-E5, lit les réponses UART       |
| `loraTxTask` | 0    | Dépile la queue et transmet via `sendP2P()`       |
| `ledTask`    | 1    | Clignote selon l'état du système                  |

---

## Ajouter un nouveau capteur

1. Définir un nouveau préfixe `ESP5:` côté client
2. Ajouter `_dataESP5` et `_hasESP5` dans `Central.h`
3. Dupliquer un bloc `else if` dans `tcpTask()` — `Central.cpp`
4. Modifier `tryAggregate()` pour inclure la nouvelle donnée dans la trame

---

## Dépendances PlatformIO

```ini
[env:adafruit_feather_esp32s3]
platform = espressif32
board = adafruit_feather_esp32s3
framework = arduino
lib_deps = WiFi
```

---

## Auteurs

Projet de fin d'études — Système de ruche connectée  
