#ifndef WIFI_H
#define WIFI_H
#include <Arduino.h>

extern unsigned long lastWiFiCheck;
extern void WiFiEvent(WiFiEvent_t event);
extern void initWiFi();
extern void checkWiFi();
extern String getLocalIPAddress();

#endif
