#ifndef SMARTLIB_H
#define SMARTLIB_H

#include <Arduino.h>
#include <SPI.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
// The ESP8266 WiFi headers define these too (4 and 8192). Ethernet.h must
// see only its own values, the ones the Ethernet library is compiled with:
// its inline constructors mark a free socket with MAX_SOCK_NUM, and a 4
// where the library expects 8 reads as "socket 4 in use".
#undef MAX_SOCK_NUM
#undef UDP_TX_PACKET_MAX_SIZE
#elif defined(ESP32)
#include <WiFi.h>
#include <WiFiUdp.h>
#else
#define WIFI_NONE
#endif

#include <Ethernet.h>
#include <PubSubClient.h>

// OTA updates and NTP time need the ESP cores (Update, lwIP SNTP, settimeofday).
#if defined(ESP32) || defined(ESP8266)
#define SMARTLIB_ESP
#endif

#ifndef ETHERNET_FORK_VRATK0529
#warning "SmartLib: upstream Ethernet library found, use https://github.com/Vratk0529/Ethernet (hangs on cable pull, no DHCP hostname/NTP)"
#endif

// Topic layout used throughout:
//   <deviceName>/TX/<subtopic>   device -> broker
//   <deviceName>/RX/<subtopic>   broker -> device (subscribed automatically)

#ifndef SMARTLIB_MAX_HANDLERS
#define SMARTLIB_MAX_HANDLERS 16
#endif

// POSIX TZ string for local time; time(nullptr) is UTC regardless.
#ifndef SMARTLIB_DEFAULT_TZ
#define SMARTLIB_DEFAULT_TZ "CET-1CEST,M3.5.0,M10.5.0/3" // Europe/Prague
#endif

// Used when DHCP does not offer an NTP server (option 42).
#ifndef SMARTLIB_DEFAULT_NTP
#define SMARTLIB_DEFAULT_NTP "pool.ntp.org"
#endif

// espota's default port for the platform, so `upload_protocol = espota`
// works without --port.
#ifndef SMARTLIB_OTA_PORT
#if defined(ESP8266)
#define SMARTLIB_OTA_PORT 8266
#else
#define SMARTLIB_OTA_PORT 3232
#endif
#endif

// tools/smartscan.py finds devices by asking this port.
#ifndef SMARTLIB_DISCOVERY_PORT
#define SMARTLIB_DISCOVERY_PORT 3234
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

     // ---- identity ----------------------------------------------------
     // Network name: DHCP hostname (router lease list), mDNS "<name>.local"
     // and the MQTT client id. Defaults to the device name with anything
     // but letters, digits and '-' dropped. Call before begin().
     void setHostname(const char *hostname);
     const char *hostname() const { return _hostname; }

     // Shown in the Info topic. Not copied: pass a literal or a global.
     void setFirmwareVersion(const char *version) { _fwVersion = version; }

     // Retained "<device>/TX/Info": hostname, IP, MAC, chip, firmware
     // version and MD5, reset reason, NTP server. Sent on every connect.
     void publishInfo(const char *subTopic = "Info");

     // Blink the activity LED to find the device physically. Also on
     // MQTT: "<device>/RX/Identify" with the seconds as payload (empty
     // = 10, "0"/"off" = stop).
     void identify(uint16_t seconds = 10);

     // Answer tools/smartscan.py (UDP SMARTLIB_DISCOVERY_PORT) with the
     // Info JSON plus uptime/RSSI/MQTT state. On by default.
     void disableDiscovery() { _discoveryEnabled = false; }

#ifdef SMARTLIB_ESP
     // ---- OTA ---------------------------------------------------------
     // On by default, and it needs a password: set one here or with
     // -DSMARTLIB_OTA_PASSWORD=\"...\", otherwise OTA stays off. Upload
     // with `upload_protocol = espota`, `upload_port = <hostname>.local`
     // (or the IP on Ethernet), `upload_flags = --auth=<password>`.
     void setOTAPassword(const char *password);
     // Testing only: accept updates from anyone on the network.
     void allowOTAWithoutPassword() { _otaNoPassword = true; }
     void disableOTA() { _otaEnabled = false; }
     // Runs right before an update starts writing flash (relays off,
     // stop tasks...). The MQTT session is closed after it.
     void setOTAStartCallback(void (*callback)(void)) { _otaStartCallback = callback; }
     bool otaActive() const { return _otaActive; }

     // ---- time --------------------------------------------------------
     // NTP starts once the network is up, from the server DHCP offers or
     // else SMARTLIB_DEFAULT_NTP. Call these before begin().
     void setTimezone(const char *posixTz);
     void setNTPServer(const char *server);
     void disableNTP() { _ntpEnabled = false; }

     // Wall clock is set (NTP or kept across a soft reset).
     static bool timeValid();
     // Local time with strftime(); false (and "") until timeValid().
     static bool timeString(char *buf, size_t size, const char *fmt = "%Y-%m-%dT%H:%M:%S%z");
     // Server in use: DHCP-provided IP or the fallback name.
     const char *ntpServer();
#endif

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
     void macAddress(uint8_t mac[6]) const;
     bool networkUp();
     void startNetworkServices();
     void serviceIdentify();
     void serviceDiscovery();
     size_t infoJson(char *buf, size_t size, bool live);

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

     bool actStatus = false;
     char _SSID[64], _PASS[64], _MQTT_NAME[64], _MQTT_PASS[64];
     char _deviceName[64];
     char _hostname[33] = {0};
     const char *_fwVersion = nullptr;
     int8_t _ACT_LED = -1;
     int8_t _ETH_CS = -1;
     int8_t _ETH_RST = -1;
     bool _ACT_HIGH = true;
     char _topic[128];
     uint8_t _reconnectionTries = 0;
     uint32_t _lastWeakWiFiLog = 0;
     uint32_t _lastMqttAttempt = 0;
     uint32_t _lastDhcpAttempt = 0;

     uint32_t _identifyStart = 0;
     uint32_t _identifyLength = 0;
     uint32_t _identifyLastToggle = 0;

     bool _availEnabled = true;
     char _availTopic[128] = {0};
     char _availOnline[32] = {0};
     char _availOffline[32] = {0};

     uint8_t _macAddress[6] = {0};
     bool ethOrWiFi = true; // false if Ethernet, true if WiFi

     bool _netServicesStarted = false;
     bool _discoveryEnabled = true;
     UDP *_discovery = nullptr;
#ifndef WIFI_NONE
     WiFiUDP _wifiDiscoveryUdp;
#endif
     EthernetUDP _ethDiscoveryUdp;

#ifdef SMARTLIB_ESP
     void startOTA();
     void otaStarting();
     void startTime();
     void serviceTime();

     bool _otaEnabled = true;
     bool _otaNoPassword = false;
     bool _otaRunning = false;
     bool _otaActive = false;
     char _otaPassword[64] = {0};
     void (*_otaStartCallback)(void) = nullptr;

     // Ethernet has no lwIP, so neither ArduinoOTA nor SNTP work there:
     // SmartLib speaks espota and NTP itself over the W5x00 sockets.
     void ethOtaHandle();
     void ethOtaRun(IPAddress host, uint16_t port, uint32_t size, int command, const char *md5);
     EthernetUDP _ethOtaUdp;
     enum : uint8_t { ETH_OTA_IDLE, ETH_OTA_WAITAUTH } _ethOtaState = ETH_OTA_IDLE;
     char _ethOtaNonce[33] = {0};
     char _ethOtaMd5[33] = {0};
     uint32_t _ethOtaSize = 0;
     uint16_t _ethOtaPort = 0;
     int _ethOtaCommand = 0;
     uint32_t _ethOtaAuthStart = 0;

     void ethNtpSend();
     void ethNtpReceive();
     EthernetUDP _ethNtpUdp;
     bool _ethNtpWaiting = false;
     bool _ethNtpSynced = false;
     uint32_t _ethNtpSentAt = 0;
     uint32_t _ethNtpLastAttempt = 0;
     uint8_t _ethNtpFailures = 0;

     bool _ntpEnabled = true;
     bool _ntpConfigured = false;
     char _tz[64] = SMARTLIB_DEFAULT_TZ;
     char _ntpFallback[64] = SMARTLIB_DEFAULT_NTP;
     char _ntpDhcp[16] = {0};
     const char *_ntpActive = "";
#endif
};

#endif
