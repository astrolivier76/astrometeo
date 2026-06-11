#include <Arduino.h>
#include "SHT.h"
#include <Wire.h>
#include <SHTSensor.h>
//#define SIMULATION_MODE
#include "debug.h"

float correction_temperature_sht = 0.0;
const float seuil_chauffage = 2.0;

namespace {
    struct SHTGenericSensor {
        SHTSensor sensor; // Objet générique Sensirion
        bool available = false;
        bool isSleeping = false;
        float temperature = -999;
        float humidite = -999;
        float dewpoint = -999;
        int retryCount = 0;
        unsigned long lastRetryTime = 0;
        const int maxRetries = 3;
        const unsigned long retryInterval = 30000; // 30 secondes
    };
    SHTGenericSensor shtSensor;
}

#ifdef SIMULATION_MODE
bool initSHT() {
    DEBUG_PRINTLN("[SHT] Mode simulation activé.");
    shtSensor.available = true;
    return true;
}

void updateSHT() {
    if (!shtSensor.available) return;

    shtSensor.temperature = 25 + random(-20, 20) * 0.1;
    shtSensor.humidite = 50 + random(-10, 10) * 0.5;
    
    float a = 17.27;
    float b = 237.7;
    float gamma = (a * shtSensor.temperature) / (b + shtSensor.temperature) + log(shtSensor.humidite / 100.0);
    shtSensor.dewpoint = (b * gamma) / (a - gamma);

    DEBUG_PRINTF("[SHT] Temp: %.2f°C, Humidité: %.2f%%\n",
                 shtSensor.temperature, shtSensor.humidite);
}
#else

bool initSHT() {
    DEBUG_PRINTLN("[SHT] Initialisation et auto-détection du capteur Sensirion...");
    const int maxRetries = 3;
    
    for (int i = 0; i < maxRetries; i++) {
        // init() tente de détecter automatiquement le modèle de SHT connecté
        if (shtSensor.sensor.init()) {
            
            // On lance une lecture test pour confirmer que tout fonctionne
            if (shtSensor.sensor.readSample()) {
                shtSensor.available = true;
                shtSensor.retryCount = 0;
                DEBUG_PRINTLN("SHT (Sensirion) initialisé avec succès.");
                return true;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
        DEBUG_PRINTF("Tentative %d/3 échouée.\n", i + 1);
    }
    
    shtSensor.available = false;
    DEBUG_PRINTLN("Erreur : Impossible d'initialiser le capteur SHT.");
    return false;
}

void updateSHT() {
    // Tentative de récupération si capteur défaillant
    if (!shtSensor.available) {
        unsigned long currentMillis = millis();
        if (shtSensor.retryCount < shtSensor.maxRetries && 
            (currentMillis - shtSensor.lastRetryTime >= shtSensor.retryInterval)) {
            
            shtSensor.retryCount++;
            shtSensor.lastRetryTime = currentMillis;
            DEBUG_PRINTF("[SHT] Tentative de récupération %d/%d\n", shtSensor.retryCount, shtSensor.maxRetries);
            
            if (initSHT()) {
                DEBUG_PRINTLN("[SHT] Capteur récupéré avec succès!");
                return; // Sortir pour éviter la lecture immédiate
            } else {
                DEBUG_PRINTF("[SHT] Échec de la récupération %d/%d\n", shtSensor.retryCount, shtSensor.maxRetries);
                
                if (shtSensor.retryCount >= shtSensor.maxRetries) {
                    DEBUG_PRINTLN("[SHT] Abandon après 3 tentatives échouées");
                }
            }
        }
        return; // Ne pas tenter de lecture si capteur non disponible
    }
    
    // --- NOUVEAU : Filet de sécurité avec une 2ème tentative immédiate ---
    bool readOk = shtSensor.sensor.readSample();
    
    if (!readOk) {
        vTaskDelay(pdMS_TO_TICKS(20)); // Petite pause de 20ms pour laisser souffler l'I2C
        readOk = shtSensor.sensor.readSample(); // Deuxième tentative
    }
    
    if (readOk) {
        float temp = shtSensor.sensor.getTemperature();
        float hum = shtSensor.sensor.getHumidity();
        
        if (isnan(temp) || isnan(hum) || (temp == 0 && hum == 0)) {
            DEBUG_PRINTLN("Erreur de lecture du SHT : valeurs invalides ignorées.");
            // Au lieu de tuer le capteur, on sort simplement pour garder la dernière valeur connue
            return;
        }
        
        shtSensor.temperature = temp + correction_temperature_sht;
        shtSensor.humidite = hum;
        
        float a = 17.27;
        float b = 237.7;
        float gamma = (a * shtSensor.temperature) / (b + shtSensor.temperature) + log(hum / 100.0);
        shtSensor.dewpoint = (b * gamma) / (a - gamma);
        
        DEBUG_PRINTF("[SHT] Temp: %.2f°C, Humidité: %.2f%%, Point de rosée: %.2f°C\n",
                     shtSensor.temperature, shtSensor.humidite, shtSensor.dewpoint);
    } else {
        DEBUG_PRINTLN("Erreur de communication persistante avec le SHT.");
        shtSensor.available = false; // On ne tue le capteur que s'il est vraiment injoignable
    }
}
#endif

void sleepSHT() {
    if (!shtSensor.available || shtSensor.isSleeping) return;
    
    #ifndef SIMULATION_MODE
    // Les commandes I2C codées en dur (ex: 0x44) ont été retirées. 
    // La librairie Sensirion utilise le mode "Single Shot" par défaut :
    // Le capteur se met de lui-même en veille (Idle) après chaque mesure.
    #endif
    
    shtSensor.isSleeping = true;
    DEBUG_PRINTLN("[SHT] Capteur en mode sleep (Auto Idle)");
}

void wakeSHT() {
    if (!shtSensor.available || !shtSensor.isSleeping) return;
    
    #ifndef SIMULATION_MODE
    // Le réveil sera géré automatiquement de manière transparente 
    // par readSample() lors de la prochaine lecture.
    #endif
    
    shtSensor.isSleeping = false;
    DEBUG_PRINTLN("[SHT] Capteur prêt pour la mesure");
}

bool isSHTSleeping() {
    return shtSensor.isSleeping;
}

bool isSHTAvailable() { return shtSensor.available; }
float getTemperature_SHT() { 
    if (!shtSensor.available) return -999.0;
    return shtSensor.temperature; 
}
float getHumidity_SHT() { 
    if (!shtSensor.available) return -999.0;
    return shtSensor.humidite; 
}
float getDewpoint_SHT() { 
    if (!shtSensor.available) return -999.0;
    return shtSensor.dewpoint; 
}
