#include <Arduino.h>
#include "BME280.h"
#include <Wire.h>
#include <BME280I2C.h>
//#define SIMULATION_MODE
#include "debug.h"

float correction_altitude_pression = 6.0;
const float seuil_chauffage = 2.0;

namespace {
    struct BME280Sensor {
        BME280I2C bme;
        bool available = false;
        bool isSleeping = false;
        float temperature = -999;
        float pression = -999;
        float humidite = -999;
        float dewpoint = -999;
        int retryCount = 0;
        unsigned long lastRetryTime = 0;
        const int maxRetries = 3;
        const unsigned long retryInterval = 30000; // 30 secondes
    };
    BME280Sensor sensor;
}

#ifdef SIMULATION_MODE
bool initBME() {
    DEBUG_PRINTLN("[BME280] Mode simulation activé.");
    sensor.available = true;
    return true;
}

void updateBME() {
    if (!sensor.available) return;

    sensor.temperature = 25 + random(-20, 20) * 0.1;
    sensor.pression = 1013.25 + random(-10, 10) * 0.01;
    sensor.humidite = 50 + random(-10, 10) * 0.5;
    sensor.dewpoint = 10 + random(-10, 10) * 0.1;

    DEBUG_PRINTF("[BME280] Temp: %.2f°C, Pression: %.2fhPa, Humidité: %.2f%%\n",
                 sensor.temperature, sensor.pression, sensor.humidite);
}
#else

bool initBME() {
    DEBUG_PRINTLN("[BME280] Initialisation...");
    
    // Configuration pour le mode forced (économie d'énergie)
    BME280I2C::Settings settings(
        BME280::OSR_X1,    // temp
        BME280::OSR_X1,    // humidité  
        BME280::OSR_X1,    // pression
        BME280::Mode_Forced, // Mode forced pour économie
        BME280::StandbyTime_1000ms,
        BME280::Filter_Off,
        BME280::SpiEnable_False,
        BME280I2C::I2CAddr_0x76
    );
    
    sensor.bme.setSettings(settings);
    
    const int maxRetries = 3;
    for (int i = 0; i < maxRetries; i++) {
        if (sensor.bme.begin()) {
            sensor.available = true;
            sensor.retryCount = 0;
            DEBUG_PRINTLN("BME280 initialisé avec succès en mode forced.");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        DEBUG_PRINTF("Tentative %d/3 échouée.\n", i + 1);
    }
    sensor.available = false;
    DEBUG_PRINTLN("Erreur : Impossible d'initialiser le BME280.");
    return false;
}

void updateBME() {
    // Tentative de récupération si capteur défaillant
    if (!sensor.available) {
        unsigned long currentMillis = millis();
        if (sensor.retryCount < sensor.maxRetries && 
            (currentMillis - sensor.lastRetryTime >= sensor.retryInterval)) {
            
            sensor.retryCount++;
            sensor.lastRetryTime = currentMillis;
            DEBUG_PRINTF("[BME280] Tentative de récupération %d/%d\n", sensor.retryCount, sensor.maxRetries);
            
            if (initBME()) {
                DEBUG_PRINTLN("[BME280] Capteur récupéré avec succès!");
                return; // Sortir pour éviter la lecture immédiate
            } else {
                DEBUG_PRINTF("[BME280] Échec de la récupération %d/%d\n", sensor.retryCount, sensor.maxRetries);
                
                if (sensor.retryCount >= sensor.maxRetries) {
                    DEBUG_PRINTLN("[BME280] Abandon après 3 tentatives échouées");
                }
            }
        }
        return; // Ne pas tenter de lecture si capteur non disponible
    }
    
    // En mode forced, on doit déclencher une mesure
    if (sensor.isSleeping) {
        wakeBME(); // Réveille le capteur si endormi
    }
    
    float temp(NAN), hum(NAN), pres(NAN);
    BME280::TempUnit tempUnit(BME280::TempUnit_Celsius);
    BME280::PresUnit presUnit(BME280::PresUnit_hPa);
    
    // Lecture des données
    sensor.bme.read(pres, temp, hum, tempUnit, presUnit);
    
    if (isnan(temp) || isnan(hum) || isnan(pres)) {
        DEBUG_PRINTLN("Erreur de lecture du BME280 : valeurs invalides.");
        sensor.available = false;
        return;
    }
    
    sensor.temperature = temp;
    sensor.pression = pres;
    sensor.pression = sensor.pression + correction_altitude_pression;
    sensor.humidite = hum;
    
    // Calcul du point de rosée
    float a = 17.27;
    float b = 237.7;
    float gamma = (a * sensor.temperature) / (b + sensor.temperature) + log(hum / 100.0);
    sensor.dewpoint = (b * gamma) / (a - gamma);
    
    DEBUG_PRINTF("[BME280] Temp: %.2f°C, Pression: %.2fhPa, Humidité: %.2f%%, Rosée: %.2f°C\n",
                 sensor.temperature, sensor.pression, sensor.humidite, sensor.dewpoint);
}
#endif

void sleepBME() {
    if (!sensor.available || sensor.isSleeping) return;
    
    #ifndef SIMULATION_MODE
    // Le BME280 en mode Forced se met automatiquement en sleep après chaque mesure
    // On marque simplement le statut
    #endif
    
    sensor.isSleeping = true;
    DEBUG_PRINTLN("[BME280] Capteur en mode sleep");
}

void wakeBME() {
    if (!sensor.available || !sensor.isSleeping) return;
    
    #ifndef SIMULATION_MODE
    // En mode Forced, le simple fait de lire les données réveille le capteur
    // et déclenche une nouvelle mesure
    // On attend un peu pour que la mesure soit prête
    vTaskDelay(pdMS_TO_TICKS(10)); // Temps pour la mesure
    #endif
    
    sensor.isSleeping = false;
    DEBUG_PRINTLN("[BME280] Capteur réveillé");
}

bool isBMESleeping() {
    return sensor.isSleeping;
}

bool isBMEAvailable() { return sensor.available; }
float getTemperature_BME() { 
    if (!sensor.available) return -999.0;
    return sensor.temperature; 
}
float getHumidity_BME() { 
    if (!sensor.available) return -999.0;
    return sensor.humidite; 
}
float getPressure_BME() { 
    if (!sensor.available) return -999.0;
    return sensor.pression; 
}
float getDewpoint_BME() { 
    if (!sensor.available) return -999.0;
    return sensor.dewpoint; 
}

int needChauffage_BME280() {
    if (!sensor.available) return 0;
    return (sensor.temperature <= sensor.dewpoint + seuil_chauffage) ? 1 : 0;
}
