#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include "SPI.h"
#include "variablesWEB.h"
#include "debug.h"

// --- SPIFSS : utiliser pour sauvegarder et utiliser les coefficients K1 à K7 sur la page web ---
void initSPIFFS() {
  if (!SPIFFS.begin()) {
    DEBUG_PRINTLN("initSPIFFS - Une erreur s'est produite lors de l'initialisation du SPIFFS");
  }
  DEBUG_PRINTLN("SPIFFS chargé avec succés");
}
void createConfigDir() {
  if (!SPIFFS.exists("/config")) {
    SPIFFS.mkdir("/config");
  }
}

void saveConstantsToFile() {
  File configFile = SPIFFS.open("/config/config.txt", "w");
  if (configFile) {
    configFile.println("K1=" + String(K1));
    configFile.println("K2=" + String(K2));
    configFile.println("K3=" + String(K3));
    configFile.println("K4=" + String(K4));
    configFile.println("K5=" + String(K5));
    configFile.println("K6=" + String(K6));
    configFile.println("K7=" + String(K7));
    configFile.println("temperature_ciel_clair=" + String(temperature_ciel_clair));
    configFile.println("temperature_ciel_couvert=" + String(temperature_ciel_couvert));
    configFile.close();
  } else {
    DEBUG_PRINTLN("Échec de l'ouverture du fichier de configuration pour l'écriture");
  }
}

void loadConstantsFromFile() {
  File configFile = SPIFFS.open("/config/config.txt", "r");
  if (configFile) {
    while (configFile.available()) {
      String line = configFile.readStringUntil('\n');
      int separator = line.indexOf('=');
      if (separator != -1) {
        String key = line.substring(0, separator);
        String value = line.substring(separator + 1);
        if (key == "K1") K1 = value.toFloat();
        else if (key == "K2") K2 = value.toFloat();
        else if (key == "K3") K3 = value.toFloat();
        else if (key == "K4") K4 = value.toFloat();
        else if (key == "K5") K5 = value.toFloat();
        else if (key == "K6") K6 = value.toFloat();
        else if (key == "K7") K7 = value.toFloat();
        else if (key == "temperature_ciel_clair") temperature_ciel_clair = value.toFloat();
        else if (key == "temperature_ciel_couvert") temperature_ciel_couvert = value.toFloat();
      }
    }
    configFile.close();
  } else {
    DEBUG_PRINTLN("Échec de l'ouverture du fichier de configuration pour la lecture");
  }
}

void listSPIFFSFiles(const char* dirname) {
  DEBUG_PRINTLN("Listing SPIFFS files:");
  File root = SPIFFS.open(dirname);
  if (!root) {
    DEBUG_PRINTLN("Échec de l'ouverture du répertoire principal");
    return;
  }
  if (!root.isDirectory()) {
    DEBUG_PRINTLN("Pas de répertoire trouvé");
    return;
  }
  File file = root.openNextFile();
  while (file) {
    DEBUG_PRINT("  FILE: ");
    DEBUG_PRINTLN(file.name());
    file = root.openNextFile();
  }
}

void handleConstantsPage(AsyncWebServerRequest *request) {
  String html = readFile(SPIFFS, "/index.html");
  html.replace("#k1", String(K1));
  html.replace("#k2", String(K2));
  html.replace("#k3", String(K3));
  html.replace("#k4", String(K4));
  html.replace("#k5", String(K5));
  html.replace("#k6", String(K6));
  html.replace("#k7", String(K7));
  html.replace("#temperature_ciel_clair", String(temperature_ciel_clair));
  html.replace("#temperature_ciel_couvert", String(temperature_ciel_couvert));
  request->send(200, "text/html", html);
}

String readFile(fs::FS &fs, const char *path) {
  File file = fs.open(path, "r");
  if (!file || file.isDirectory()) {
    return "";
  }

  String content = file.readString();
  file.close();
  return content;
}
