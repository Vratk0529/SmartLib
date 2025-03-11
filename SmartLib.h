#ifndef SMARTLIB_H
#define SMARTLIB_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

class SmartLib {
    public:
    SmartLib(const char *deviceName, const char* SSID, const char* PASS, const char* MQTT_SRV, \
         const char* MQTT_NAME, const char* MQTT_PASS, uint8_t ACT_LED = -1, bool ACT_HIGH = true);

    void maintainConnection();
    bool getStatus();

    void setAct(bool state);
    void toggleAct();

    void setMQTTCallback(MQTT_CALLBACK_SIGNATURE);

    void sendToMQTTStr(const char *topic, const char *payload);
    void sendToMQTT(const char *topic, const char *fmt, ...);

    void loop();

    char * getRxTopic(const char * topic);

    private:
    void setStringSafe(char* var, size_t size, const char* to);

    static WiFiClient _mqttClient;
    PubSubClient client;

    bool actStatus;

    char _SSID[64], _PASS[64], _MQTT_NAME[64], _MQTT_PASS[64];
    char _deviceName[64];
    uint8_t _ACT_LED;
    bool _ACT_HIGH;
    char _topic[128];
};

#endif