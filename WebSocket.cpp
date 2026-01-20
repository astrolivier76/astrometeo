#include "WebSocket.h"
#include "debug.h"
#include <ArduinoJson.h>
#include "variablesWEB.h"  // Ajoutez cette inclusion

// Déclarations externes (maintenant dans variablesWEB.h)
// extern SemaphoreHandle_t xSensorDataMutex;
// extern SensorData cachedData;
// extern void formatWebSocketData(const SensorData& data, char* buffer, size_t bufferSize);

AsyncWebSocket ws("/ws");
bool websocketEnabled = true;

void initWebSocket() {
    ws.onEvent(onWebSocketEvent);
    DEBUG_PRINTLN("WebSocket initialisé");
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            DEBUG_PRINTF("Client WebSocket connecté #%u from %s\n", 
                        client->id(), client->remoteIP().toString().c_str());
            // Envoyer les données immédiatement à la connexion
            sendCurrentDataToClient(client);
            break;
            
        case WS_EVT_DISCONNECT:
            DEBUG_PRINTF("Client WebSocket déconnecté #%u\n", client->id());
            break;
            
        case WS_EVT_DATA:
            handleWebSocketMessage(client, data, len);
            break;
            
        case WS_EVT_PONG:
            DEBUG_PRINTLN("WebSocket pong reçu");
            break;
            
        case WS_EVT_ERROR:
            DEBUG_PRINTF("WebSocket error: %s\n", (char*)data);
            break;
    }
}

void handleWebSocketMessage(AsyncWebSocketClient *client, uint8_t *data, size_t len) {
    String message = String((char*)data).substring(0, len);
    DEBUG_PRINTF("Message WebSocket reçu: %s\n", message.c_str());
    
    if (message == "get_readings" || message == "refresh") {
        // Envoyer les données immédiatement
        sendCurrentDataToClient(client);
    }
    else if (message == "ping") {
        // Répondre au ping
        client->text("{\"type\":\"pong\"}");
    }
    else {
        DEBUG_PRINTF("Message WebSocket non reconnu: %s\n", message.c_str());
    }
}

void sendCurrentDataToClient(AsyncWebSocketClient *client) {
    if (xSemaphoreTake(xSensorDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        char wsBuffer[512];
        formatWebSocketData(cachedData, wsBuffer, sizeof(wsBuffer));
        String jsonData = "{\"type\":\"sensor_update\",\"data\":" + String(wsBuffer) + "}";
        client->text(jsonData);
        xSemaphoreGive(xSensorDataMutex);
        DEBUG_PRINTLN("Données envoyées au client WebSocket");
    } else {
        DEBUG_PRINTLN("Impossible d'acquérir le mutex pour WebSocket");
        client->text("{\"type\":\"error\",\"message\":\"Unable to acquire sensor data\"}");
    }
}

void notifyClients(const String& data) {
    if (websocketEnabled && ws.count() > 0) {
        ws.textAll(data);
        DEBUG_PRINTF("Données broadcastées à %d clients\n", ws.count());
    }
}

void handleWebSocket() {
    // Nettoyer les clients déconnectés
    ws.cleanupClients();
}

// Fonction pour obtenir le nombre de clients connectés
uint32_t getWebSocketClientCount() {
    return ws.count();
}

// Fonction pour désactiver/réactiver WebSocket
void setWebSocketEnabled(bool enabled) {
    websocketEnabled = enabled;
    if (!enabled) {
        ws.closeAll();
        DEBUG_PRINTLN("WebSocket désactivé");
    } else {
        DEBUG_PRINTLN("WebSocket activé");
    }
}

bool isWebSocketEnabled() {
    return websocketEnabled;
}
