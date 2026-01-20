#include "MQTT.h"
#include "debug.h"
#include <ArduinoJson.h>

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
bool mqttEnabled = true;
unsigned long lastMqttReconnectAttempt = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 10000; // 10 secondes au lieu de 5

void initMQTT() {
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    DEBUG_PRINTLN("MQTT initialisé");
}

void reconnectMQTT() {
    if (!mqttEnabled) return;
    
    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - lastMqttReconnectAttempt > MQTT_RECONNECT_INTERVAL) {
            lastMqttReconnectAttempt = now;
            DEBUG_PRINT("Tentative de connexion MQTT...");
            
            if (mqttClient.connect("ESP32_AstroMeteo", MQTT_USER, MQTT_PASSWORD)) {
                DEBUG_PRINTLN("connecté");
                
                // Abonnements aux topics
                mqttClient.subscribe(MQTT_TOPIC_PREFIX "command");
                mqttClient.subscribe(MQTT_TOPIC_PREFIX "config/+");
                
            } else {
                DEBUG_PRINT("échec, rc=");
                DEBUG_PRINT(mqttClient.state());
                DEBUG_PRINTLN(" réessai dans 10 secondes");
            }
        }
    }
}

void publishSensorData(const SensorData& data) {
    if (!mqttClient.connected() || !mqttEnabled) return;

    DynamicJsonDocument doc(512);
    
    doc["temperature"] = data.temperature;
    doc["pression"] = data.pression;
    doc["humidity"] = data.humidity;
    doc["dewpoint"] = data.dewpoint;
    doc["sky_temperature"] = data.skyT;
    doc["cloud_cover"] = data.nuages;
    doc["safety_status"] = data.safe;
    doc["illuminance"] = data.lux;
    doc["sqm"] = data.sqm;
    doc["wind_speed"] = data.vent;
    doc["rainfall"] = data.pluie;
    doc["rain_detected"] = data.gouttes;
    doc["timestamp"] = data.lastUpdate;

    char buffer[512];
    size_t n = serializeJson(doc, buffer);
    
    if (mqttClient.publish(MQTT_TOPIC_PREFIX "sensors", buffer, n)) {
        DEBUG_PRINTLN("Données MQTT publiées");
    } else {
        DEBUG_PRINTLN("Échec publication MQTT");
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    char message[length + 1];
    memcpy(message, payload, length);
    message[length] = '\0';
    
    DEBUG_PRINTF("Message MQTT reçu [%s]: %s\n", topic, message);
    
    String topicStr = String(topic);
    
    if (topicStr.equals(MQTT_TOPIC_PREFIX "command")) {
        if (strcmp(message, "reboot") == 0) {
            ESP.restart();
        } else if (strcmp(message, "enable") == 0) {
            mqttEnabled = true;
        } else if (strcmp(message, "disable") == 0) {
            mqttEnabled = false;
        }
    }
}

void handleMQTT() {
    if (mqttEnabled) {
        reconnectMQTT();
        mqttClient.loop();
    }
}
