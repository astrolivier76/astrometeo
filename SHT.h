#ifndef SHT_H
#define SHT_H

bool initSHT();
void updateSHT();

bool isSHTAvailable();
float getTemperature_SHT();
float getHumidity_SHT();
float getDewpoint_SHT();

void sleepSHT();
void wakeSHT();
bool isSHTSleeping();

#endif
