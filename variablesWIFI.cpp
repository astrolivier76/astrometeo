#include <Arduino.h>
#include "variablesWIFI.h"

const char* ssid = "xxx";           // Variable pour stocker le SSID du wifi - A modifier par l'utilisateur
const char* password = "xxx";       // Varibale pour stocker le mot de passe du WIFI - a modifier par l'utilisteur

IPAddress ip(192, 168, 0, 200);              // Variable pour attribuer une adresse fixe à l'ESP32. Evite que l'adresse ne change et fiabilise la connexion dans le temps - a modifier par l'utilisateur.
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
