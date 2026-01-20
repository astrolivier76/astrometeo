#include <Arduino.h>
#include <Adafruit_MLX90614.h>
#include <Wire.h>
#include "debug.h"
#include "variablesWEB.h"
#include "SHT.h"
#define SHT

namespace {
    struct MLX90614Sensor {
        Adafruit_MLX90614 mlx;
        bool available = false;
        bool isSleeping = false;
        float temperature_ambiante = -999;
        float temperature_ciel = -999;
        float temperature_ciel_corrigee = -999;
        float nuages = -999;
        int safe_nuages = -999;
        int retryCount = 0;
        unsigned long lastRetryTime = 0;
        const int maxRetries = 3;
        const unsigned long retryInterval = 30000; // 30 secondes
    };
    MLX90614Sensor sensor;
}

float Sign(float x) {
    if (x > 0) return 1;
    if (x < 0) return -1;
    return 0;
}

float calculerT67(float Ta, float K2, float K6, float K7) {
    float terme = (K2 / 10.0 - Ta);
    float abs_terme = abs(terme);
    
    if (K6 == 0) {
        return 0;
    }
    
    if (abs_terme < 1) {
        return Sign(K6) * Sign(terme) * abs_terme;
    } else {
        return (K6 / 10.0) * Sign(terme) * (log(abs_terme) / log(10.0) + K7 / 100.0);
    }
}

bool initMLX() {
    #ifdef SIMULATION_MODE
        DEBUG_PRINTLN("[MLX90614] Mode simulation activé.");
        sensor.available = true;
        return true;
    #else
        const int maxInitRetries = 3;
        for (int i = 0; i < maxInitRetries; i++) {
            if (sensor.mlx.begin()) {
                sensor.available = true;
                sensor.retryCount = 0;
                DEBUG_PRINTLN("MLX90614 initialisé avec succès.");
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        sensor.available = false;
        DEBUG_PRINTLN("Erreur : Impossible d'initialiser le MLX90614.");
        return false;
    #endif
}

void updateMLX() {
    // Tentative de récupération si capteur défaillant
    if (!sensor.available) {
        unsigned long currentMillis = millis();
        if (sensor.retryCount < sensor.maxRetries && 
            (currentMillis - sensor.lastRetryTime >= sensor.retryInterval)) {
            
            sensor.retryCount++;
            sensor.lastRetryTime = currentMillis;
            DEBUG_PRINTF("[MLX90614] Tentative de récupération %d/%d\n", sensor.retryCount, sensor.maxRetries);
            
            if (initMLX()) {
                DEBUG_PRINTLN("[MLX90614] Capteur récupéré avec succès!");
                return; // Sortir pour éviter la lecture immédiate
            } else {
                DEBUG_PRINTF("[MLX90614] Échec de la récupération %d/%d\n", sensor.retryCount, sensor.maxRetries);
                
                if (sensor.retryCount >= sensor.maxRetries) {
                    DEBUG_PRINTLN("[MLX90614] Abandon après 3 tentatives échouées");
                }
            }
        }
        return; // Ne pas tenter de lecture si capteur non disponible
    }

    #ifdef SIMULATION_MODE
        sensor.temperature_ambiante = 24.0 + random(-1, 1) * 0.1;
        sensor.temperature_ciel = -5.0 + random(-1, 1) * 0.2;
    #else
        #ifdef SHT
        sensor.temperature_ambiante = getTemperature_SHT();
        sensor.temperature_ciel = sensor.mlx.readObjectTempC();
        if (isnan(sensor.temperature_ambiante) || isnan(sensor.temperature_ciel)) {
            DEBUG_PRINTLN("Erreur de lecture du MLX90614 : valeurs invalides.");
            sensor.available = false;
            return;
        }
        #else
        sensor.temperature_ambiante = sensor.mlx.readAmbientTempC();
        sensor.temperature_ciel = sensor.mlx.readObjectTempC();
        if (isnan(sensor.temperature_ambiante) || isnan(sensor.temperature_ciel)) {
            DEBUG_PRINTLN("Erreur de lecture du MLX90614 : valeurs invalides.");
            sensor.available = false;
            return;
        }
        #endif
    #endif

    // CALCUL EXACT SELON LA FORMULE LUNATICO
    float T67 = calculerT67(sensor.temperature_ambiante, K2, K6, K7);
    
    float correction_temperature = (K1 / 100.0) * (sensor.temperature_ambiante - K2 / 10.0) + (K3 / 100.0) * pow(exp(K4 / 1000.0 * sensor.temperature_ambiante), (K5 / 100.0)) + T67;

    sensor.temperature_ciel_corrigee = sensor.temperature_ciel - correction_temperature;

    // Calcul du pourcentage de nuages
    if (sensor.temperature_ciel_corrigee < temperature_ciel_clair) {
        sensor.nuages = 0.0;
        sensor.safe_nuages = 1;
    } else if (sensor.temperature_ciel_corrigee > temperature_ciel_couvert) {
        sensor.nuages = 100.0;
        sensor.safe_nuages = 0;
    } else {
        sensor.nuages = map(sensor.temperature_ciel_corrigee * 100, 
                           temperature_ciel_clair * 100, 
                           temperature_ciel_couvert * 100, 0, 100);
        sensor.nuages = constrain(sensor.nuages, 0, 100);
        sensor.safe_nuages = (sensor.nuages <= 25) ? 1 : 0;
    }

    DEBUG_PRINTF("[MLX90614] Temp ambiante: %.2f°C, Temp ciel: %.2f°C, Correction: %.2f°C, Nuages: %.1f%%\n",
                 sensor.temperature_ambiante, sensor.temperature_ciel_corrigee, correction_temperature, sensor.nuages);
    DEBUG_PRINTF("[MLX90614] T67: %.3f, K1=%.1f, K2=%.1f, K3=%.1f, K4=%.1f, K5=%.1f, K6=%.1f, K7=%.1f\n",
                 T67, K1, K2, K3, K4, K5, K6, K7);
}

void sleepMLX() {
    if (!sensor.available || sensor.isSleeping) return;
    
    #ifndef SIMULATION_MODE
    // Commande de sleep pour MLX90614
    Wire.beginTransmission(0x5A);
    Wire.write(0xFF);
    Wire.write(0xF6);
    Wire.endTransmission();
    #endif
    
    sensor.isSleeping = true;
    DEBUG_PRINTLN("[MLX90614] Capteur en mode sleep");
}

void wakeMLX() {
    if (!sensor.available || !sensor.isSleeping) return;
    
    #ifndef SIMULATION_MODE
    // Réveil par une lecture simple
    Wire.beginTransmission(0x5A);
    Wire.endTransmission();
    vTaskDelay(pdMS_TO_TICKS(50));
    #endif
    
    sensor.isSleeping = false;
    DEBUG_PRINTLN("[MLX90614] Capteur réveillé");
}

bool isMLXSleeping() {
    return sensor.isSleeping;
}

bool isMLXAvailable() { return sensor.available; }
float getTemperature_Sky() { 
    if (!sensor.available) return -999.0;
    return sensor.temperature_ciel_corrigee; 
}
float getNuages() { 
    if (!sensor.available) return -999.0;
    return sensor.nuages; 
}
int getSafeNuages() { 
    if (!sensor.available) return -999;
    return sensor.safe_nuages; 
}
