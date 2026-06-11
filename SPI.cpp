#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include "SPI.h"
#include "variablesWEB.h"
#include "debug.h"

// --- VARIABLES EXTERNES ---
extern float luxThresholdChauffage; 
extern int capteur_ambiant;
extern float correction_temperature_ambiante;
extern float correction_temperature_bme280;
extern float correction_altitude_pression;
extern float correction_temperature_sht;

void initSPIFFS() {
  if (!SPIFFS.begin(true)) {
    DEBUG_PRINTLN("initSPIFFS - Erreur d'initialisation SPIFFS");
  } else {
    DEBUG_PRINTLN("SPIFFS chargé avec succés");
  }
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
    configFile.println("luxThresholdChauffage=" + String(luxThresholdChauffage));
    
    // NOUVEAU : Sauvegarde du capteur choisi et des offsets
    configFile.println("capteur_ambiant=" + String(capteur_ambiant));
    configFile.println("corr_temp=" + String(correction_temperature_ambiante));
    configFile.println("corr_press=" + String(correction_altitude_pression));
    
    configFile.close();
    DEBUG_PRINTLN("Configuration sauvegardée");
  } else {
    DEBUG_PRINTLN("Échec de sauvegarde");
  }
}

void loadConstantsFromFile() {
  if (!SPIFFS.exists("/config/config.txt")) {
      DEBUG_PRINTLN("Fichier config inexistant, utilisation par défaut");
      return;
  }

  File configFile = SPIFFS.open("/config/config.txt", "r");
  if (configFile) {
    while (configFile.available()) {
      String line = configFile.readStringUntil('\n');
      line.trim(); 
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
        else if (key == "luxThresholdChauffage") luxThresholdChauffage = value.toFloat();
        
        // NOUVEAU : Chargement dynamique
        else if (key == "capteur_ambiant") capteur_ambiant = value.toInt();
        else if (key == "corr_press") correction_altitude_pression = value.toFloat();
        else if (key == "corr_temp") {
            correction_temperature_ambiante = value.toFloat();
            // On propage l'offset unique aux deux librairies automatiquement
            correction_temperature_bme280 = correction_temperature_ambiante;
            correction_temperature_sht = correction_temperature_ambiante;
        }
      }
    }
    configFile.close();
    DEBUG_PRINTLN("Configuration chargée depuis SPIFFS");
  } else {
    DEBUG_PRINTLN("Échec lecture");
  }
}

void listSPIFFSFiles(const char* dirname) {
  File root = SPIFFS.open(dirname);
  if (!root || !root.isDirectory()) return;
  File file = root.openNextFile();
  while (file) {
    DEBUG_PRINTLN(file.name());
    file = root.openNextFile();
  }
}

void handleConstantsPage(AsyncWebServerRequest *request) {
  if (SPIFFS.exists("/index.html")) request->send(SPIFFS, "/index.html", "text/html");
  else request->send(404, "text/plain", "Index.html introuvable dans SPIFFS");
}

String readFile(fs::FS &fs, const char *path) {
  File file = fs.open(path, "r");
  if (!file || file.isDirectory()) return "";
  String content = file.readString();
  file.close();
  return content;
}
