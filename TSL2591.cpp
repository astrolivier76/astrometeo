#include <Arduino.h>
#include <Wire.h>
#include "debug.h"
//#define SIMULATION_MODE
#include <Adafruit_Sensor.h>
#include <Adafruit_TSL2591.h>
Adafruit_TSL2591 tsl = Adafruit_TSL2591();

// --- CONSTANTES TSL2591 ---
float lux = 0.00;
float sqm = 0.00;
bool tslAvailable = false;
int tslRetryCount = 0;
unsigned long tslLastRetryTime = 0;
const int tslMaxRetries = 3;
const unsigned long tslRetryInterval = 30000; // 30 secondes

// --- MODE SIMULATION ---
#ifdef SIMULATION_MODE
bool isTSL2591Present(){
    tslAvailable = true;
    return true;
}
bool calibrateTSL2591(){
    return true;
}
void updateTSL2591(){}
float getLux() {
  if (!tslAvailable) return -999.0;
  lux = 500 + random(-100, 100);
  return lux;
}

float getSQM() {
  if (!tslAvailable) return -999.0;
  updateTSL2591();
  lux = getLux();
  sqm = log10(lux/108000)/-0.4;
  return sqm;
}

#else

// --- MODE REEL ---
struct {
  bool status;
  uint32_t full;
  uint16_t ir;
  uint16_t visible;
  int      gain;
  int      timing;
  float    lux;
} tsl2591Data {false, 0, 0, 0, 0, 0, 0.0};

bool isTSL2591Present() {
  Wire.beginTransmission(TSL2591_ADDR);
  byte error = Wire.endTransmission();
  tslAvailable = (error == 0);
  return tslAvailable;
}

bool initTSL2591() {
    if (tsl.begin()) {
        tslAvailable = true;
        tslRetryCount = 0;
        
        // Configuration initiale
        tsl.setGain(TSL2591_GAIN_MED);
        tsl.setTiming(TSL2591_INTEGRATIONTIME_200MS);
        
        DEBUG_PRINTLN("TSL2591 initialisé avec succès.");
        return true;
    }
    tslAvailable = false;
    DEBUG_PRINTLN("Erreur: Impossible d'initialiser le TSL2591.");
    return false;
}

void configureSensorTSL2591(tsl2591Gain_t gainSetting, tsl2591IntegrationTime_t timeSetting) {
  if (!tslAvailable) return;
  tsl.setGain(gainSetting);
  tsl.setTiming(timeSetting);
}

bool calibrateTSL2591() {
  if (!tslAvailable) return false;
  
  if (tsl2591Data.visible < 100) {
    switch (tsl2591Data.gain) {
      case TSL2591_GAIN_LOW :
        configureSensorTSL2591(TSL2591_GAIN_MED, TSL2591_INTEGRATIONTIME_200MS);
        break;
      case TSL2591_GAIN_MED :
        configureSensorTSL2591(TSL2591_GAIN_HIGH, TSL2591_INTEGRATIONTIME_200MS);
        break;
      case TSL2591_GAIN_HIGH :
        configureSensorTSL2591(TSL2591_GAIN_MAX, TSL2591_INTEGRATIONTIME_200MS);
        break;
      case TSL2591_GAIN_MAX :
        switch (tsl2591Data.timing) {
          case TSL2591_INTEGRATIONTIME_200MS :
            configureSensorTSL2591(TSL2591_GAIN_MAX, TSL2591_INTEGRATIONTIME_300MS);
            break;
          case TSL2591_INTEGRATIONTIME_300MS :
            configureSensorTSL2591(TSL2591_GAIN_MAX, TSL2591_INTEGRATIONTIME_400MS);
            break;
          case TSL2591_INTEGRATIONTIME_400MS :
            configureSensorTSL2591(TSL2591_GAIN_MAX, TSL2591_INTEGRATIONTIME_500MS);
            break;
          case TSL2591_INTEGRATIONTIME_500MS :
            configureSensorTSL2591(TSL2591_GAIN_MAX, TSL2591_INTEGRATIONTIME_600MS);
            break;
          case TSL2591_INTEGRATIONTIME_600MS :
            return false;
            break;
          default:
            configureSensorTSL2591(TSL2591_GAIN_MAX, TSL2591_INTEGRATIONTIME_600MS);
            break;
        }
        break;
      default:
        configureSensorTSL2591(TSL2591_GAIN_MED, TSL2591_INTEGRATIONTIME_200MS);
        break;
    }
    return true;
  }

  if (tsl2591Data.visible > 30000) {
    switch (tsl2591Data.gain) {
      case TSL2591_GAIN_LOW :
        switch (tsl2591Data.timing) {
          case TSL2591_INTEGRATIONTIME_500MS :
            configureSensorTSL2591(TSL2591_GAIN_LOW, TSL2591_INTEGRATIONTIME_400MS);
            break;
          case TSL2591_INTEGRATIONTIME_400MS :
            configureSensorTSL2591(TSL2591_GAIN_LOW, TSL2591_INTEGRATIONTIME_300MS);
            break;
          case TSL2591_INTEGRATIONTIME_300MS :
            configureSensorTSL2591(TSL2591_GAIN_LOW, TSL2591_INTEGRATIONTIME_200MS);
            break;
          case TSL2591_INTEGRATIONTIME_200MS :
            return false;
            break;
          default:
            configureSensorTSL2591(TSL2591_GAIN_LOW, TSL2591_INTEGRATIONTIME_200MS);
            break;
        }
        break;
      case TSL2591_GAIN_MED :
        configureSensorTSL2591(TSL2591_GAIN_LOW, TSL2591_INTEGRATIONTIME_200MS);
        break;
      case TSL2591_GAIN_HIGH :
        configureSensorTSL2591(TSL2591_GAIN_MED, TSL2591_INTEGRATIONTIME_200MS);
        break;
      case TSL2591_GAIN_MAX :
        configureSensorTSL2591(TSL2591_GAIN_HIGH, TSL2591_INTEGRATIONTIME_200MS);
        break;
      default:
        configureSensorTSL2591(TSL2591_GAIN_MED, TSL2591_INTEGRATIONTIME_200MS);
        break;
    }
    return true;
  }
  return false;
}

// --- CORRECTION RECURSION INFINIE ---
// Ajout d'une variable statique pour limiter la profondeur de récursion
void updateTSL2591() {
    static int recursionDepth = 0; // Compteur de sécurité

    // Tentative de récupération si capteur défaillant
    if (!tslAvailable) {
        unsigned long currentMillis = millis();
        if (tslRetryCount < tslMaxRetries && 
            (currentMillis - tslLastRetryTime >= tslRetryInterval)) {
            
            tslRetryCount++;
            tslLastRetryTime = currentMillis;
            DEBUG_PRINTF("[TSL2591] Tentative de récupération %d/%d\n", tslRetryCount, tslMaxRetries);
            
            if (initTSL2591()) {
                DEBUG_PRINTLN("[TSL2591] Capteur récupéré avec succès!");
                return; // Sortir pour éviter la lecture immédiate
            } else {
                DEBUG_PRINTF("[TSL2591] Échec de la récupération %d/%d\n", tslRetryCount, tslMaxRetries);
                
                if (tslRetryCount >= tslMaxRetries) {
                    DEBUG_PRINTLN("[TSL2591] Abandon après 3 tentatives échouées");
                }
            }
        }
        return; // Ne pas tenter de lecture si capteur non disponible
    }
  
    if (!tsl.begin()) {
        tslAvailable = false;
        DEBUG_PRINTLN("Erreur: TSL2591 non disponible");
        return;
    }
    
    tsl2591Data.full    = tsl.getFullLuminosity();
    tsl2591Data.ir      = tsl2591Data.full >> 16;
    tsl2591Data.visible = tsl2591Data.full & 0xFFFF;
    tsl2591Data.lux     = tsl.calculateLux(tsl2591Data.visible, tsl2591Data.ir);
    tsl2591Data.gain    = tsl.getGain();
    tsl2591Data.timing  = tsl.getTiming();

    bool changed = calibrateTSL2591();
    
    // --- GESTION DE LA RECURSION SECURISEE ---
    if (changed) {
        if (recursionDepth < 3) {
            DEBUG_PRINTF("[TSL2591] Gain ajusté, nouvelle mesure (profondeur %d)...\n", recursionDepth);
            recursionDepth++;
            updateTSL2591();
            recursionDepth--; // Décrémenter en sortant
        } else {
            DEBUG_PRINTLN("[TSL2591] AVERTISSEMENT : Limite de récursion atteinte, arrêt de l'ajustement du gain.");
        }
    }
}

float getLux() {
  if (!tslAvailable) return -999.0;
  lux = tsl2591Data.lux;
  return lux;
}

float getSQM() {
  if (!tslAvailable) return -999.0;
  if (tsl2591Data.lux <= 0) return 0.0; // Eviter log10(0) ou valeurs négatives
  return log10(tsl2591Data.lux/108000.0)/-0.4;
}

#endif
