#include <Arduino.h>
#include <Wire.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Arduino_JSON.h>
#include <freertos/semphr.h>
#include "esp_system.h"
#include "driver/timer.h"
#include <esp_sleep.h>
#include <esp_wifi.h>
#include "SPIFFS.h"

// --- Configuration du firmware ---
//#define SIMULATION_MODE
//#define DEBUG_MODE
#define CHAUFFAGE
#define SHT                              // A désactiver ici et dans MLX90614.cpp si non utilisé

// --- Bibliothèques locales ---
#include "debug.h"
#include "MQTT.h"
#include "WebSocket.h"
#include "variablesWEB.h"
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

// --- Paramètres Système & Alimentation ---
#define CPU_FREQ_LOW            80
#define CPU_FREQ_NORMAL         160
#define CPU_FREQ_HIGH           240

#define SLEEP_TIMEOUT           120000  // Délai d'inactivité avant veille (ms)
#define ECONOMY_CHECK_INTERVAL  300000  // Vérification en mode éco (ms)
#define HEARTBEAT_INTERVAL      60000   // Fréquence du heartbeat normal (ms)

// --- Paramètres WiFi Adaptatif (RSSI) ---
#define RSSI_EXCELLENT          -50
#define RSSI_GOOD               -65
#define RSSI_FAIR               -75
#define RSSI_POOR               -85

#define WIFI_POWER_MAX          82      // 20dBm
#define WIFI_POWER_MEDIUM       74      // 17dBm
#define WIFI_POWER_LOW          62      // 14dBm
#define WIFI_POWER_MIN          52      // 11dBm

// --- Paramètres de Fréquence des Capteurs (en secondes) ---
#define INTERO_VENT             5
#define INTERO_PLUVIO           360
#define INTERO_GOUTTES          60
#define CACHED_INTERVAL         20000   // Rafraîchissement du cache (ms)
#ifdef CHAUFFAGE
    const byte pinChauffage = 13;
    #define INTERO_CHAUFFAGE    600     
#endif

// --- Paramètres de Maintenance ---
#define REBOOT_INTERVAL_MS           604800000UL // 7 jours
#define MEM_CHECK_INTERVAL           172800000UL // 48 heures
#define LOG_CLEANUP_INTERVAL         604800000UL // 7 jours
#define CRITICAL_HEAP_SIZE           100000
#define MQTT_PUBLISH_INTERVAL        60000
#define WEBSOCKET_BROADCAST_INTERVAL 5000

// --- Variables Globales ---
AsyncWebServer server(80);
AsyncEventSource events("/events");
OTAHandler otaHandler;

hw_timer_t *watchdogTimer = NULL;
SemaphoreHandle_t xSensorDataMutex;

JSONVar readings;
JSONVar ascomData;

// État du système
bool isInEconomyMode = false;
bool isInModemSleep  = false;

// Chronomètres de cycle
unsigned long lastActivityTime      = 0;
unsigned long lastEconomyCheck      = 0;
unsigned long lastHeartbeat         = 0;
unsigned long lastRebootCheck       = 0;
unsigned long lastMemCheck          = 0;
unsigned long lastLogCleanup        = 0;
unsigned long lastMqttPublish       = 0;
unsigned long lastBroadcast         = 0;

unsigned long t_lastActionVent      = 0;
unsigned long t_lastActionPluvio    = 0;
unsigned long t_lastActionGouttes   = 0;
#ifdef CHAUFFAGE
    unsigned long t_lastActionChauffage = 0;
    int chauffage = 0;
#endif

// --- Prototypes ---
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

/* --------------------------------------------------------------------------
 * GESTION DES INTERRUPTIONS
 * -------------------------------------------------------------------------- */

void IRAM_ATTR resetModule() {
    ets_printf("Watchdog timeout! Redémarrage...\n");
    ESP.restart();
}

/* --------------------------------------------------------------------------
 * FONCTIONS DE CONFIGURATION MATÉRIELLE ET ÉNERGÉTIQUE
 * -------------------------------------------------------------------------- */

/**
 * Ajuste dynamiquement la puissance d'émission WiFi selon la qualité du signal
 * pour économiser l'énergie et réduire la chauffe.
 */
void adjustWiFiPowerBasedOnRSSI() {
    if (!WiFi.isConnected()) return;

    int8_t rssi = WiFi.RSSI();
    uint8_t new_power = WIFI_POWER_MIN;
    
    if (rssi >= RSSI_EXCELLENT || rssi >= RSSI_GOOD) {
        new_power = WIFI_POWER_LOW;
    } else if (rssi >= RSSI_FAIR) {
        new_power = WIFI_POWER_MIN;
    }
    
    esp_wifi_set_max_tx_power(new_power);
    DEBUG_PRINTF("RSSI: %ddB -> Puissance WiFi: %d\n", rssi, new_power);
}

/**
 * Configure les broches non câblées en entrée pull-down pour 
 * éviter les interférences électromagnétiques (effet antenne).
 */
void setupUnusedPins() {
    const int unusedPins[] = {0, 2, 4, 5, 12, 15, 16, 17, 18, 19, 23, 25, 26, 33};
    for (int pin : unusedPins) {
        pinMode(pin, INPUT_PULLDOWN);
    }
    DEBUG_PRINTLN("Broches non utilisées configurées en input pull-down");
}

void setCPUFrequency(uint32_t freq_mhz) {
    if (freq_mhz != getCpuFrequencyMhz()) {
        setCpuFrequencyMhz(freq_mhz);
        DEBUG_PRINTF("Fréquence CPU réglée à: %d MHz\n", getCpuFrequencyMhz());
    }
}

/**
 * Prévient les erreurs logiques lors du retour à zéro de la fonction millis() 
 * (survient environ tous les 49,7 jours).
 */
void handleMillisOverflow() {
    DEBUG_PRINTLN("Cycle millis() détecté, réinitialisation des horodatages...");
    unsigned long currentMillis = millis();
    
    lastActivityTime    = currentMillis;
    cachedData.lastUpdate = 0;
    t_lastActionVent    = currentMillis;
    t_lastActionPluvio  = currentMillis;
    t_lastActionGouttes = currentMillis;
    
    #ifdef CHAUFFAGE
        t_lastActionChauffage = currentMillis;
    #endif
    
    lastRebootCheck  = currentMillis;
    lastMemCheck     = currentMillis;
    lastLogCleanup   = currentMillis;
    lastHeartbeat    = currentMillis;
    lastEconomyCheck = currentMillis;
}

/* --------------------------------------------------------------------------
 * GESTION DU MODE VEILLE (SLEEP)
 * -------------------------------------------------------------------------- */

void enterLightSleep() {
    if (!isInModemSleep) {
        DEBUG_PRINTLN("Activation du light sleep avec maintien WiFi");
        esp_sleep_enable_timer_wakeup(1000000); // 1 seconde max
        esp_light_sleep_start();
        
        esp_wifi_set_max_tx_power(WIFI_POWER_MIN);
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
        
        isInModemSleep = true;
    }
}

void exitLightSleep() {
    if (isInModemSleep) {
        DEBUG_PRINTLN("Sortie du light sleep");
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        adjustWiFiPowerBasedOnRSSI();
        
        isInModemSleep = false;
        updateSensorCache();
    }
}

void wakeAllSensors() {
    wakeBME();
    #ifdef SHT
        wakeSHT();
    #endif
    wakeMLX();
    vTaskDelay(pdMS_TO_TICKS(30));
}

void sleepAllSensors() {
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

/* --------------------------------------------------------------------------
 * INITIALISATION PRINCIPALE
 * -------------------------------------------------------------------------- */

void setup() {
    Serial.begin(115200);
    Wire.begin();

    // Optimisations énergétiques initiales
    btStop();
    setupUnusedPins();
    setCPUFrequency(CPU_FREQ_NORMAL);
    
    #ifdef DEBUG_MODE
        if (!btStarted()) DEBUG_PRINTLN("Economie Bluetooth: OK");
    #endif

    // Initialisation Watchdog (30s)
    watchdogTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(watchdogTimer, &resetModule, true);
    timerAlarmWrite(watchdogTimer, 30000000, false);
    timerAlarmEnable(watchdogTimer);

    initWiFi();
    initSPIFFS();
    createConfigDir();
    loadConstantsFromFile();

    // Initialisation des capteurs
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

    // Création du Mutex pour la sécurité du multi-tâche
    xSensorDataMutex = xSemaphoreCreateMutex();
    if (xSensorDataMutex == NULL) {
        DEBUG_PRINTLN("Erreur: Impossible de créer le mutex");
    }

    otaHandler.begin(server);

    // Initialisation des données
    updateSensorCache();
    sleepAllSensors();

    // Configuration des routes HTTP
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
        request->send(200, "application/json", getSensorReadings());
    });

    server.on("/ascom", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        request->send(200, "application/json", generateASCOMData());
    });

    server.on("/version", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        String versionJson = "{\"version\": \"" + String(firmwareVersionAstroMeteo) + "\"}";
        request->send(200, "application/json", versionJson);
    });

    server.on("/ip", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        request->send(200, "application/json", getLocalIPAddress());
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
        String response = "{\"mqtt\":\"" + String(mqttEnabled ? "enabled" : "disabled") + 
                          "\",\"websocket\":\"" + String(websocketEnabled ? "enabled" : "disabled") + "\"}";
        request->send(200, "application/json", response);
    });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        String response = "{\"mqtt_connected\":" + String(mqttClient.connected() ? "true" : "false") + 
                          ",\"websocket_clients\":" + String(ws.count()) + 
                          ",\"clients_connected\":" + String(ws.count()) + "}";
        request->send(200, "application/json", response);
    });

    server.addHandler(&events);
    server.serveStatic("/", SPIFFS, "/");
    listSPIFFSFiles("/");
    server.begin();

    // Démarrage des protocoles externes
    initWebSocket();
    initMQTT();
    server.addHandler(&ws);

    // Initialisation finale de l'énergie et des chronomètres
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    adjustWiFiPowerBasedOnRSSI();

    unsigned long currentMillis = millis();
    lastRebootCheck  = currentMillis;
    lastMemCheck     = currentMillis;
    lastLogCleanup   = currentMillis;
    lastHeartbeat    = currentMillis;
    lastActivityTime = currentMillis;
    lastEconomyCheck = currentMillis;

    DEBUG_PRINTLN("Initialisation complète terminée.");
}

/* --------------------------------------------------------------------------
 * BOUCLE PRINCIPALE
 * -------------------------------------------------------------------------- */

void loop() {
    timerWrite(watchdogTimer, 0); // Reset du Watchdog
    unsigned long currentMillis = millis();

    // Gestion de l'overflow de millis()
    static unsigned long previousMillisCycle = 0;
    if (currentMillis < previousMillisCycle) {
        handleMillisOverflow();
    }
    previousMillisCycle = currentMillis;

    // --- GESTION DU MODE ÉCONOMIE ---
    if (currentMillis - lastActivityTime >= SLEEP_TIMEOUT) {
        if (!isInEconomyMode) {
            DEBUG_PRINTLN("Entrée en mode économie d'énergie");
            isInEconomyMode = true;

            #ifdef CHAUFFAGE
                digitalWrite(pinChauffage, LOW);
                chauffage = 0;
                DEBUG_PRINTLN("Chauffage coupé par sécurité (Mode économie)");
            #endif            
            
            setCPUFrequency(CPU_FREQ_LOW);
            if (mqttClient.connected()) mqttClient.disconnect();
        }
        
        if (!isInModemSleep) enterLightSleep();
        
        // Entretien minimum en mode veille
        if (currentMillis - lastEconomyCheck >= ECONOMY_CHECK_INTERVAL) {
            lastEconomyCheck = currentMillis;
            if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL * 5) {
                lastHeartbeat = currentMillis;
                checkWiFi();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(3000));
        return; // Évite l'exécution du reste de la boucle
    } 
    else if (isInEconomyMode) {
        // Révéil du système
        DEBUG_PRINTLN("Sortie du mode économie - Mode actif");
        isInEconomyMode = false;
        
        setCPUFrequency(CPU_FREQ_NORMAL);
        if (isInModemSleep) exitLightSleep();
        if (mqttEnabled && WiFi.status() == WL_CONNECTED) reconnectMQTT();
        
        lastHeartbeat    = currentMillis;
        lastEconomyCheck = currentMillis;
    }

    // --- TÂCHES DE FOND (MODE ACTIF) ---
    
    // Heartbeat WiFi
    if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL * 5) {
        lastHeartbeat = currentMillis;
        checkWiFi();
        adjustWiFiPowerBasedOnRSSI();
    }

    // Redémarrage préventif hebdomadaire sécurisé (attente du jour via SQM)
    if (currentMillis - lastRebootCheck >= REBOOT_INTERVAL_MS) {
        float currentSQM = 0;
        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            currentSQM = cachedData.sqm;
            xSemaphoreGive(xSensorDataMutex);
        }

        const float SEUIL_SQM_JOUR = 10.0;
        if (currentSQM > -100.0 && currentSQM < SEUIL_SQM_JOUR) {
            DEBUG_PRINTLN("7 jours écoulés et soleil levé : Redémarrage hebdomadaire préventif.");
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP.restart();
        }
    }

    // Surveillance de l'intégrité de la mémoire RAM
    if (currentMillis - lastMemCheck >= MEM_CHECK_INTERVAL) {
        lastMemCheck = currentMillis;
        uint32_t freeHeap = ESP.getFreeHeap();
        
        if (freeHeap < CRITICAL_HEAP_SIZE) {
            DEBUG_PRINTF("ERREUR: Mémoire critique (%u octets). Redémarrage immédiat.\n", freeHeap);
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP.restart();
        }
    }

    // Nettoyage des logs de mise à jour
    if (currentMillis - lastLogCleanup >= LOG_CLEANUP_INTERVAL) {
        lastLogCleanup = currentMillis;
        otaHandler.cleanLogs();
    }

    // --- GESTION DES CAPTEURS ET COMMUNICATIONS ---
    
    // Cache global
    if (currentMillis - cachedData.lastUpdate >= CACHED_INTERVAL) {
        updateSensorCache();
    }

    handleMQTT();
    handleWebSocket();

    // Publication MQTT
    if (currentMillis - lastMqttPublish >= MQTT_PUBLISH_INTERVAL * 2) {
        lastMqttPublish = currentMillis;
        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            publishSensorData(cachedData);
            xSemaphoreGive(xSensorDataMutex);
        }
    }

    // Broadcast WebSocket
    if (currentMillis - lastBroadcast >= WEBSOCKET_BROADCAST_INTERVAL) {
        lastBroadcast = currentMillis;
        broadcastSensorData();
    }

    // Anémomètre (Haute fréquence pour la sécurité de la monture)
    if (currentMillis - t_lastActionVent >= INTERO_VENT * 1000) {
        t_lastActionVent = currentMillis;
        getSendVitesseVent(INTERO_VENT);
        
        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            cachedData.vent = getVent();
            xSemaphoreGive(xSensorDataMutex);
        }
    }

    // Pluviomètre
    if (currentMillis - t_lastActionPluvio >= INTERO_PLUVIO * 1000) {
        t_lastActionPluvio = currentMillis;
        getSendPluviometre(INTERO_PLUVIO);
    }

    // Capteur de pluie capacitif
    if (currentMillis - t_lastActionGouttes >= INTERO_GOUTTES * 1000) {
        t_lastActionGouttes = currentMillis;
        updateGOUTTES();
    }

    // Gestion de la résistance anti-rosée
    #ifdef CHAUFFAGE
    if (currentMillis - t_lastActionChauffage >= INTERO_GOUTTES * 1000) {
        t_lastActionChauffage = currentMillis;
        gererChauffageRosse(cachedData.dewpoint);
    }
    #endif

    vTaskDelay(pdMS_TO_TICKS(200));
}

/* --------------------------------------------------------------------------
 * FONCTIONS AUXILIAIRES ET FORMATAGE DES DONNÉES
 * -------------------------------------------------------------------------- */

/**
 * Sort le système du mode veille lorsqu'une requête est reçue (NINA, Web).
 */
void handleWakeUpRequest() {
    lastActivityTime = millis();
    if (isInEconomyMode) {
        setCPUFrequency(CPU_FREQ_NORMAL);
        if (isInModemSleep) exitLightSleep();
        
        isInEconomyMode = false;
        
        if (mqttEnabled && WiFi.status() == WL_CONNECTED) reconnectMQTT();
        broadcastSensorData();
    }
}

/**
 * Lit l'ensemble des capteurs I2C et met à jour le cache mémoire de manière
 * sécurisée via un Mutex pour éviter la corruption de données pendant la lecture.
 */
void updateSensorCache() {
    unsigned long currentMillis = millis();
    wakeAllSensors();
    vTaskDelay(pdMS_TO_TICKS(10));
    
    if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
        float temp     = -999.0;
        float pressure = -999.0;
        float humidity = -999.0;
        float dewpoint = -999.0;
        float skyT     = -999.0;
        float nuages   = -999.0;
        float safe     = -999.0;
        float lux      = -999.0;
        float sqm      = -999.0;

        #ifdef SHT            
            updateBME();
            updateSHT();
            updateMLX();
            updateTSL2591();

            temp     = isSHTAvailable() ? getTemperature_SHT() : -999.0;
            pressure = isBMEAvailable() ? getPressure_BME() : -999.0;
            humidity = isSHTAvailable() ? getHumidity_SHT() : -999.0;
            dewpoint = isSHTAvailable() ? getDewpoint_SHT() : -999.0;
            skyT     = isMLXAvailable() ? getTemperature_Sky() : -999.0;
            nuages   = isMLXAvailable() ? getNuages() : -999.0;
            safe     = isMLXAvailable() ? (float)getSafeNuages() : -999.0;
            lux      = getLux();
            sqm      = getSQM();
        #else
            updateBME();
            updateMLX();
            updateTSL2591();            
            
            temp     = isBMEAvailable() ? getTemperature_BME() : -999.0;
            pressure = isBMEAvailable() ? getPressure_BME() : -999.0;
            humidity = isBMEAvailable() ? getHumidity_BME() : -999.0;
            dewpoint = isBMEAvailable() ? getDewpoint_BME() : -999.0;
            skyT     = isMLXAvailable() ? getTemperature_Sky() : -999.0;
            nuages   = isMLXAvailable() ? getNuages() : -999.0;
            safe     = isMLXAvailable() ? (float)getSafeNuages() : -999.0;
            lux      = getLux();
            sqm      = getSQM();
        #endif            

        cachedData.temperature = temp;
        cachedData.pression    = pressure;
        cachedData.humidity    = humidity;
        cachedData.dewpoint    = dewpoint;
        cachedData.skyT        = skyT;
        cachedData.nuages      = nuages;
        cachedData.safe        = safe;
        cachedData.lux         = lux;
        cachedData.sqm         = sqm;
        cachedData.vent        = getVent();
        cachedData.pluie       = getValPluviometre();
        cachedData.gouttes     = getGOUTTES();
        cachedData.lastUpdate  = currentMillis;

        // Libération du Mutex requise avant de poursuivre d'autres opérations complexes
        xSemaphoreGive(xSensorDataMutex);
    }
    
    sleepAllSensors();
    broadcastSensorData(); 
}

void formatWebSocketData(const SensorData& data, char* buffer, size_t bufferSize) {
    snprintf(buffer, bufferSize, 
             "{\"temperature\":%.2f,\"pression\":%.2f,\"humidity\":%.2f,\"dewpoint\":%.2f,\"skyT\":%.2f,\"nuages\":%.1f,\"safe\":%.1f,\"lux\":%.1f,\"sqm\":%.1f,\"Vent\":%.2f,\"Pluie\":%.3f,\"Gouttes\":%d,\"k1\":%.1f,\"k2\":%.1f,\"k3\":%.1f,\"k4\":%.1f,\"k5\":%.1f,\"k6\":%.1f,\"k7\":%.1f,\"temp_ciel_clair\":%.1f,\"temp_ciel_couvert\":%.1f}",
             data.temperature, data.pression, data.humidity, data.dewpoint, 
             data.skyT, data.nuages, data.safe, data.lux, data.sqm, 
             data.vent, data.pluie, data.gouttes,
             K1, K2, K3, K4, K5, K6, K7,
             temperature_ciel_clair, temperature_ciel_couvert);
}

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

String getSensorReadings() {
    SensorData tempData;
    if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
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
             tempData.vent, tempData.pluie, tempData.gouttes);

    return String(jsonBuffer);
}

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
