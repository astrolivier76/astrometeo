#ifndef TSL2591_H
#define TSL2591_H
#include <Arduino.h>

bool isTSL2591Present();
bool calibrateTSL2591();
void updateTSL2591();
float getLux();
float getSQM();

#endif
