#include <Arduino.h>
#include "SHT.h"
#include <Wire.h>
#include <SHT3x.h>
//#define SIMULATION_MODE
#include "debug.h"

const float seuil_chauffage = 2.0;

namespace {
    struct SHT3xSensor {
        SHT3x sensor;
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
    SHT3xSensor shtSensor;
}

#ifdef SIMULATION_MODE
bool initSHT() {
    DEBUG_PRINTLN("[SHT3x] Mode simulation activé.");
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

    DEBUG_PRINTF("[SHT3x] Temp: %.2f°C, Humidité: %.2f%%\n",
                 shtSensor.temperature, shtSensor.humidite);
}
#else

bool initSHT() {
    DEBUG_PRINTLN("[SHT3x] Initialisation...");
    const int maxRetries = 3;
    
    for (int i = 0; i < maxRetries; i++) {
        shtSensor.sensor.Begin();
        
        shtSensor.sensor.UpdateData();
        float temp = shtSensor.sensor.GetTemperature();
        float hum = shtSensor.sensor.GetRelHumidity();
        
        if (!isnan(temp) && !isnan(hum) && temp != 0 && hum != 0) {
            shtSensor.available = true;
            shtSensor.retryCount = 0;
            DEBUG_PRINTLN("SHT3x initialisé avec succès.");
            return true;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
        DEBUG_PRINTF("Tentative %d/3 échouée.\n", i + 1);
    }
    
    shtSensor.available = false;
    DEBUG_PRINTLN("Erreur : Impossible d'initialiser le SHT3x.");
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
            DEBUG_PRINTF("[SHT3x] Tentative de récupération %d/%d\n", shtSensor.retryCount, shtSensor.maxRetries);
            
            if (initSHT()) {
                DEBUG_PRINTLN("[SHT3x] Capteur récupéré avec succès!");
                return; // Sortir pour éviter la lecture immédiate
            } else {
                DEBUG_PRINTF("[SHT3x] Échec de la récupération %d/%d\n", shtSensor.retryCount, shtSensor.maxRetries);
                
                if (shtSensor.retryCount >= shtSensor.maxRetries) {
                    DEBUG_PRINTLN("[SHT3x] Abandon après 3 tentatives échouées");
                }
            }
        }
        return; // Ne pas tenter de lecture si capteur non disponible
    }
    
    shtSensor.sensor.UpdateData();
    float temp = shtSensor.sensor.GetTemperature();
    float hum = shtSensor.sensor.GetRelHumidity();
    
    if (isnan(temp) || isnan(hum) || temp == 0 || hum == 0) {
        DEBUG_PRINTLN("Erreur de lecture du SHT3x : valeurs invalides.");
        shtSensor.available = false;
        return;
    }
    
    shtSensor.temperature = temp;
    shtSensor.humidite = hum;
    
    float a = 17.27;
    float b = 237.7;
    float gamma = (a * shtSensor.temperature) / (b + shtSensor.temperature) + log(hum / 100.0);
    shtSensor.dewpoint = (b * gamma) / (a - gamma);
    
    DEBUG_PRINTF("[SHT3x] Temp: %.2f°C, Humidité: %.2f%%, Point de rosée: %.2f°C\n",
                 shtSensor.temperature, shtSensor.humidite, shtSensor.dewpoint);
}
#endif

void sleepSHT() {
    if (!shtSensor.available || shtSensor.isSleeping) return;
    
    #ifndef SIMULATION_MODE
    Wire.beginTransmission(0x44);
    Wire.write(0xB0);
    Wire.write(0x98);
    Wire.endTransmission();
    #endif
    
    shtSensor.isSleeping = true;
    DEBUG_PRINTLN("[SHT3x] Capteur en mode sleep");
}

void wakeSHT() {
    if (!shtSensor.available || !shtSensor.isSleeping) return;
    
    #ifndef SIMULATION_MODE
    Wire.beginTransmission(0x44);
    Wire.write(0x35);
    Wire.write(0x17);
    Wire.endTransmission();
    vTaskDelay(pdMS_TO_TICKS(5));
    #endif
    
    shtSensor.isSleeping = false;
    DEBUG_PRINTLN("[SHT3x] Capteur réveillé");
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

int needChauffage_SHT() {
    if (!shtSensor.available) return 0;
    return (shtSensor.temperature <= shtSensor.dewpoint + seuil_chauffage) ? 1 : 0;
}
