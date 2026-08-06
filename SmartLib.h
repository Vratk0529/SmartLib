#ifndef SMARTLIB_H
#define SMARTLIB_H

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#elif defined(ESP32)
#include <WiFi.h>
#else
#define WIFI_NONE
#endif

// Topic layout used throughout:
//   <deviceName>/TX/<subtopic>   device -> broker
//   <deviceName>/RX/<subtopic>   broker -> device (subscribed automatically)

#ifndef SMARTLIB_MAX_HANDLERS
#define SMARTLIB_MAX_HANDLERS 16
#endif

// Handler for a routed message. topic is the full MQTT topic as delivered.
typedef void (*SmartTopicHandler)(const char *topic, const String &payload);

// One SmartLib instance per sketch: the network clients and the MQTT
// callback plumbing are static.

class SmartLib
{
public:
#ifndef WIFI_NONE
     SmartLib(const char *deviceName, const char *SSID, const char *PASS, const char *MQTT_SRV,
              const char *MQTT_NAME, const char *MQTT_PASS, int8_t ACT_LED = -1, bool ACT_HIGH = true);
#endif
     SmartLib(const char *deviceName, uint8_t macAddress[6], int8_t ETH_CS, int8_t ETH_RST, const char *MQTT_SRV,
              const char *MQTT_NAME, const char *MQTT_PASS, int8_t ACT_LED = -1, bool ACT_HIGH = true);

     void begin();

     void maintainConnection();
     bool getStatus();

     void setAct(bool state);
     void toggleAct();

     void loop();

     // ---- receiving -------------------------------------------------
     // Route a subtopic of "<device>/RX/" to a handler. MQTT wildcards
     // work: '+' matches one level, '#' matches the rest.
     //   Smart.on("Relay/+", onRelay);   ->  <device>/RX/Relay/1
     // The filter string is not copied, so use a literal or a global.
     bool on(const char *subTopicFilter, SmartTopicHandler handler);

     // Same, but for an absolute topic anywhere on the broker (e.g. another
     // device's TX topic). Subscribed on every (re)connect.
     bool onRaw(const char *topicFilter, SmartTopicHandler handler);

     void clearHandlers();

     // Raw callback for everything that arrives, called before on()/onRaw()
     // routing. Optional -- on() alone is enough for most devices.
     void setMQTTCallback(void (*mqttCallback)(char *, uint8_t *, unsigned int));

     // Called every time the MQTT session comes up, after resubscribing.
     // The place to republish device state, since a reconnect means the
     // broker may have restarted.
     void setConnectedCallback(void (*callback)(void));

     // ---- sending ---------------------------------------------------
     void sendToMQTTStr(const char *topic, const char *payload, bool retain = false);
     void sendToMQTT(const char *topic, const char *fmt, ...);
     void sendToMQTTRetained(const char *topic, const char *fmt, ...);

     // Reply on the TX topic matching an RX topic that was just received,
     // i.e. "<device>/RX/Relay/1" -> "<device>/TX/Relay/1". Handy for
     // acknowledging a command with the resulting state.
     void replyTo(const char *rxTopic, const char *payload, bool retain = true);

     // uptime / heap / IP / link quality as JSON, for dashboards.
     void publishDiagnostics(const char *subTopic = "Diagnostics");

     // ---- availability (MQTT last will) ------------------------------
     // Enabled by default: "<device>/TX/Availability" is published as
     // "online" (retained) on connect, and the broker publishes "offline"
     // there if this device drops off. Call before begin() to change.
     void setAvailability(const char *subTopic, const char *onlinePayload = "online",
                          const char *offlinePayload = "offline");
     void disableAvailability();

     // ---- helpers ---------------------------------------------------
     char *getRxTopic(const char *topic);

     // Inverse of getRxTopic: given a topic as delivered to a callback,
     // returns the part after "<device>/RX/", or nullptr if the topic is
     // not addressed to this device. Points into the caller's buffer.
     const char *rxSubTopic(const char *fullTopic) const;

     const char *deviceName() const { return _deviceName; }
     IPAddress getIP() const;
     // WiFi RSSI in dBm, or 0 on Ethernet.
     int32_t getRSSI() const;

     // "1", "on", "true", "yes", "open" (any case) -> true.
     static bool isOn(const String &payload);
     // Non-blocking interval: `static uint32_t t; if (SmartLib::every(t, 1000))`
     static bool every(uint32_t &last, uint32_t period);
     // MQTT-style filter match, exposed because it is useful on its own.
     static bool topicMatches(const char *filter, const char *topic);

private:
     void setStringSafe(char *var, size_t size, const char *to);
     void commonInit(const char *deviceName, const char *MQTT_NAME, const char *MQTT_PASS, int8_t ACT_LED,
                     bool ACT_HIGH);
     void onConnected();
     void dispatch(char *topic, uint8_t *payload, unsigned int length);

     static void mqttCallback(char *topic, uint8_t *payload, unsigned int length);
     static void (*_mqttCallback)(char *topic, uint8_t *payload, unsigned int length);
     static SmartLib *_instance;

#ifndef WIFI_NONE
     static WiFiClient _wifiClient;
#endif
     static EthernetClient _ethClient;
     PubSubClient client;

     struct Handler
     {
          const char *filter;
          SmartTopicHandler handler;
          bool raw;
     };
     Handler _handlers[SMARTLIB_MAX_HANDLERS] = {};
     uint8_t _handlerCount = 0;
     void (*_connectedCallback)(void) = nullptr;

     bool actStatus;
     char _SSID[64], _PASS[64], _MQTT_NAME[64], _MQTT_PASS[64];
     char _deviceName[64];
     int8_t _ACT_LED = -1;
     int8_t _ETH_CS = -1;
     int8_t _ETH_RST = -1;
     bool _ACT_HIGH;
     char _topic[128];
     uint8_t _reconnectionTries = 0;
     uint32_t _lastWeakWiFiLog = 0;
     uint32_t _lastMqttAttempt = 0;
     uint32_t _lastDhcpAttempt = 0;

     bool _availEnabled = true;
     char _availTopic[128] = {0};
     char _availOnline[32] = {0};
     char _availOffline[32] = {0};

     uint8_t _macAddress[6] = {0};
     bool ethOrWiFi = true; // false if Ethernet, true if WiFi
};

#endif
