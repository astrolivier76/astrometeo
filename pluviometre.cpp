#include <Arduino.h>
#include "pluviometre.h"
#include "debug.h"

namespace {
    struct Pluviometre {
        const uint8_t pin = 26;
        volatile unsigned long compteur_rain = 0;
        unsigned long dernierEnvoi = 0;
        const uint32_t debounce_time_pluvio = 50; // 50ms pour le pluviomètre
        float quantite = 0.0;
        portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    };
    Pluviometre pluviometre;
}

// Constantes spécifiques à votre pluviomètre
const float MM_PAR_BASCULEMENT = 0.2794; // 1 basculement = 0.2794 mm

void IRAM_ATTR cntPluviometre() {
    static uint32_t last_interrupt_time = 0;
    uint32_t interrupt_time = millis();
    
    // Débounce logiciel
    if (interrupt_time - last_interrupt_time > pluviometre.debounce_time_pluvio) {
        portENTER_CRITICAL_ISR(&pluviometre.mux);
        pluviometre.compteur_rain++;
        portEXIT_CRITICAL_ISR(&pluviometre.mux);
    }
    last_interrupt_time = interrupt_time;
}

void init_pluviometre() {
    pinMode(pluviometre.pin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pluviometre.pin), cntPluviometre, FALLING);
    pluviometre.dernierEnvoi = millis();
}

void getSendPluviometre(int measurementPeriodOfRain) {
    unsigned long count;
    
    // Lecture sécurisée du compteur
    portENTER_CRITICAL(&pluviometre.mux);
    count = pluviometre.compteur_rain;
    pluviometre.compteur_rain = 0;
    portEXIT_CRITICAL(&pluviometre.mux);
    
    DEBUG_PRINTF("Basculements pluie = %lu\n", count);
    DEBUG_PRINTF("Période de mesure = %d sec\n", measurementPeriodOfRain);
    
    // Calcul du taux de précipitation en mm/h
    if (measurementPeriodOfRain > 0) {
        float quantite_mesuree = count * MM_PAR_BASCULEMENT;
        pluviometre.quantite = (quantite_mesuree / measurementPeriodOfRain) * 3600.0;
    } else {
        pluviometre.quantite = 0.0;
    }
    
    DEBUG_PRINTF("Quantité Pluie = %.3f mm/h\n", pluviometre.quantite);
}

float getValPluviometre() {
    return pluviometre.quantite;
}
