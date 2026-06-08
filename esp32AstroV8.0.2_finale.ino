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

// --- VARIABLES POUR LA CALIBRATION MLX EMBARQUÉE ---
#define MAX_CALIB_POINTS 720
struct CalibPoint {
    float Ta;
    float TsBrute;
};
CalibPoint calibData[MAX_CALIB_POINTS];
int calibPointCount = 0;
volatile bool isRecordingCalib = false;
unsigned long lastCalibRecordTime = 0;
volatile bool isOptimizing = false;

// MODIFICATION : Seuil de luminosité dynamique réglable depuis l'interface (Défaut : 10 Lux)
float luxThresholdCalib = 10.0; 

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
    ets_printf("Watchdog timeout! Redémarrage d'urgence...\n");
    esp_restart();
}

/* --------------------------------------------------------------------------
 * FONCTIONS DE CONFIGURATION MATÉRIELLE ET ÉNERGÉTIQUE
 * -------------------------------------------------------------------------- */

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
        esp_sleep_enable_timer_wakeup(1000000); 
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
 * TÂCHE IA D'AUTO-CALIBRATION (FreeRTOS)
 * -------------------------------------------------------------------------- */

void optimizeCalibTask(void *pvParameters) {
    isOptimizing = true;
    DEBUG_PRINTLN("[IA] Début de l'optimisation mathématique...");
    
    float bestK[5] = {K1, K2, K3, K4, K5};
    
    auto calcVariance = [](float k1, float k2, float k3, float k4, float k5) {
        if (calibPointCount == 0) return 0.0f;
        
        float sum = 0.0f;
        float values[MAX_CALIB_POINTS];
        
        for(int i=0; i < calibPointCount; i++) {
            float ta = calibData[i].Ta;
            float ts = calibData[i].TsBrute;
            float td = (k1/100.0f) * (ta - k2/10.0f) + (k3/100.0f) * pow(exp(k4/1000.0f * ta), (k5/100.0f));
            values[i] = ts - td; 
            sum += values[i];
        }
        float mean = sum / calibPointCount;
        
        float variance = 0.0f;
        for(int i=0; i < calibPointCount; i++) {
            variance += pow(values[i] - mean, 2);
        }
        return variance / calibPointCount;
    };
    
    float bestVar = calcVariance(bestK[0], bestK[1], bestK[2], bestK[3], bestK[4]);
    float simTemp = 50.0f; 
    
    for (int i = 0; i < 5000; i++) {
        float testK[5];
        
        testK[0] = bestK[0] + (random(-100, 100) / 100.0f) * simTemp;
        if (testK[0] < 0.0f) testK[0] = 0.0f; else if (testK[0] > 100.0f) testK[0] = 100.0f;
        
        testK[1] = bestK[1] + (random(-100, 100) / 100.0f) * simTemp;
        if (testK[1] < -50.0f) testK[1] = -50.0f;
        else if (testK[1] > 50.0f) testK[1] = 50.0f;
        
        testK[2] = bestK[2] + (random(-100, 100) / 100.0f) * simTemp;
        if (testK[2] < 0.0f) testK[2] = 0.0f; else if (testK[2] > 50.0f) testK[2] = 50.0f;
        
        testK[3] = bestK[3] + (random(-100, 100) / 100.0f) * simTemp;
        if (testK[3] < 0.0f) testK[3] = 0.0f;
        else if (testK[3] > 200.0f) testK[3] = 200.0f;
        
        testK[4] = bestK[4] + (random(-100, 100) / 100.0f) * simTemp;
        if (testK[4] < 0.0f) testK[4] = 0.0f; else if (testK[4] > 200.0f) testK[4] = 200.0f;
        
        float v = calcVariance(testK[0], testK[1], testK[2], testK[3], testK[4]);
        if (v < bestVar) {
            bestVar = v;
            for(int j=0; j<5; j++) bestK[j] = testK[j];
        }
        simTemp *= 0.995f;
        
        if (i % 100 == 0) vTaskDelay(pdMS_TO_TICKS(10));
    }

    K1 = bestK[0]; K2 = bestK[1]; K3 = bestK[2];
    K4 = bestK[3]; K5 = bestK[4];
    saveConstantsToFile(); 
    
    DEBUG_PRINTF("[IA] Optimisation terminée. Variance: %.4f\n", bestVar);
    isOptimizing = false;
    vTaskDelete(NULL); 
}

/* --------------------------------------------------------------------------
 * INITIALISATION PRINCIPALE
 * -------------------------------------------------------------------------- */

void setup() {
    Serial.begin(115200);
    Wire.begin();
    
    btStop();
    setupUnusedPins();
    setCPUFrequency(CPU_FREQ_NORMAL);
    
    #ifdef DEBUG_MODE
        if (!btStarted()) DEBUG_PRINTLN("Economie Bluetooth: OK");
    #endif

    watchdogTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(watchdogTimer, &resetModule, true);
    timerAlarmWrite(watchdogTimer, 30000000, false);
    timerAlarmEnable(watchdogTimer);

    initWiFi();
    initSPIFFS();
    createConfigDir();
    loadConstantsFromFile();

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

    xSensorDataMutex = xSemaphoreCreateMutex();
    if (xSensorDataMutex == NULL) {
        DEBUG_PRINTLN("Erreur: Impossible de créer le mutex");
    }

    otaHandler.begin(server);
    updateSensorCache();
    sleepAllSensors();
    
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
        
        if (request->hasParam("newK1", true) && request->getParam("newK1", true)->value().length() > 0) 
            K1 = request->getParam("newK1", true)->value().toFloat();
        if (request->hasParam("newK2", true) && request->getParam("newK2", true)->value().length() > 0) 
            K2 = request->getParam("newK2", true)->value().toFloat();
     
        if (request->hasParam("newK3", true) && request->getParam("newK3", true)->value().length() > 0) 
            K3 = request->getParam("newK3", true)->value().toFloat();
        if (request->hasParam("newK4", true) && request->getParam("newK4", true)->value().length() > 0) 
            K4 = request->getParam("newK4", true)->value().toFloat();
        if (request->hasParam("newK5", true) && request->getParam("newK5", true)->value().length() > 0) 
            K5 = request->getParam("newK5", true)->value().toFloat();
            
        if (request->hasParam("newK6", true) && request->getParam("newK6", true)->value().length() > 0) 
            K6 = request->getParam("newK6", true)->value().toFloat();
            
        if (request->hasParam("newK7", true) && request->getParam("newK7", true)->value().length() > 0) 
            K7 = request->getParam("newK7", true)->value().toFloat();
            
        if (request->hasParam("newTempClair", true) && request->getParam("newTempClair", true)->value().length() > 0) {
            temperature_ciel_clair = request->getParam("newTempClair", true)->value().toFloat();
        }
        if (request->hasParam("newTempCouvert", true) && request->getParam("newTempCouvert", true)->value().length() > 0) {
            temperature_ciel_couvert = request->getParam("newTempCouvert", true)->value().toFloat();
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

    server.on("/configuration.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        request->send(SPIFFS, "/configuration.html", String(), false);
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

    // --- ROUTES API POUR LA CALIBRATION ---
    server.on("/api/calib/action", HTTP_POST, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        
        // MODIFICATION : Capturer le réglage de seuil de Lux s'il est transmis
        if (request->hasParam("threshold", true)) {
            luxThresholdCalib = request->getParam("threshold", true)->value().toFloat();
            DEBUG_PRINTF("Nouveau seuil de Lux défini : %.1f Lux\n", luxThresholdCalib);
        }
        
        if (request->hasParam("action", true)) {
            String action = request->getParam("action", true)->value();
            
            if (action == "start") {
                isRecordingCalib = true;
                lastCalibRecordTime = millis() - 60000; 
            } else if (action == "stop") {
                isRecordingCalib = false;
            } else if (action == "clear") {
                calibPointCount = 0;
                isRecordingCalib = false;
            } else if (action == "optimize") {
                if (calibPointCount >= 5 && !isOptimizing) {
                    xTaskCreate(optimizeCalibTask, "OptimizeTask", 8192, NULL, 1, NULL);
                }
            }
        }
        request->send(200, "text/plain", "OK");
    });

    server.on("/api/calib/data", HTTP_GET, [](AsyncWebServerRequest *request) {
        handleWakeUpRequest();
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        response->print("{\"isRecording\":");
        response->print(isRecordingCalib ? "true" : "false");
        response->print(",\"isOptimizing\":");
        response->print(isOptimizing ? "true" : "false");
        response->print(",\"count\":");
        response->print(calibPointCount);
        // MODIFICATION : On transmet la valeur actuelle du seuil à l'interface HTML
        response->printf(",\"luxThreshold\":%.1f", luxThresholdCalib); 
        response->printf(",\"k\":[%.1f,%.1f,%.1f,%.1f,%.1f],\"data\":[", K1, K2, K3, K4, K5);
       
        for(int i=0; i < calibPointCount; i++) {
            response->printf("{\"Ta\":%.2f,\"Ts\":%.2f}", calibData[i].Ta, calibData[i].TsBrute);
            if (i < calibPointCount - 1) response->print(",");
        }
        response->print("]}");
        request->send(response);
    });
    
    server.addHandler(&events);
    server.serveStatic("/", SPIFFS, "/");
    listSPIFFSFiles("/");
    server.begin();

    initWebSocket();
    initMQTT();
    server.addHandler(&ws);
    
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
    timerWrite(watchdogTimer, 0); 
    unsigned long currentMillis = millis();
    static unsigned long previousMillisCycle = 0;
    
    if (currentMillis < previousMillisCycle) {
        handleMillisOverflow();
    }
    previousMillisCycle = currentMillis;
    
    if (currentMillis - lastActivityTime >= SLEEP_TIMEOUT && !isRecordingCalib && !isOptimizing) {
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
        
        if (currentMillis - lastEconomyCheck >= ECONOMY_CHECK_INTERVAL) {
            lastEconomyCheck = currentMillis;
            if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL * 5) {
                lastHeartbeat = currentMillis;
                checkWiFi();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(3000));
        return; 
    } 
    else if (isInEconomyMode) {
        DEBUG_PRINTLN("Sortie du mode économie - Mode actif");
        isInEconomyMode = false;
        
        setCPUFrequency(CPU_FREQ_NORMAL);
        if (isInModemSleep) exitLightSleep();
        if (mqttEnabled && WiFi.status() == WL_CONNECTED) reconnectMQTT();
        
        lastHeartbeat    = currentMillis;
        lastEconomyCheck = currentMillis;
    }

    if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL * 5) {
        lastHeartbeat = currentMillis;
        checkWiFi();
        adjustWiFiPowerBasedOnRSSI();
    }

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

    if (currentMillis - lastMemCheck >= MEM_CHECK_INTERVAL) {
        lastMemCheck = currentMillis;
        uint32_t freeHeap = ESP.getFreeHeap();
        
        if (freeHeap < CRITICAL_HEAP_SIZE) {
            DEBUG_PRINTF("ERREUR: Mémoire critique (%u octets). Redémarrage immédiat.\n", freeHeap);
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP.restart();
        }
    }

    if (currentMillis - lastLogCleanup >= LOG_CLEANUP_INTERVAL) {
        lastLogCleanup = currentMillis;
        otaHandler.cleanLogs();
    }

    if (currentMillis - cachedData.lastUpdate >= CACHED_INTERVAL) {
        updateSensorCache();
    }

    handleMQTT();
    handleWebSocket();

    if (currentMillis - lastMqttPublish >= MQTT_PUBLISH_INTERVAL * 2) {
        lastMqttPublish = currentMillis;
        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            publishSensorData(cachedData);
            xSemaphoreGive(xSensorDataMutex);
        }
    }

    if (currentMillis - lastBroadcast >= WEBSOCKET_BROADCAST_INTERVAL) {
        lastBroadcast = currentMillis;
        broadcastSensorData();
    }

    if (currentMillis - t_lastActionVent >= INTERO_VENT * 1000) {
        t_lastActionVent = currentMillis;
        getSendVitesseVent(INTERO_VENT);
        
        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            cachedData.vent = getVent();
            xSemaphoreGive(xSensorDataMutex);
        }
    }

    if (currentMillis - t_lastActionPluvio >= INTERO_PLUVIO * 1000) {
        t_lastActionPluvio = currentMillis;
        getSendPluviometre(INTERO_PLUVIO);
    }

    if (currentMillis - t_lastActionGouttes >= INTERO_GOUTTES * 1000) {
        t_lastActionGouttes = currentMillis;
        updateGOUTTES();
    }

    #ifdef CHAUFFAGE
    if (currentMillis - t_lastActionChauffage >= INTERO_GOUTTES * 1000) {
        t_lastActionChauffage = currentMillis;
        gererChauffageRosse(cachedData.dewpoint);
    }
    #endif

    // --- GESTION ENREGISTREMENT CALIBRATION EMBARQUÉE ---
    if (isRecordingCalib) {
        
        float currentLux = -1.0;
        if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
            currentLux = cachedData.lux;
            xSemaphoreGive(xSensorDataMutex);
        }
        
        // MODIFICATION : Comparaison dynamique avec "luxThresholdCalib" au lieu du "10.0" en dur
        if (currentLux >= 0 && currentLux < luxThresholdCalib) {
            DEBUG_PRINTF("Coucher du Soleil détecté (%.1f Lux < Seuil: %.1f Lux). Arrêt automatique de la session.\n", currentLux, luxThresholdCalib);
            isRecordingCalib = false;
        }
        else if (currentMillis - lastCalibRecordTime >= 60000) {
            lastCalibRecordTime = currentMillis;
            if (calibPointCount < MAX_CALIB_POINTS) {
                if (xSemaphoreTake(xSensorDataMutex, portMAX_DELAY) == pdTRUE) {
                    float ta = cachedData.temperature;
                    float tsky_esp = cachedData.skyT;
                    
                    if (ta != -999.0f && tsky_esp != -999.0f) {
                        float td = (K1/100.0f) * (ta - K2/10.0f) + (K3/100.0f) * pow(exp(K4/1000.0f * ta), (K5/100.0f));
                        calibData[calibPointCount].Ta = ta;
                        calibData[calibPointCount].TsBrute = tsky_esp + td;
                        calibPointCount++;
                    }
                    xSemaphoreGive(xSensorDataMutex);
                }
            } else {
                isRecordingCalib = false; 
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
}

/* --------------------------------------------------------------------------
 * FONCTIONS AUXILIAIRES ET FORMATAGE DES DONNÉES
 * -------------------------------------------------------------------------- */

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
