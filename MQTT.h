#ifndef MQTT_H
#define MQTT_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include "variablesWEB.h"

// Configuration MQTT - À adapter selon votre broker
#define MQTT_SERVER ""  // Adresse du broker MQTT
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASSWORD ""
#define MQTT_TOPIC_PREFIX "astrometeo/"

extern PubSubClient mqttClient;
extern bool mqttEnabled;

void initMQTT();
void reconnectMQTT();
void publishSensorData(const SensorData& data);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void handleMQTT();

#endif
