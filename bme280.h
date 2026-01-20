#ifndef BME280_H
#define BME280_H

bool initBME(); 
void updateBME();
bool isBMEAvailable();

float getTemperature_BME();
float getHumidity_BME();
float getPressure_BME();
float getDewpoint_BME();
int needChauffage_BME280();

void sleepBME();
void wakeBME();
bool isBMESleeping();

#endif
