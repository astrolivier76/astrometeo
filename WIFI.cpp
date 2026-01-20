#include <Arduino.h>
#include <WiFi.h>
#include "variablesWIFI.h"
#include "debug.h"
#include <esp_wifi.h>

// --- PARAMETRES WIFI ---
unsigned long previousMillis = 0;
const long interval = 10000;
const long checkInterval = 60000;
unsigned long lastCheckMillis = 0;
int reconnectAttempts = 0;
const int maxReconnectAttempts = 5;
unsigned long lastWiFiCheck = 0;
const long wifiCheckInterval = 60000; // 60 secondes au lieu de 30

void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case SYSTEM_EVENT_STA_GOT_IP:
            DEBUG_PRINTLN("Connecté au WiFi");
            DEBUG_PRINT("address IP: ");
            DEBUG_PRINTLN(WiFi.localIP());
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            DEBUG_PRINTLN("Connexion au WIFI perdue, tentative de reconnection...");
            WiFi.begin(ssid, password);
            break;
    }
}

void initWiFi(void) {
    WiFi.mode(WIFI_STA);
    WiFi.onEvent(WiFiEvent);
    
    // Configuration IP fixe
    if (!WiFi.config(ip, gateway, subnet)) {
        DEBUG_PRINTLN("Échec de la configuration de l'adresse IP fixe");
    }
    
    WiFi.begin(ssid, password);
    DEBUG_PRINT("Connection au WiFi ..");
    
    previousMillis = millis();
    while (WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINT('.');
        delay(500);
        if (millis() - previousMillis >= interval) {
            DEBUG_PRINTLN("Échec de la connexion au WiFi, redémarrage...");
            ESP.restart();
        }
    }
    
    // Configuration agressive de l'économie d'énergie WiFi
    WiFi.setSleep(true);
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    
    DEBUG_PRINTLN("Connecté au WiFi");
    DEBUG_PRINT("Adresse IP: ");
    DEBUG_PRINTLN(WiFi.localIP());
}

void checkWiFi() {
    unsigned long currentMillis = millis();
    if (currentMillis - lastWiFiCheck >= wifiCheckInterval) {
        lastWiFiCheck = currentMillis;
        
        if (WiFi.status() != WL_CONNECTED) {
            DEBUG_PRINTLN("WiFi déconnecté, tentative de reconnexion...");
            WiFi.disconnect(false);
            delay(100);
            WiFi.begin(ssid, password);
            
            unsigned long reconnectStart = millis();
            reconnectAttempts++;
            
            while (WiFi.status() != WL_CONNECTED) {
                delay(500);
                if (millis() - reconnectStart >= interval || reconnectAttempts >= maxReconnectAttempts) {
                    DEBUG_PRINTLN("Échec de reconnexion. Redémarrage...");
                    ESP.restart();
                }
            }
            reconnectAttempts = 0;
            DEBUG_PRINTLN("WiFi reconnecté avec succès");
        }
    }
}

String getLocalIPAddress() {
    char jsonBuffer[50];
    snprintf(jsonBuffer, sizeof(jsonBuffer), "{\"ip\":\"%s\"}", WiFi.localIP().toString().c_str());
    return String(jsonBuffer);
}
