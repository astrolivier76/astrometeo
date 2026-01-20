#include <Arduino.h>
#include "gouttes.h"
#include "debug.h"

// --- STRUCTURE POUR UTILISER LE CAPTEUR DE GOUTTES ---
namespace {
    struct CapteurGouttes {
        const uint8_t pin = 32;           // pin pour la lecture de la mesure
        const uint8_t alimPin = 33;       // pin sortant du 3,3V -> permet de n'alimenté le capteur que lors de la mesure
        int valeur = 0;
    };
    CapteurGouttes capteur;
}

void init_GOUTTES() {
    pinMode(capteur.pin, INPUT_PULLUP);
    pinMode(capteur.alimPin, OUTPUT);
    digitalWrite(capteur.alimPin, LOW);
}

void updateGOUTTES() {
    // 1. Alimente le capteur
    digitalWrite(capteur.alimPin, HIGH);

    // 2. Attend la stabilisation (50ms pour plus de fiabilité)
    vTaskDelay(pdMS_TO_TICKS(50));

    // 3. Lecture avec validation sur 2 échantillons
    static int lastReading = HIGH;
    int currentReading = digitalRead(capteur.pin);

    if (currentReading == LOW && lastReading == LOW) {
        capteur.valeur = 1;
        DEBUG_PRINTLN("pluie (confirmée)");
    } else if (currentReading == HIGH) {
        capteur.valeur = 0;
        DEBUG_PRINTLN("sec");
    }
    lastReading = currentReading;

    // 4. Coupe l'alimentation immédiatement
    digitalWrite(capteur.alimPin, LOW);
}

int getGOUTTES() {
    return capteur.valeur;
}
