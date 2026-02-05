#include <Arduino.h>
#include <WiFi.h>
#include "variablesWIFI.h"
#include "debug.h"
#include <esp_wifi.h>

// --- PARAMETRES WIFI ---
unsigned long previousMillis = 0;
const long interval = 10000;
// const long checkInterval = 60000; // Pas utilisé
// unsigned long lastCheckMillis = 0; // Pas utilisé
int reconnectAttempts = 0;
const int maxReconnectAttempts = 20; // Augmenté car la vérification est plus rapide
unsigned long lastWiFiCheck = 0;
const long wifiCheckInterval = 30000; // Vérif toutes les 30s

// Variables pour la gestion non-bloquante
bool isReconnecting = false;
unsigned long reconnectStartTime = 0;

void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case SYSTEM_EVENT_STA_GOT_IP:
            DEBUG_PRINTLN("[WiFi] Connecté !");
            DEBUG_PRINT("Adresse IP: ");
            DEBUG_PRINTLN(WiFi.localIP());
            isReconnecting = false;
            reconnectAttempts = 0;
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            DEBUG_PRINTLN("[WiFi] Déconnecté.");
            // Ne pas bloquer ici avec WiFi.begin, on laisse checkWiFi gérer
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
    DEBUG_PRINT("Connexion au WiFi en cours");
    
    // Au démarrage, on peut se permettre d'attendre un peu (mais pas indéfiniment)
    previousMillis = millis();
    while (WiFi.status() != WL_CONNECTED) {
        DEBUG_PRINT('.');
        delay(500);
        if (millis() - previousMillis >= 10000) { // 10 secondes max au boot
            DEBUG_PRINTLN("\n[WiFi] Pas de connexion au démarrage, continuation en mode autonome.");
            break; 
        }
    }
    
    // Configuration agressive de l'économie d'énergie WiFi
    WiFi.setSleep(true);
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    
    if(WiFi.status() == WL_CONNECTED){
        DEBUG_PRINTLN("\n[WiFi] Connecté au démarrage");
    }
}

// VERSION NON-BLOQUANTE ET SÉCURISÉE
void checkWiFi() {
    unsigned long currentMillis = millis();

    // Si on est déjà connecté, on ne fait rien sauf vérifier périodiquement
    if (WiFi.status() == WL_CONNECTED) {
        isReconnecting = false;
        reconnectAttempts = 0;
        return;
    }

    // Si on n'est pas connecté, on gère la reconnexion sans bloquer le loop()
    if (currentMillis - lastWiFiCheck >= wifiCheckInterval) {
        lastWiFiCheck = currentMillis;
        
        DEBUG_PRINTLN("[WiFi] Tentative de reconnexion (Non-bloquant)...");
        
        // On ne déconnecte pas explicitement pour éviter de casser une négo en cours
        WiFi.reconnect();
        
        reconnectAttempts++;
        if (reconnectAttempts > maxReconnectAttempts) {
             DEBUG_PRINTLN("[WiFi] Trop d'échecs. Redémarrage du module pour forcer le hardware.");
             // On redémarre l'ESP seulement si vraiment nécessaire après de nombreux échecs
             // Car un reboot ferme l'observatoire temporairement (perte de contrôle)
             ESP.restart(); 
        }
    }
}

String getLocalIPAddress() {
    // Optimisation : éviter snprintf pour une simple IP
    return "{\"ip\":\"" + WiFi.localIP().toString() + "\"}";
}
