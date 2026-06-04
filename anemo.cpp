// anemo.cpp
#include "anemo.h"
#include <Arduino.h>
#include "debug.h"

namespace {
    struct Anemometre {
        const uint8_t pin = 14;
        volatile unsigned long compteur_wind = 0;
        float vitesse = 0.0;          // dernière valeur lissée/valide
        portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    };
    Anemometre anemometre;
}

// Constantes spécifiques à votre anémomètre
const float KMH_PAR_TOUR = 2.4;          // 1 tour = 2.4 km/h
const float IMPULSIONS_PAR_TOUR = 2;     // Typiquement 2 impulsions par tour (rising/falling)
const uint32_t DEBOUNCE_TIME_WIND = 7.5; // 7,5ms pour capturer les hautes vitesses (max de 180 km/h)

// Paramètres de filtrage
#define MAX_PHYSICAL_SPEED 200.0f   // km/h : au-delà, on considère la valeur suspecte
#define MAX_JUMP_FACTOR 4.0f        // si raw > last_valid * MAX_JUMP_FACTOR -> suspect
#define MAX_JUMP_ABS 30.0f          // si jump absolu > 30 km/h -> suspect
#define EMA_ALPHA 0.35f             // coefficient pour lissage (0..1)

// ISR pour le compteur d’impulsions
void IRAM_ATTR cntAnemometre() {
    static uint32_t last_interrupt_time = 0;
    uint32_t interrupt_time = millis();

    // Débounce logiciel
    if (interrupt_time - last_interrupt_time > DEBOUNCE_TIME_WIND) {
        portENTER_CRITICAL_ISR(&anemometre.mux);
        anemometre.compteur_wind++;
        portEXIT_CRITICAL_ISR(&anemometre.mux);
    }
    last_interrupt_time = interrupt_time;
}

// Initialisation
void init_vent() {
    pinMode(anemometre.pin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(anemometre.pin), cntAnemometre, FALLING);
}

// Calcul de la vitesse sur une période donnée
void getSendVitesseVent(int measurementPeriodOfWind) {
    if (measurementPeriodOfWind < 1) measurementPeriodOfWind = 1;

    unsigned long count;
    
    // Lecture sécurisée du compteur
    portENTER_CRITICAL(&anemometre.mux);
    count = anemometre.compteur_wind;
    anemometre.compteur_wind = 0;
    portEXIT_CRITICAL(&anemometre.mux);

    // Conversion impulsions -> tours
    float tours = (float)count / IMPULSIONS_PAR_TOUR;
    float tours_par_seconde = tours / (float)measurementPeriodOfWind;
    float raw_vitesse = tours_par_seconde * KMH_PAR_TOUR; // vitesse brute

    DEBUG_PRINTF("Impulsions=%lu, raw_vitesse=%.2f km/h\n", count, raw_vitesse);

    // Premier garde-fou : clamp physique
    bool suspect = false;
    if (raw_vitesse > MAX_PHYSICAL_SPEED) {
        DEBUG_PRINTF("Mesure suspecte : au-dessus de MAX_PHYSICAL_SPEED (%.2f)\n", raw_vitesse);
        suspect = true;
    }

    // Deuxième garde-fou : variation trop importante vs dernière valeur valide
    float last = anemometre.vitesse; // dernière valeur lissée/valide
    if (!suspect && last > 0.001f) { // si on a une valeur précédente
        float rel = (raw_vitesse / last);
        float diff = fabs(raw_vitesse - last);
        if (rel > MAX_JUMP_FACTOR || diff > MAX_JUMP_ABS) {
            DEBUG_PRINTF("Mesure suspecte : saut trop important (raw=%.2f, last=%.2f)\n", raw_vitesse, last);
            suspect = true;
        }
    }

    // Application du filtre
    float newV;
    if (suspect) {
        newV = last; // conserver dernière valeur stable
    } else {
        newV = EMA_ALPHA * raw_vitesse + (1.0f - EMA_ALPHA) * last;
    }

    anemometre.vitesse = newV;
    DEBUG_PRINTF("Vitesse finale = %.2f km/h (raw=%.2f, suspect=%d)\n", anemometre.vitesse, raw_vitesse, suspect);
}

// Getter pour vitesse
float getVent() {
    return anemometre.vitesse;
}
