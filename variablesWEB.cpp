#include "variablesWEB.h"

// --- Initialisation des variables pour le calcul de la température du ciel, varibales modifiables depuis l'interface web ---
float K1 = 33.0;
float K2 = 0.0;
float K3 = 4.0;
float K4 = 100.0;
float K5 = 100.0;
float K6 = 0.0;
float K7 = 0.0;

float temperature_ciel_clair = -8;
float temperature_ciel_couvert = 0;

unsigned long lastTime = 0;
unsigned long timerDelay = 10000;

// Initialisation de la structure de cache
SensorData cachedData = {
    -999, -999, -999, -999, -999, -999, -999, -999, -999, -999, -999, -999, 0
};
