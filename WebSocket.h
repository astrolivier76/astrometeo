#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

extern AsyncWebSocket ws;
extern bool websocketEnabled;

// Déclarations des fonctions
void initWebSocket();
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                     AwsEventType type, void *arg, uint8_t *data, size_t len);
void handleWebSocketMessage(AsyncWebSocketClient *client, uint8_t *data, size_t len);
void sendCurrentDataToClient(AsyncWebSocketClient *client);
void notifyClients(const String& data);
void handleWebSocket();
uint32_t getWebSocketClientCount();
void setWebSocketEnabled(bool enabled);
bool isWebSocketEnabled();

#endif
