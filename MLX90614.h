#ifndef MLX90614_H
#define MLX90614_H

float Sign(float x);

bool initMLX(); 
void updateMLX();
bool isMLXAvailable();

float getTemperature_Sky();
float getNuages();
int getSafeNuages();

void sleepMLX();
void wakeMLX();
bool isMLXSleeping();

#endif
