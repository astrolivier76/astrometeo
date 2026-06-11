#include <Arduino.h>
#include "gouttes.h"
#include "debug.h"
#include <math.h>

// --- STRUCTURE POUR LE CAPTEUR RC-SPC1K ---
namespace {
    struct CapteurGouttes {
        const uint8_t pinFreq = 34;       // Lecture de la pluie (LM555)
        const uint8_t pinNTC = 32;        // Lecture analogique de la température de surface
        const uint8_t pinHeater = 13;     // Pilotage du MOSFET pour la résistance
        
        int valeur = 0;                   // 1 = pluie, 0 = sec
        unsigned long frequence = 0;      
        float tempSurface = 0.0;          // Température du capteur en °C
    };
    CapteurGouttes capteur;

    volatile unsigned long impulsions = 0;

    void IRAM_ATTR compterImpulsions() {
        impulsions++;
    }
}

void init_GOUTTES() {
    pinMode(capteur.pinFreq, INPUT); 
    pinMode(capteur.pinHeater, OUTPUT);
    digitalWrite(capteur.pinHeater, LOW); // Chauffage éteint par défaut
}

// Fonction utilitaire pour lire la NTC
void lireTemperatureSurface() {
    int adcVal = analogRead(capteur.pinNTC);
    
    // Sécurité pour éviter les divisions par zéro
    if (adcVal > 0 && adcVal < 4095) {
        // Pont diviseur: Vout = 3.3V * (RNTC / (R2 + RNTC))
        // R2 = 1000 ohms. ADC sur 12 bits (4095).
        float rNtc = 1000.0 * ((float)adcVal / (4095.0 - (float)adcVal));
        
        // Calcul via l'équation simplifiée (Beta approx. 3400 pour cette NTC 1k)
        float temp = rNtc / 1000.0;          // (R/Ro)
        temp = log(temp);                    // ln(R/Ro)
        temp /= 3400.0;                      // 1/B * ln(R/Ro)
        temp += 1.0 / (25.0 + 273.15);       // + (1/To)
        temp = 1.0 / temp;                   // Inversion
        capteur.tempSurface = temp - 273.15; // Conversion en Celsius
        
        DEBUG_PRINTF("[CAPTEUR PLUIE] Température surface : %.1f °C\n", capteur.tempSurface);
    }
}

void updateGOUTTES() {
    // 1. Lecture de la NTC
    lireTemperatureSurface();

    // 2. Mesure de la fréquence (lissage sur 500 ms)
    impulsions = 0;
    attachInterrupt(digitalPinToInterrupt(capteur.pinFreq), compterImpulsions, RISING);
    vTaskDelay(pdMS_TO_TICKS(500)); 
    detachInterrupt(digitalPinToInterrupt(capteur.pinFreq));

    capteur.frequence = impulsions * 2;
    DEBUG_PRINTF("[CAPTEUR PLUIE] Fréquence : %lu Hz\n", capteur.frequence);

    // 3. Déduction de l'état
    const unsigned long SEUIL_PLUIE = 4000; 

    if (capteur.frequence == 0) {
        capteur.valeur = 1;
    } else if (capteur.frequence < SEUIL_PLUIE) {
        capteur.valeur = 1;
    } else {
        capteur.valeur = 0;
    }
}

// AJOUT : Implémentation du Getter pour la température de surface
float getTemperatureSurfaceGouttes() {
    return capteur.tempSurface;
}

int getGOUTTES() {
    return capteur.valeur;
}
