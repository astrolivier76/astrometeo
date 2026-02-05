#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "SPIFFS.h"
#include "SPI.h"
#include "variablesWEB.h"
#include "debug.h"

// --- SPIFSS : utiliser pour sauvegarder et utiliser les coefficients K1 à K7 ---
void initSPIFFS() {
  if (!SPIFFS.begin(true)) { // true = format if failed (plus sûr)
    DEBUG_PRINTLN("initSPIFFS - Une erreur s'est produite lors de l'initialisation du SPIFFS");
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
    configFile.close();
    DEBUG_PRINTLN("Constantes sauvegardées");
  } else {
    DEBUG_PRINTLN("Échec de l'ouverture du fichier de configuration pour l'écriture");
  }
}

void loadConstantsFromFile() {
  if (!SPIFFS.exists("/config/config.txt")) {
      DEBUG_PRINTLN("Fichier config inexistant, utilisation des valeurs par défaut");
      return;
  }

  File configFile = SPIFFS.open("/config/config.txt", "r");
  if (configFile) {
    while (configFile.available()) {
      String line = configFile.readStringUntil('\n');
      line.trim(); // Enlever les espaces et sauts de ligne
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
    DEBUG_PRINTLN("Constantes chargées depuis SPIFFS");
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

// --- MODIFICATION CRITIQUE ICI ---
// On ne charge plus le HTML en RAM pour faire des replace().
// On envoie directement le fichier (Streaming) = 0 usage RAM.
void handleConstantsPage(AsyncWebServerRequest *request) {
  if (SPIFFS.exists("/index.html")) {
    request->send(SPIFFS, "/index.html", "text/html");
  } else {
    request->send(404, "text/plain", "Index.html introuvable dans SPIFFS");
  }
}

// Fonction utilitaire conservée si besoin ailleurs, mais plus utilisée pour l'index
String readFile(fs::FS &fs, const char *path) {
  File file = fs.open(path, "r");
  if (!file || file.isDirectory()) {
    return "";
  }
  String content = file.readString();
  file.close();
  return content;
}
