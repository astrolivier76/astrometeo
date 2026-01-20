#ifndef CONNECT_H
#define CONNECT_H
#include <WiFi.h>

extern const char* ssid ;           // Variable pour stocker le SSID du wifi - A modifier par l'utilisateur
extern const char* password ;       // Varibale pour stocker le mot de passe du WIFI - a modifier par l'utilisteur

extern IPAddress ip;
extern IPAddress gateway;
extern IPAddress subnet;

#endif
