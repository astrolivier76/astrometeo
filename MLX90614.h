#ifndef MLX90614_H
#define MLX90614_H

float Sign(float x);

bool initMLX(); 
// On ajoute un argument par défaut pour la température externe
void updateMLX(float tempAmbianteExterne = -999.0);
bool isMLXAvailable();

float getTemperature_Ambiante_MLX(); // Nouvelle fonction
float getTemperature_Sky();
float getNuages();
int getSafeNuages();

void sleepMLX();
void wakeMLX();
bool isMLXSleeping();

#endif
