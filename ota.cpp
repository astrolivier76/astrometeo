#include "ota.h"
#include "debug.h"

// --- CODE NECESSAIRE POUR LA MISE A JOUR DE L'ESP32 PAR OTA ---
OTAHandler::OTAHandler() {
    _ssid = ssid;
    _password = password;
    logs = "";
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
    logs += message + "\n";
    if (logs.length() > 2000) {
        logs = logs.substring(logs.length() - 2000);
    }
}

void OTAHandler::handleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    static UpdateClass updater;
    
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
            String msg = "Written " + String(len) + " bytes";
            DEBUG_PRINTLN(msg);
            addToLogs(msg);
        }
    }

    if (final) {
        if (Update.end(true)) {
            String msg = "Update Success: " + String(index + len) + "B";
            DEBUG_PRINTLN(msg);
            addToLogs(msg);
            request->send(200, "text/plain", "Update Success. Rebooting...");
            delay(1000);
            ESP.restart();
        } else {
            Update.printError(Serial);
            addToLogs("Update end failed");
            request->send(500, "text/plain", "Update failed");
        }
    }
}
