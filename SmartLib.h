#ifndef SMARTLIB_H
#define SMARTLIB_H

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif

class SmartLib {
    public:
    SmartLib(const char *deviceName, const char* SSID, const char* PASS, const char* MQTT_SRV, \
         const char* MQTT_NAME, const char* MQTT_PASS, int8_t ACT_LED = -1, bool ACT_HIGH = true);
    SmartLib(const char *deviceName, uint8_t macAddress[6], int8_t ETH_CS, int8_t ETH_RST, const char* MQTT_SRV,\
         const char* MQTT_NAME, const char* MQTT_PASS, int8_t ACT_LED = -1, bool ACT_HIGH = true);

    void begin();

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

    static WiFiClient _wifiClient;
    static EthernetClient _ethClient;
    PubSubClient client;

    bool actStatus;

    char _SSID[64], _PASS[64], _MQTT_NAME[64], _MQTT_PASS[64];
    char _deviceName[64];
    int8_t _ACT_LED = -1;
    int8_t _ETH_CS = -1;
    int8_t _ETH_RST = -1;
    bool _ACT_HIGH;
    char _topic[128];
    uint8_t _reconnectionTries = 0;

    uint8_t _macAddress[6] = {0};
    bool ethOrWiFi = true; // false if Ethernet, true if WiFi
};

#endif