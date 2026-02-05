#include "ota.h"
#include "debug.h"

// --- CODE NECESSAIRE POUR LA MISE A JOUR DE L'ESP32 PAR OTA ---
OTAHandler::OTAHandler() {
    _ssid = ssid;
    _password = password;
    logs = "";
    // Réserve de la mémoire dès le début pour éviter la fragmentation
    // Cela crée un bloc contigu en RAM une fois pour toutes
    logs.reserve(2100); 
}

void OTAHandler::cleanLogs() {
    logs = "";
    DEBUG_PRINTLN("Logs OTA nettoyés.");
}

void OTAHandler::begin(AsyncWebServer &server) {
    if(!SPIFFS.begin(true)){
        DEBUG_PRINTLN("OTAHandler - Erreur d'initialisation SPIFFS");
        return;
    }
    
    server.on("/OTA", HTTP_GET, [&](AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/OTA.html");
    });

    server.on("/update", HTTP_POST, [&](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", 
            (Update.hasError()) ? "Update failed" : "Update Success");
        response->addHeader("Connection", "close");
        request->send(response);
    }, [&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        handleFileUpload(request, filename, index, data, len, final);
    });

    server.on("/logs", HTTP_GET, [&](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", logs);
    });
}

void OTAHandler::addToLogs(const String& message) {
    // Protection mémoire simple
    if (logs.length() + message.length() + 2 > 2000) {
        // Si on dépasse, on coupe drastiquement la première moitié pour éviter
        // de faire des substring() trop fréquents (lourds pour le CPU/RAM)
        logs = logs.substring(logs.length() / 2);
    }
    logs += message + "\n";
}

void OTAHandler::handleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    // Pas de static ici, risque de problèmes si plusieurs upload échouent/recommencent
    
    if (index == 0) {
        String msg = "Update Start: " + filename;
        DEBUG_PRINTLN(msg);
        addToLogs(msg);
        
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
            addToLogs("Update begin failed");
        }
    }

    if (!Update.hasError()) {
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
            addToLogs("Write failed");
        } else {
            // Optimisation : On ne log pas CHAQUE paquet, sinon la RAM explose et le WiFi sature
            // On log seulement tous les 50 Ko environ
            static size_t lastLogSize = 0;
            if (index - lastLogSize > 50000) {
                 String msg = "Written " + String(index + len) + " bytes...";
                 DEBUG_PRINTLN(msg);
                 addToLogs(msg);
                 lastLogSize = index;
            }
        }
    }

    if (final) {
        if (Update.end(true)) {
            String msg = "Update Success: " + String(index + len) + "B";
            DEBUG_PRINTLN(msg);
            addToLogs(msg);
            request->send(200, "text/plain", "Update Success. Rebooting...");
            // Le delay est acceptable ici car on va rebooter de toute façon
            delay(1000);
            ESP.restart();
        } else {
            Update.printError(Serial);
            addToLogs("Update end failed");
            request->send(500, "text/plain", "Update failed");
        }
    }
}
