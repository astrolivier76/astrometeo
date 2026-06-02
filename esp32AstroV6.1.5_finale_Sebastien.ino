#include <Arduino.h>
//#define SIMULATION_MODE
#define DEBUG_MODE
#define CHAUFFAGE
#define SHT                // A commenter egalement dans MLX90614 si non utilisée.
#include "debug.h"
#include "esp_system.h"
#include "driver/timer.h"
#include <freertos/semphr.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include "MQTT.h"
#include "WebSocket.h"

// Déclare un handle pour le watchdog matériel
hw_timer_t *watchdogTimer = NULL;

// Déclare le mutex pour protéger les données des capteurs
SemaphoreHandle_t xSensorDataMutex;

// Fonction de rappel pour le watchdog
void IRAM_ATTR resetModule() {
    ets_printf("Watchdog timeout! Redémarrage...\n");
    ESP.restart();
}

#include <Wire.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include "variablesWEB.h"
#include <Arduino_JSON.h>
JSONVar readings;
JSONVar ascomData;
// --- INCLUSION DES EN-TÊTES DES CAPTEURS ---
#include "BME280.h"
#ifdef SHT
#include "SHT.h"
#endif
#include "MLX90614.h"
#include "TSL2591.h"
#include "WIFI.h"
#include "SPI.h"
#include "anemo.h"
#include "pluviometre.h"
#include "gouttes.h"
#include "version.h"
#include "ota.h"
OTAHandler otaHandler;
// --- FIN INCLUSION DES EN-TÊTES DES CAPTEURS ---

//PARAMETRE POUR LE CHAUFFAFE SI ACTIVE
#ifdef CHAUFFAGE
const byte pinChauffage = 13;
unsigned long t_lastActionChauffage = 0;
int chauffage = 0;
#define INTERO_CHAUFFAGE 600 // DELAI POUR LA VERIFICATION DES CONDITIONS DE NECESSITE DU CHAUFFAGE TOUTES LES 60 SECONDES
#endif

// --- PARAMETRES POUR LE MODE ECONOMIE ---
#define SLEEP_TIMEOUT 120000  // 2 minutes d'inactivité avant mode économie
bool isInEconomyMode = false;
bool isInModemSleep = false;
unsigned long lastEconomyCheck = 0;
const unsigned long ECONOMY_CHECK_INTERVAL = 300000;
// Vérif toutes les 5min
unsigned long lastActivityTime = 0;

//PARAMETRES POUR LA DUREE DE MISE A JOUR DES VALEURS DANS LE CACHE DATA
#define cachedInterval 20000 //MISE A JOUR DU CACHE TOUTES LES 20s

// --- PARAMETRES POUR LE REDEMARRGAGE HEBDOMADAIRE DE L'ESP32 ET NETTOYAGE DES LOGS ---
unsigned long lastRebootCheck = 0;
unsigned long lastMemCheck = 0;
unsigned long lastLogCleanup = 0;
unsigned long lastHeartbeat = 0;

// --- PARAMETRES POUR LE HEARTBEAT ---
#define HEARTBEAT_INTERVAL 60000  // 1 minute

// --- PARAMETRES POUR LE MQTT ET LE WEBSOCKET
unsigned long lastMqttPublish = 0;
const unsigned long MQTT_PUBLISH_INTERVAL = 60000;  // 60 secondes
unsigned long lastBroadcast = 0;
#define WEBSOCKET_BROADCAST_INTERVAL 5000 // 5 secondes

// --- PARAMETRES POUR LA PUISSANCE WIFI ADAPTATIVE ---
#define RSSI_EXCELLENT -50    // RSSI excellent -> pleine puissance
#define RSSI_GOOD -65         // RSSI bon -> puissance moyenne
#define RSSI_FAIR -75         // RSSI acceptable -> puissance réduite
#define RSSI_POOR -85         // RSSI faible -> puissance minimale

#define WIFI_POWER_MAX 82     // 20dBm - puissance maximale
#define WIFI_POWER_MEDIUM 74  // 17dBm - puissance moyenne  
#define WIFI_POWER_LOW 62     
// 14dBm - puissance réduite
#define WIFI_POWER_MIN 52     // 11dBm - puissance minimale

// --- PARAMETRES FREQUENCE CPU ---
#define CPU_FREQ_LOW 80
#define CPU_FREQ_NORMAL 160
#define CPU_FREQ_HIGH 240

// --- INTERVALLES OPTIMISES ---
#define INTERO_VENT 5         // 5s
#define INTERO_PLUVIO 360     // 6min  
#define INTERO_GOUTTES 60     // 60s

unsigned long t_lastActionVent = 0;
unsigned long t_lastActionPluvio = 0;
unsigned long t_lastActionGouttes = 0;

// --- PARAMETRES WEB ---
AsyncWebServer server(80);
AsyncEventSource events("/events");
// --- FONCTIONS DE GESTION DE LA LECTURE DES CAPTEURS ET TRAITEMENT DES DONNEES ---
void updateSensorCache();
String getSensorReadings();
String generateASCOMData();
void handleWakeUpRequest();
void enterLightSleep();
void exitLightSleep();
void formatWebSocketData(const SensorData& data, char* buffer, size_t bufferSize);
void adjustWiFiPowerBasedOnRSSI();
void setupUnusedPins();
void setCPUFrequency(uint32_t freq_mhz);
void handleMillisOverflow();
void broadcastSensorData();

// Fonction pour ajuster la puissance WiFi en fonction du RSSI
void adjustWiFiPowerBasedOnRSSI() {
    if(!WiFi.isConnected()) return;
    int8_t rssi = WiFi.RSSI();
    uint8_t new_power = WIFI_POWER_MIN;
    
    if (rssi >= RSSI_EXCELLENT) {
        new_power = WIFI_POWER_LOW;
    } 
    else if (rssi >= RSSI_GOOD) {
        new_power = WIFI_POWER_LOW;
    }
    else if (rssi >= RSSI_FAIR) {
        new_power = WIFI_POWER_MIN;
    }
    else {
        new_power = WIFI_POWER_MIN;
    }
    
    esp_wifi_set_max_tx_power(new_power);
    DEBUG_PRINTF("RSSI: %ddB -> Puissance WiFi: %d\n", rssi, new_power);
}

// Fonction pour configurer les broches non utilisées
void setupUnusedPins() {
    const int unusedPins[] = {0, 2, 4, 5, 12, 15, 16, 17, 18, 19, 23, 25, 26, 33};
    for (int pin : unusedPins) {
        pinMode(pin, INPUT_PULLDOWN);
    }
    DEBUG_PRINTLN("Broches non utilisées configurées en input pull-down");
}

// Fonction pour gérer la fréquence CPU
void setCPUFrequency(uint32_t freq_mhz) {
    if(freq_mhz != getCpuFrequencyMhz()) {
        setCpuFrequencyMhz(freq_mhz);
        DEBUG_PRINTF("Fréquence CPU réglée à: %d MHz\n", getCpuFrequencyMhz());
    }
}

// Gestion optimisée de l'overflow millis
void handleMillisOverflow() {
    DEBUG_PRINTLN("Cycle millis() détecté, réinitialisation des horodatages...");
    unsigned long currentMillis = millis();
    lastActivityTime = currentMillis;
    cachedData.lastUpdate = 0;
    t_lastActionVent = currentMillis;
    t_lastActionPluvio = currentMillis;
    t_lastActionGouttes = currentMillis;
    #ifdef CHAUFFAGE
    t_lastActionChauffage = currentMillis;
    #endif
    lastRebootCheck = currentMillis;
    lastMemCheck = currentMillis;
    lastLogCleanup = currentMillis;
    lastHeartbeat = currentMillis;
    lastEconomyCheck = currentMillis;
}

void enterLightSleep() {
    if (!isInModemSleep) {
        DEBUG_PRINTLN("Activation du light sleep avec maintien WiFi");
// Configuration pour light sleep - WiFi reste connecté
        esp_sleep_enable_timer_wakeup(1000000);
// 1 seconde max
        esp_light_sleep_start();
// Réduction agressive de la puissance WiFi
        esp_wifi_set_max_tx_power(WIFI_POWER_MIN);
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
        
        isInModemSleep = true;
        DEBUG_PRINTLN("Light sleep activé - WiFi maintenu");
    }
}

void exitLightSleep() {
    if (isInModemSleep) {
        DEBUG_PRINTLN("Sortie du light sleep");
// Retour en mode normal
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        adjustWiFiPowerBasedOnRSSI();
        
        isInModemSleep = false;
        DEBUG_PRINTLN("Mode normal restauré");
// Mise à jour des données après le réveil
        updateSensorCache();
    }
}

// === FONCTIONS DE GESTION SLEEP DES CAPTEURS ===
void wakeAllSensors() {
    DEBUG_PRINTLN("Réveil de tous les capteurs...");
    wakeBME();
    #ifdef SHT
    wakeSHT();
    #endif
    wakeMLX();
    
    vTaskDelay(pdMS_TO_TICKS(30));
}

void sleepAllSensors() {
    DEBUG_PRINTLN("Mise en sleep de tous les capteurs...");
    sleepBME();
    #ifdef SHT
    sleepSHT();
    #endif
    sleepMLX();
}

bool areSensorsSleeping() {
    #ifdef SHT
    return isBMESleeping() && isSHTSleeping() && isMLXSleeping();
    #else
    return isBMESleeping() && isMLXSleeping();
    #endif
}

void setup() {
    Serial.begin(115200);
    Wire.begin();
// --- OPTIMISATIONS ÉNERGÉTIQUES ---
    btStop();
    
    // Configuration des broches non utilisées
    setupUnusedPins();
// Fréquence CPU réduite en normale et non au max (pas utile dans le cas d'une station météo et permet des économies d'energies)
    setCPUFrequency(CPU_FREQ_NORMAL);
    #ifdef DEBUG_MODE
    if (!btStarted()) {
        DEBUG_PRINTLN("Economie Bluetooth: OK");
    }
    DEBUG_PRINTLN("Optimisations énergétiques activées");
    #endif

    // --- Initialisation du watchdog matériel (30 secondes)---
    watchdogTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(watchdogTimer, &resetModule, true);
    timerAlarmWrite(watchdogTimer, 30000000, false);
    timerAlarmEnable(watchdogTimer);

    // --- INITIALISATIONS STANDARDS ---
    initWiFi();
    initSPIFFS();
    createConfigDir();
    loadConstantsFromFile();
// --- INITIALISATION DES CAPTEURS ---
    initBME();
    #ifdef SHT
    initSHT();
    #endif
    initMLX();
    init_vent();
    init_pluviometre();
    init_GOUTTES();

#ifdef CHAUFFAGE
    pinMode(pinChauffage, OUTPUT);
#endif

    // --- SETUP DU MUTEX ---
    xSensorDataMutex = xSemaphoreCreateMutex();
    if (xSensorDataMutex == NULL) {
        DEBUG_PRINTLN("Erreur: Impossible de créer le mutex");
    } else {
        DEBUG_PRINTLN("Mutex créé avec succès");
    }

    // --- SETUP MAJ PAR OTA ---
    otaHandler.begin(server);
// --- PREMIERE LECTURE DES CAPTEURS ---
    updateSensorCache();
    sleepAllSensors();
    DEBUG_PRINTLN("Capteurs mis en sleep après initialisation");
// --- CONFIGURATION DES ROUTES POUR LE SERVEUR WEB ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        request->send(SPIFFS, "/index.html");
    });
    server.on("/constantesmlx", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        handleConstantsPage(request);
    });
    server.on("/constantesmlx", HTTP_POST, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        K1 = request->arg("newK1").toFloat();
        K2 = request->arg("newK2").toFloat();
        K3 = request->arg("newK3").toFloat();
        K4 = request->arg("newK4").toFloat();
        K5 = request->arg("newK5").toFloat();
        K6 = request->arg("newK6").toFloat();
        K7 = request->arg("newK7").toFloat();

        // Sauvegarde des nouvelles constantes de température ciel
   
         if (request->hasArg("newTempClair")) {
            temperature_ciel_clair = request->arg("newTempClair").toFloat();
        }
        if (request->hasArg("newTempCouvert")) {
            temperature_ciel_couvert = request->arg("newTempCouvert").toFloat();
        }
        
        saveConstantsToFile();
        request->send(200, "text/plain", "OK");
    });
    server.on("/readings", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        String json = getSensorReadings();
        request->send(200, "application/json", json);
    });
    server.on("/ascom", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        String json = generateASCOMData();
        request->send(200, "application/json", json);
    });
    server.on("/version", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        String versionJson = "{\"version\": \"" + String(firmwareVersionAstroMeteo) + "\"}";
        request->send(200, "application/json", versionJson);
    });
    server.on("/ip", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        String ipJson = getLocalIPAddress();
        request->send(200, "application/json", ipJson);
    });
    server.on("/OTA.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        request->send(SPIFFS, "/OTA.html", String(), false);
    });
    server.on("/api/control", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        if (request->hasParam("mqtt")) {
            mqttEnabled = request->getParam("mqtt")->value() == "enable";
        }
        if (request->hasParam("websocket")) {
            websocketEnabled = request->getParam("websocket")->value() == "enable";
        }
        String response = "{\"mqtt\":\"" + String(mqttEnabled ? "enabled" : "disabled") + "\",\"websocket\":\"" 
        + String(websocketEnabled ? "enabled" : "disabled") + "\"}";
        request->send(200, "application/json", response);
    });
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        String response = "{\"mqtt_connected\":" + String(mqttClient.connected() ? "true" : "false") + ",\"websocket_clients\":" + String(ws.count()) + ",\"clients_connected\":" + String(ws.count()) + "}";
        request->send(200, "application/json", response);
    });
    server.addHandler(&events);
    server.serveStatic("/", SPIFFS, "/");
    listSPIFFSFiles("/");
    server.begin();

    // Initialisation WebSocket et MQTT
    initWebSocket();
    initMQTT();
    server.addHandler(&ws);
// Configuration initiale WiFi pour économie
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    adjustWiFiPowerBasedOnRSSI();
// --- INITIALISATION DES VARIABLES DE TIMING ---
    lastRebootCheck = millis();
    lastMemCheck = millis();
    lastLogCleanup = millis();
    lastHeartbeat = millis();
    lastActivityTime = millis();
    lastEconomyCheck = millis();

    DEBUG_PRINTLN("Initialisation complète avec optimisations énergétiques.");
}

void loop() {
    // --- RESET DU WATCHDOG ---
    timerWrite(watchdogTimer, 0);
    unsigned long currentMillis = millis();

    // --- Gestion proactive de l'overflow de millis() ---
    static unsigned long previousMillisCycle = 0;
    if (currentMillis < previousMillisCycle) {
        handleMillisOverflow();
    }
    previousMillisCycle = currentMillis;
// --- GESTION DU MODE ÉCONOMIE AVEC LIGHT SLEEP ---
    if (currentMillis - lastActivityTime >= SLEEP_TIMEOUT) {
        if (!isInEconomyMode) {
            // Entrée en mode économie
            DEBUG_PRINTLN("Entrée en mode économie d'énergie");
            isInEconomyMode = true;

// On s'assure d'éteindre le MOSFET avant de dormir pour éviter une chauffe infinie
            #ifdef CHAUFFAGE
            digitalWrite(pinChauffage, LOW);
            chauffage = 0;
            DEBUG_PRINTLN("Chauffage coupé par sécurité (Mode économie)");
            #endif            
            
            // Réduction fréquence CPU
            setCPUFrequency(CPU_FREQ_LOW);
// Arrêt des services non essentiels
            if (mqttClient.connected()) {
                mqttClient.disconnect();
            }
            DEBUG_PRINTLN("Services non essentiels arrêtés");
        }
        
        // Light sleep
        if (!isInModemSleep) {
            enterLightSleep();
        }
        
        // Vérifications très réduites en mode économie
        if (currentMillis - lastEconomyCheck >= ECONOMY_CHECK_INTERVAL) {
            lastEconomyCheck = currentMillis;
// Heartbeat très réduit
            if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL * 5) {
                lastHeartbeat = currentMillis;
                checkWiFi();
                DEBUG_PRINTLN("[Heartbeat] WiFi check (mode économie).");
            }
        }
        
        // Délai augmenté en mode économie
        vTaskDelay(pdMS_TO_TICKS(3000));
        return;
    } 
    else if (isInEconomyMode) {
        // Sortie du mode économie
        DEBUG_PRINTLN("Sortie du mode économie - Mode actif");
        isInEconomyMode = false;
        
        // Restauration fréquence CPU
        setCPUFrequency(CPU_FREQ_NORMAL);
// Sortie du light sleep
        if (isInModemSleep) {
            exitLightSleep();
        }
        
        // Redémarrage des services
        if (mqttEnabled && WiFi.status() == WL_CONNECTED) {
            reconnectMQTT();
        }
        
        lastHeartbeat = currentMillis;
        lastEconomyCheck = currentMillis;
    }

    // --- HEARTBEAT en mode normal ---
    if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL * 5) {
        lastHeartbeat = currentMillis;
        checkWiFi();
        adjustWiFiPowerBasedOnRSSI();
        DEBUG_PRINTLN("[Heartbeat] WiFi check avec ajustement de puissance.");
    }

    // --- Redémarrage hebdomadaire ---
    #define REBOOT_INTERVAL_MS 604800000UL
    if (currentMillis - lastRebootCheck >= REBOOT_INTERVAL_MS) {
        lastRebootCheck = currentMillis;
        DEBUG_PRINTLN("Redémarrage hebdomadaire planifié. Au revoir!");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
    }

    // --- Surveillance de la mémoire (moins fréquente) ---
    #define MEM_CHECK_INTERVAL 172800000UL    // 48h au lieu de 24h
    #define CRITICAL_HEAP_SIZE 100000
    if (currentMillis - lastMemCheck >= MEM_CHECK_INTERVAL) {
        lastMemCheck = currentMillis;
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t minHeap = ESP.getMinFreeHeap();
        DEBUG_PRINTF("[Mémoire] Libre: %u octets, Minimum historique: %u octets\n", freeHeap, minHeap);
        if (freeHeap < CRITICAL_HEAP_SIZE) {
            DEBUG_PRINTF("ERREUR: Mémoire critique (%u octets). Redémarrage immédiat.\n", freeHeap);
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP.restart();
        }
    }

    // --- Nettoyage périodique des logs OTA ---
    #define LOG_CLEANUP_INTERVAL 604800000UL
    if (currentMillis - lastLogCleanup >= LOG_CLEANUP_INTERVAL) {
        lastLogCleanup = currentMillis;
        otaHandler.cleanLogs();
        DEBUG_PRINTLN("Nettoyage périodique des logs OTA effectué.");
    }

    // --- TRAITEMENTS NORMAUX AVEC INTERVALLES RÉDUITS ---
    
    // Mise à jour du cache
    if (currentMillis - cachedData.lastUpdate >= cachedInterval) {
        updateSensorCache();
    }

    // Gestion MQTT et WebSocket
    handleMQTT();
    handleWebSocket();
// Publication MQTT moins fréquente
    if (currentMillis - lastMqttPublish >= MQTT_PUBLISH_INTERVAL * 2) {
        lastMqttPublish = currentMillis;
        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            publishSensorData(cachedData);
            xSemaphoreGive(xSensorDataMutex);
        }
    }

    // Broadcast WebSocket optimisé
    if (currentMillis - lastBroadcast >= WEBSOCKET_BROADCAST_INTERVAL) {
        lastBroadcast = currentMillis;
        broadcastSensorData();
    }

    // --- GESTION DES CAPTEURS AVEC INTERVALLES AUGMENTÉS ---
    
    // Anémomètre toutes les 5s
    if (currentMillis - t_lastActionVent >= INTERO_VENT * 1000) {
        t_lastActionVent = currentMillis;
        getSendVitesseVent(INTERO_VENT);
        
        // Injection immédiate de la nouvelle vitesse dans le cache pour ASCOM / NINA
        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            cachedData.vent = getVent();
            xSemaphoreGive(xSensorDataMutex);
        }
    }

    // Pluviomètre toutes les 6min
    if (currentMillis - t_lastActionPluvio >= INTERO_PLUVIO * 1000) {
        t_lastActionPluvio = currentMillis;
        getSendPluviometre(INTERO_PLUVIO);
    }

    // Capteur de gouttes toutes les 1min
    if (currentMillis - t_lastActionGouttes >= INTERO_GOUTTES * 1000) {
        t_lastActionGouttes = currentMillis;
        updateGOUTTES();
    }

    // Chauffage intelligent du capteur de pluie (synchronisé toutes les 1 min pour lutter contre la rosée)
    #ifdef CHAUFFAGE
    if (currentMillis - t_lastActionChauffage >= INTERO_GOUTTES * 1000) {
        t_lastActionChauffage = currentMillis;
        // On confie directement le calcul et la commutation matérielle à votre fonction dédiée
        gererChauffageRosse(cachedData.dewpoint);
    }
    #endif

    // Délai augmenté pour réduire la consommation
    vTaskDelay(pdMS_TO_TICKS(200));
}

// --- REVEIL DE L'ESP32 ---
void handleWakeUpRequest() {
    lastActivityTime = millis();
    if (isInEconomyMode) {
        DEBUG_PRINTLN("Requête reçue - Sortie du mode économie en cours...");
// Restauration immédiate de la fréquence CPU
        setCPUFrequency(CPU_FREQ_NORMAL);
// Sortie du light sleep
        if (isInModemSleep) {
            exitLightSleep();
        }
        
        isInEconomyMode = false;
// Redémarrage rapide des services
        if (mqttEnabled && WiFi.status() == WL_CONNECTED) {
            reconnectMQTT();
        }
     
        // Broadcast immédiat aux clients WebSocket
        broadcastSensorData();
    }
}

// --- FONCTION POUR LA MISE EN CACHE DES DONNEES ---
// *** CORRECTION DU DEADLOCK ICI ***
void updateSensorCache() {
    unsigned long currentMillis = millis();
    DEBUG_PRINTLN("Mise à jour du cache des capteurs...");
    
    wakeAllSensors();
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Prise du Mutex pour lecture/écriture des capteurs
    if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
        // Valeurs par défaut en cas de panne
        float temp = -999.0;
        float pressure = -999.0;
        float humidity = -999.0;
        float dewpoint = -999.0;
        float skyT = -999.0;
        float nuages = -999.0;
        float safe = -999.0;
        float lux = -999.0;
        float sqm = -999.0;
// Lecture des capteurs avec gestion des pannes
        #ifdef SHT            
        updateBME();
        updateSHT();
        updateMLX();
        updateTSL2591();

        temp = isSHTAvailable() ? getTemperature_SHT() : -999.0;
        pressure = isBMEAvailable() ? getPressure_BME() : -999.0;
        humidity = isSHTAvailable() ? getHumidity_SHT() : -999.0;
        dewpoint = isSHTAvailable() ? getDewpoint_SHT() : -999.0;
        skyT = isMLXAvailable() ?
        getTemperature_Sky() : -999.0;
        nuages = isMLXAvailable() ? getNuages() : -999.0;
        safe = isMLXAvailable() ? (float)getSafeNuages() : -999.0;
        lux = getLux();
// TSL2591 gère déjà les erreurs
        sqm = getSQM();
// TSL2591 gère déjà les erreurs
        #else
        updateBME();
        updateMLX();
        updateTSL2591();            
        temp = isBMEAvailable() ? getTemperature_BME() : -999.0;
        pressure = isBMEAvailable() ? getPressure_BME() : -999.0;
        humidity = isBMEAvailable() ?
        getHumidity_BME() : -999.0;
        dewpoint = isBMEAvailable() ? getDewpoint_BME() : -999.0;
        skyT = isMLXAvailable() ? getTemperature_Sky() : -999.0;
        nuages = isMLXAvailable() ? getNuages() : -999.0;
        safe = isMLXAvailable() ? (float)getSafeNuages() : -999.0;
        lux = getLux();
        sqm = getSQM();
        #endif            

        cachedData.temperature = temp;
        cachedData.pression = pressure;
        cachedData.humidity = humidity;
        cachedData.dewpoint = dewpoint;
        cachedData.skyT = skyT;
        cachedData.nuages = nuages;
        cachedData.safe = safe;
        cachedData.lux = lux;
        cachedData.sqm = sqm;
        cachedData.vent = getVent();
        cachedData.pluie = getValPluviometre();
        cachedData.gouttes = getGOUTTES();
        cachedData.lastUpdate = currentMillis;
// --- CORRECTION DEADLOCK ---
        // On libère le Mutex AVANT d'appeler d'autres fonctions complexes
        xSemaphoreGive(xSensorDataMutex);
    }
    
    // Le Mutex est libre, on peut faire le reste sans risque
    sleepAllSensors();
    broadcastSensorData(); // Cette fonction gère son propre verrouillage
    
    DEBUG_PRINTF("[CACHE] Données mises à jour - Capteurs en sleep: %s\n", areSensorsSleeping() ? "OUI" : "NON");
// Log des capteurs défaillants
    if (!isBMEAvailable()) DEBUG_PRINTLN("[ERREUR] Capteur BME280 défaillant");
    #ifdef SHT
    if (!isSHTAvailable()) DEBUG_PRINTLN("[ERREUR] Capteur SHT défaillant");
    #endif
    if (!isMLXAvailable()) DEBUG_PRINTLN("[ERREUR] Capteur MLX90614 défaillant");
}

// --- FONCTION POUR FORMATER LES DONNEES WEBSOCKET ---
void formatWebSocketData(const SensorData& data, char* buffer, size_t bufferSize) {
    // Format cohérent avec getSensorReadings()
    snprintf(buffer, bufferSize, 
             "{\"temperature\":%.2f,\"pression\":%.2f,\"humidity\":%.2f,\"dewpoint\":%.2f,\"skyT\":%.2f,\"nuages\":%.1f,\"safe\":%.1f,\"lux\":%.1f,\"sqm\":%.1f,\"Vent\":%.2f,\"Pluie\":%.3f,\"Gouttes\":%d,\"k1\":%.1f,\"k2\":%.1f,\"k3\":%.1f,\"k4\":%.1f,\"k5\":%.1f,\"k6\":%.1f,\"k7\":%.1f,\"temp_ciel_clair\":%.1f,\"temp_ciel_couvert\":%.1f}",
             data.temperature, data.pression, data.humidity, data.dewpoint, 
             data.skyT, data.nuages, data.safe, data.lux, data.sqm, 
             data.vent, data.pluie, data.gouttes,
             K1, K2, K3, K4, K5, K6, K7,
             temperature_ciel_clair, temperature_ciel_couvert);
}

// --- BROADCAST DES DONNEES WEBSOCKET ---
void broadcastSensorData() {
    if (websocketEnabled && ws.count() > 0) {
        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            char wsBuffer[512];
            formatWebSocketData(cachedData, wsBuffer, sizeof(wsBuffer));
            String jsonData = "{\"type\":\"sensor_update\",\"data\":" + String(wsBuffer) + "}";
            notifyClients(jsonData);
            xSemaphoreGive(xSensorDataMutex);
        }
    }
}

// --- FONCTION DE GENERATION DE LA CHAINE DE CARACTERES UTILISEE AVEC LA PAGE WEB ---
String getSensorReadings() {
    SensorData tempData;
    if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        DEBUG_PRINTLN("Erreur: Impossible d'acquérir le mutex pour la lecture");
        return "{\"error\": \"Unable to acquire sensor data\"}";
    }
    
    tempData = cachedData;
    xSemaphoreGive(xSensorDataMutex);
    char jsonBuffer[512];
    snprintf(jsonBuffer, sizeof(jsonBuffer), 
             "{\"temperature\":%.2f,\"pression\":%.2f,\"humidity\":%.2f,\"dewpoint\":%.2f,\"skyT\":%.2f,\"nuages\":%.1f,\"safe\":%.1f,\"lux\":%.1f,\"sqm\":%.1f,\"k1\":%.1f,\"k2\":%.1f,\"k3\":%.1f,\"k4\":%.1f,\"k5\":%.1f,\"k6\":%.1f,\"k7\":%.1f,\"temp_ciel_clair\":%.1f,\"temp_ciel_couvert\":%.1f,\"Vent\":%.2f,\"Pluie\":%.3f,\"Gouttes\":%d}",
             tempData.temperature, tempData.pression, tempData.humidity, tempData.dewpoint, 
             tempData.skyT, tempData.nuages, tempData.safe, tempData.lux, tempData.sqm, 
             K1, K2, K3, K4, K5, K6, K7,
             temperature_ciel_clair, temperature_ciel_couvert,
             tempData.vent, tempData.pluie, 
             tempData.gouttes);

    DEBUG_PRINTLN(jsonBuffer);
    return String(jsonBuffer);
}

// --- FONCTION DE GENERATION DE LA CHAINE DE CARACTERES POUR LE DRIVER ASCOM ---
String generateASCOMData() {
    SensorData tempData;
    if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return "erreur : Impossible d'acquérir les données des capteurs";
    }
    
    tempData = cachedData;
    xSemaphoreGive(xSensorDataMutex);

    char ascomBuffer[256];
    snprintf(ascomBuffer, sizeof(ascomBuffer), 
             "T:%.2f|C:%.2f|P:%.2f|H:%.2f|D:%.2f|L:%.1f|N:%.1f|S:%.1f|Q:%.1f|V:%.2f|R:%.3f",
             tempData.temperature, tempData.skyT, tempData.pression, tempData.humidity, 
             tempData.dewpoint, tempData.lux, tempData.nuages, tempData.safe, tempData.sqm, 
             tempData.vent, tempData.pluie);
    return String(ascomBuffer);
}
