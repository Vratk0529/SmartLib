#include "SmartLib.h"

#include "esp_log_compat.h"

#include <stdarg.h>

#if defined(ESP32)
#include <ArduinoOTA.h>
#include <MD5Builder.h>
#include <Update.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <lwip/ip_addr.h>
#include <sys/time.h>
#elif defined(ESP8266)
#include <ArduinoOTA.h>
#include <MD5Builder.h>
#include <Updater.h>
#include <coredecls.h>
#include <lwip/apps/sntp.h>
#include <lwip/ip_addr.h>
#include <sys/time.h>
#endif

#ifndef WIFI_NONE
WiFiClient SmartLib::_wifiClient;
#endif
EthernetClient SmartLib::_ethClient;

void (*SmartLib::_mqttCallback)(char *, uint8_t *, unsigned int) = nullptr;
SmartLib *SmartLib::_instance = nullptr;

#ifdef SMARTLIB_ESP
// Set from the SNTP/settimeofday callbacks (lwIP task on ESP32), reported
// from loop().
static volatile bool s_timeSynced = false;
static bool s_timeSyncLogged = false;

static const uint32_t NTP_EPOCH_OFFSET = 2208988800UL; // 1900 -> 1970
static const uint16_t NTP_LOCAL_PORT = 4123;
static const uint32_t NTP_RETRY_MS = 15000;
static const uint32_t NTP_RESYNC_MS = 3600000UL;
#endif

void SmartLib::commonInit(const char *deviceName, const char *MQTT_NAME, const char *MQTT_PASS, int8_t ACT_LED,
                          bool ACT_HIGH)
{
    _instance = this;

    setStringSafe(_deviceName, sizeof(_deviceName), deviceName);
    setStringSafe(_MQTT_NAME, sizeof(_MQTT_NAME), MQTT_NAME);
    setStringSafe(_MQTT_PASS, sizeof(_MQTT_PASS), MQTT_PASS);
    setHostname(deviceName);

    // The pin itself is set up in begin(): this runs as a global
    // constructor, before FreeRTOS, and an RGB LED (digitalWrite through
    // RMT on the S3/C3 devkits) blocks forever there.
    _ACT_LED = ACT_LED;
    _ACT_HIGH = ACT_HIGH;

    setAvailability("Availability");

#if defined(SMARTLIB_ESP) && defined(SMARTLIB_OTA_PASSWORD)
    setOTAPassword(SMARTLIB_OTA_PASSWORD);
#endif

    // Default is 256 bytes for the whole packet, which silently truncates
    // longer publishes (topic + payload + header).
    client.setBufferSize(512);
    // Always installed, so on()/onRaw() work without setMQTTCallback().
    client.setCallback(mqttCallback);
}

#ifndef WIFI_NONE
SmartLib::SmartLib(const char *deviceName, const char *SSID, const char *PASS, const char *MQTT_SRV,
                   const char *MQTT_NAME, const char *MQTT_PASS, int8_t ACT_LED, bool ACT_HIGH)
    : client(_wifiClient)
{
    ethOrWiFi = true; // WiFi
    commonInit(deviceName, MQTT_NAME, MQTT_PASS, ACT_LED, ACT_HIGH);

    setStringSafe(_SSID, sizeof(_SSID), SSID);
    setStringSafe(_PASS, sizeof(_PASS), PASS);

    client.setServer(MQTT_SRV, 1883);
}
#endif

SmartLib::SmartLib(const char *deviceName, uint8_t macAddress[6], int8_t ETH_CS, int8_t ETH_RST, const char *MQTT_SRV,
                   const char *MQTT_NAME, const char *MQTT_PASS, int8_t ACT_LED, bool ACT_HIGH)
    : client(_ethClient)
{
    ethOrWiFi = false; // Ethernet
    commonInit(deviceName, MQTT_NAME, MQTT_PASS, ACT_LED, ACT_HIGH);

    memcpy(_macAddress, macAddress, sizeof(_macAddress));
    _ETH_CS = ETH_CS;
    _ETH_RST = ETH_RST;

    client.setServer(MQTT_SRV, 1883);
}

void SmartLib::begin()
{
    if (_ACT_LED != -1)
    {
        pinMode(_ACT_LED, OUTPUT);
        setAct(false);
    }

#ifdef SMARTLIB_ESP
    if (_ntpEnabled)
    {
        // Before the first sync too, so a clock kept across a soft reset
        // already reads as local time.
        setenv("TZ", _tz, 1);
        tzset();
    }
#endif

#ifndef WIFI_NONE
    if (ethOrWiFi)
    {
#if defined(ESP32)
        // The hostname is only picked up when the STA interface starts.
        WiFi.setHostname(_hostname);
        WiFi.mode(WIFI_STA);
        if (_ntpEnabled)
        {
            sntp_set_time_sync_notification_cb([](struct timeval *) { s_timeSynced = true; });
#if LWIP_DHCP_GET_NTP_SRV
            // Must be on before the lease arrives: lwIP then stores the
            // DHCP-offered NTP server (option 42) in SNTP slot 0.
            esp_sntp_servermode_dhcp(true);
#endif
        }
#elif defined(ESP8266)
        WiFi.mode(WIFI_STA);
        WiFi.hostname(_hostname);
        if (_ntpEnabled)
        {
            settimeofday_cb([](bool fromSntp) {
                if (fromSntp)
                    s_timeSynced = true;
            });
            sntp_servermode_dhcp(1);
        }
#endif
        WiFi.begin(_SSID, _PASS);
    }
    else
#endif
    {
        if (_ETH_RST != -1)
        {
            pinMode(_ETH_RST, OUTPUT);
            digitalWrite(_ETH_RST, LOW);
            delay(25);
            digitalWrite(_ETH_RST, HIGH);
            delay(25);
        }
#ifdef ETHERNET_FORK_VRATK0529
        Ethernet.setHostname(_hostname);
#endif
        Ethernet.init(_ETH_CS);
        Ethernet.begin(_macAddress);
        if (Ethernet.hardwareStatus() == EthernetNoHardware)
        {
            ESP_LOGE("Ethernet", "No Ethernet hardware found");
            return;
        }
    }
}

void SmartLib::maintainConnection()
{
#ifndef WIFI_NONE
    if (ethOrWiFi)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            setAct(false);
            WiFi.begin(_SSID, _PASS);
            uint8_t reconnectionTries = 0;
            while (!WiFi.isConnected())
            {
                ESP_LOGD("WiFi", "Connecting");
                toggleAct();
                delay(500);
                reconnectionTries++;
                if (reconnectionTries >= 20)
                {
                    WiFi.disconnect(true, false);
                    ESP_LOGD("WiFi", "Disconnected");
                    _reconnectionTries++;

                    if (_reconnectionTries >= 2)
#if defined(ESP8266)
                        ESP.restart();
#else
                        esp_deep_sleep(1000000);
#endif

                    delay(1000);
                    maintainConnection();
                    return;
                }
            }
            setAct(false);
            ESP_LOGD("WiFi", "Connected");
            _reconnectionTries = 0;
        }
    }
    else
#endif
    {
        Ethernet.maintain();
        if (Ethernet.linkStatus() == LinkOFF)
        {
            ESP_LOGW("Ethernet", "Ethernet link is down");
            return;
        }

        // If DHCP failed at boot we have no address and never would have
        // retried, so keep asking while the link is up.
        if (Ethernet.localIP() == IPAddress(0, 0, 0, 0))
        {
            if (_lastDhcpAttempt == 0 || millis() - _lastDhcpAttempt >= 30000)
            {
                _lastDhcpAttempt = millis();
                ESP_LOGW("Ethernet", "No IP address, retrying DHCP");
                Ethernet.begin(_macAddress);
            }
            return;
        }
    }

    // Before the first MQTT session, so the Info it publishes already
    // shows OTA and NTP running.
    if (!_netServicesStarted)
        startNetworkServices();

    if (!client.connected())
    {
        // Reconnecting is slow and blocking, so do not hammer it from every
        // loop() iteration.
        if (_lastMqttAttempt != 0 && millis() - _lastMqttAttempt < 2000)
            return;
        _lastMqttAttempt = millis();

        setAct(true);
        // Readable in the broker log, and unique even if two devices end
        // up with the same name.
        uint8_t mac[6];
        macAddress(mac);
        char clientId[48];
        snprintf(clientId, sizeof(clientId), "%s-%02X%02X%02X", _hostname, mac[3], mac[4], mac[5]);
        ESP_LOGD("MQTT", "Connecting as %s", clientId);

        bool connected;
        if (_availEnabled)
            // Last will: the broker publishes "offline" here if we vanish
            // without a clean disconnect.
            connected = client.connect(clientId, _MQTT_NAME, _MQTT_PASS, _availTopic, 0, true, _availOffline);
        else
            connected = client.connect(clientId, _MQTT_NAME, _MQTT_PASS);

        if (!connected)
        {
            ESP_LOGW("MQTT", "Connection failed: %d", client.state());
            if (client.state() == MQTT_CONNECT_BAD_CREDENTIALS || client.state() == MQTT_CONNECT_UNAUTHORIZED)
                ESP_LOGE("MQTT", "Bad username or password");
        }
        else
        {
            onConnected();
        }
        setAct(false);
    }
}

void SmartLib::onConnected()
{
    ESP_LOGD("MQTT", "Connected");

    if ((size_t)snprintf(_topic, sizeof(_topic), "%s/RX/#", _deviceName) >= (size_t)sizeof(_topic))
    {
        ESP_LOGE("SNPRINTF", "_topic too long");
        return;
    }
    ESP_LOGD("MQTT", "Subscribed to: %s", _topic);
    client.subscribe(_topic);

    // Absolute subscriptions have to be renewed on every session.
    for (uint8_t i = 0; i < _handlerCount; i++)
    {
        if (_handlers[i].raw)
        {
            ESP_LOGD("MQTT", "Subscribed to: %s", _handlers[i].filter);
            client.subscribe(_handlers[i].filter);
        }
    }

    if (_availEnabled)
        client.publish(_availTopic, _availOnline, true);

    publishInfo();

    if (_connectedCallback != nullptr)
        _connectedCallback();
}
bool SmartLib::getStatus()
{
#ifndef WIFI_NONE
    if (ethOrWiFi)
    {
        return WiFi.isConnected() && client.connected();
    }
    else
#endif
    {
        return Ethernet.linkStatus() == LinkON && client.connected();
    }
}

void SmartLib::setAct(bool state)
{
    if (_ACT_LED != -1)
    {
        actStatus = !state != !_ACT_HIGH; // XOR these two
        digitalWrite(_ACT_LED, actStatus);
    }
}
void SmartLib::toggleAct()
{
    if (_ACT_LED != -1)
    {
        actStatus = !actStatus;
        digitalWrite(_ACT_LED, actStatus);
    }
}

void SmartLib::setMQTTCallback(void (*callback)(char *, uint8_t *, unsigned int))
{
    _mqttCallback = callback;
}

void SmartLib::setConnectedCallback(void (*callback)(void))
{
    _connectedCallback = callback;
}

void SmartLib::sendToMQTT(const char *topic, const char *fmt, ...)
{
    char payload[256];

    va_list args;
    va_start(args, fmt);
    bool tooLong = (size_t)vsnprintf(payload, sizeof(payload), fmt, args) >= (size_t)sizeof(payload);
    va_end(args);

    if (tooLong)
    {
        ESP_LOGE("VSNPRINTF", "payload too long");
        return;
    }

    sendToMQTTStr(topic, payload);
}
void SmartLib::sendToMQTTRetained(const char *topic, const char *fmt, ...)
{
    char payload[256];

    va_list args;
    va_start(args, fmt);
    bool tooLong = (size_t)vsnprintf(payload, sizeof(payload), fmt, args) >= (size_t)sizeof(payload);
    va_end(args);

    if (tooLong)
    {
        ESP_LOGE("VSNPRINTF", "payload too long");
        return;
    }

    sendToMQTTStr(topic, payload, true);
}
void SmartLib::sendToMQTTStr(const char *topic, const char *payload, bool retain)
{
    if ((size_t)snprintf(_topic, sizeof(_topic), "%s/TX/%s", _deviceName, topic) >= (size_t)sizeof(_topic))
    {
        ESP_LOGE("SNPRINTF", "_topic too long");
        return;
    }

    setAct(true);
    client.publish(_topic, payload, retain);
    setAct(false);
}

void SmartLib::replyTo(const char *rxTopic, const char *payload, bool retain)
{
    const char *sub = rxSubTopic(rxTopic);
    if (sub == nullptr)
    {
        ESP_LOGW("MQTT", "replyTo: not an RX topic: %s", rxTopic);
        return;
    }

    sendToMQTTStr(sub, payload, retain);
}

void SmartLib::publishDiagnostics(const char *subTopic)
{
    IPAddress ip = getIP();

#if defined(ESP8266) || defined(ESP32)
    sendToMQTT(subTopic,
               "{\"uptime\":%lu,\"heap\":%lu,\"ip\":\"%u.%u.%u.%u\",\"rssi\":%ld,\"link\":\"%s\"}",
               (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap(), ip[0], ip[1], ip[2],
               ip[3], (long)getRSSI(), ethOrWiFi ? "wifi" : "ethernet");
#else
    sendToMQTT(subTopic, "{\"uptime\":%lu,\"ip\":\"%u.%u.%u.%u\",\"link\":\"ethernet\"}",
               (unsigned long)(millis() / 1000), ip[0], ip[1], ip[2], ip[3]);
#endif
}

void SmartLib::setAvailability(const char *subTopic, const char *onlinePayload, const char *offlinePayload)
{
    if ((size_t)snprintf(_availTopic, sizeof(_availTopic), "%s/TX/%s", _deviceName, subTopic) >=
        (size_t)sizeof(_availTopic))
    {
        ESP_LOGE("SNPRINTF", "availability topic too long");
        _availEnabled = false;
        return;
    }

    setStringSafe(_availOnline, sizeof(_availOnline), onlinePayload);
    setStringSafe(_availOffline, sizeof(_availOffline), offlinePayload);
    _availEnabled = true;
}

void SmartLib::disableAvailability()
{
    _availEnabled = false;
}

// ---- identity ----------------------------------------------------------------

void SmartLib::setHostname(const char *hostname)
{
    // RFC 1123 label: letters, digits and '-', no '-' at either end.
    size_t j = 0;
    for (size_t i = 0; hostname[i] != '\0' && j < sizeof(_hostname) - 1; i++)
    {
        char c = hostname[i];
        if (isalnum((unsigned char)c))
            _hostname[j++] = c;
        else if (strchr("-_ ./", c) != nullptr && j > 0 && _hostname[j - 1] != '-')
            _hostname[j++] = '-';
    }
    while (j > 0 && _hostname[j - 1] == '-')
        j--;
    _hostname[j] = '\0';

    if (j == 0)
        setStringSafe(_hostname, sizeof(_hostname), "smartlib");
}

void SmartLib::macAddress(uint8_t mac[6]) const
{
#ifndef WIFI_NONE
    if (ethOrWiFi)
    {
        WiFi.macAddress(mac);
        return;
    }
#endif
    memcpy(mac, _macAddress, 6);
}

#ifdef SMARTLIB_ESP
static const char *resetReason()
{
#if defined(ESP32)
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt-wdt";
    case ESP_RST_TASK_WDT:
        return "task-wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    default:
        return "unknown";
    }
#else
    static char reason[32] = {0};
    if (reason[0] == '\0')
        strncpy(reason, ESP.getResetReason().c_str(), sizeof(reason) - 1);
    return reason;
#endif
}

static const char *sketchMD5()
{
    // Hashes the whole app image, so only once.
    static char md5[33] = {0};
    if (md5[0] == '\0')
        strncpy(md5, ESP.getSketchMD5().c_str(), sizeof(md5) - 1);
    return md5;
}
#endif

size_t SmartLib::infoJson(char *buf, size_t size, bool live)
{
    uint8_t mac[6];
    macAddress(mac);
    IPAddress ip = getIP();

    int n = snprintf(buf, size,
                     "{\"name\":\"%s\",\"host\":\"%s\",\"ip\":\"%u.%u.%u.%u\","
                     "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"link\":\"%s\",\"fw\":\"%s\"",
                     _deviceName, _hostname, ip[0], ip[1], ip[2], ip[3], mac[0], mac[1], mac[2], mac[3], mac[4],
                     mac[5], ethOrWiFi ? "wifi" : "ethernet", _fwVersion ? _fwVersion : "");

#ifdef SMARTLIB_ESP
    char chip[24];
#if defined(ESP32)
    snprintf(chip, sizeof(chip), "%s rev%u", ESP.getChipModel(), (unsigned)ESP.getChipRevision());
#else
    snprintf(chip, sizeof(chip), "ESP8266");
#endif
    if (n > 0 && (size_t)n < size)
        n += snprintf(buf + n, size - n,
                      ",\"chip\":\"%s\",\"md5\":\"%s\",\"sdk\":\"%s\",\"reset\":\"%s\",\"ota\":%s,\"ntp\":\"%s\"",
                      chip, sketchMD5(), ESP.getSdkVersion(), resetReason(), _otaRunning ? "true" : "false",
                      ntpServer());
#endif

    if (live && n > 0 && (size_t)n < size)
    {
        n += snprintf(buf + n, size - n, ",\"uptime\":%lu,\"rssi\":%ld,\"mqtt\":%s",
                      (unsigned long)(millis() / 1000), (long)getRSSI(), client.connected() ? "true" : "false");
#ifdef SMARTLIB_ESP
        if (n > 0 && (size_t)n < size)
            n += snprintf(buf + n, size - n, ",\"heap\":%lu,\"time\":%lu", (unsigned long)ESP.getFreeHeap(),
                          (unsigned long)(timeValid() ? time(nullptr) : 0));
#endif
    }

    if (n > 0 && (size_t)n < size)
        n += snprintf(buf + n, size - n, "}");

    if (n <= 0 || (size_t)n >= size)
    {
        ESP_LOGE("SNPRINTF", "info JSON too long");
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

void SmartLib::publishInfo(const char *subTopic)
{
    char payload[400];
    if (infoJson(payload, sizeof(payload), false) > 0)
        sendToMQTTStr(subTopic, payload, true);
}

void SmartLib::identify(uint16_t seconds)
{
    if (_ACT_LED == -1)
        ESP_LOGW("Identify", "No activity LED configured, nothing to blink");

    _identifyStart = millis();
    _identifyLength = (uint32_t)seconds * 1000UL;
    _identifyLastToggle = 0;
    if (seconds == 0)
        setAct(false);
    ESP_LOGI("Identify", "Blinking for %u s", (unsigned)seconds);
}

void SmartLib::serviceIdentify()
{
    if (_identifyLength == 0)
        return;

    if (millis() - _identifyStart >= _identifyLength)
    {
        _identifyLength = 0;
        setAct(false);
        return;
    }
    if (every(_identifyLastToggle, 100))
        toggleAct();
}

// ---- discovery ---------------------------------------------------------------
// tools/smartscan.py sends "SMARTLIB?" to every address in a range (or to
// the broadcast address) and lists whoever answers with infoJson().
// "SMARTLIB IDENTIFY <s>" blinks the LED, like <device>/RX/Identify.

void SmartLib::serviceDiscovery()
{
    if (_discovery == nullptr)
        return;

    int len = _discovery->parsePacket();
    if (len <= 0)
        return;

    char msg[48];
    int n = _discovery->read((uint8_t *)msg, sizeof(msg) - 1);
    if (n <= 0)
        return;
    msg[n] = '\0';

    char reply[512];
    size_t replyLen;
    if (strncmp(msg, "SMARTLIB?", 9) == 0)
    {
        replyLen = infoJson(reply, sizeof(reply), true);
    }
    else if (strncmp(msg, "SMARTLIB IDENTIFY", 17) == 0)
    {
        int seconds = atoi(msg + 17);
        identify((uint16_t)constrain(seconds > 0 ? seconds : 10, 0, 3600));
        replyLen = (size_t)snprintf(reply, sizeof(reply), "OK");
    }
    else
    {
        return;
    }

    if (replyLen == 0)
        return;
    _discovery->beginPacket(_discovery->remoteIP(), _discovery->remotePort());
    _discovery->write((const uint8_t *)reply, replyLen);
    _discovery->endPacket();
}

// ---- network services --------------------------------------------------------

bool SmartLib::networkUp()
{
#ifndef WIFI_NONE
    if (ethOrWiFi)
        return WiFi.isConnected();
#endif
    return Ethernet.linkStatus() != LinkOFF && !(Ethernet.localIP() == IPAddress(0, 0, 0, 0));
}

void SmartLib::startNetworkServices()
{
    _netServicesStarted = true;

    if (_discoveryEnabled)
    {
#ifndef WIFI_NONE
        if (ethOrWiFi)
            _discovery = &_wifiDiscoveryUdp;
        else
#endif
            _discovery = &_ethDiscoveryUdp;

        if (!_discovery->begin(SMARTLIB_DISCOVERY_PORT))
        {
            ESP_LOGE("Discovery", "UDP port %u unavailable", SMARTLIB_DISCOVERY_PORT);
            _discovery = nullptr;
        }
    }

#ifdef SMARTLIB_ESP
    startOTA();
    startTime();
#endif
}

#ifdef SMARTLIB_ESP
// ---- OTA ---------------------------------------------------------------------

void SmartLib::setOTAPassword(const char *password)
{
    setStringSafe(_otaPassword, sizeof(_otaPassword), password ? password : "");
}

static void logOtaProgress(uint32_t done, uint32_t total)
{
    static uint8_t lastTenth = 255;
    uint8_t tenth = total ? (uint8_t)((uint64_t)done * 10 / total) : 0;
    if (done == 0)
        lastTenth = 255;
    if (tenth != lastTenth)
    {
        lastTenth = tenth;
        ESP_LOGI("OTA", "%u%%", (unsigned)tenth * 10);
    }
}

static const char *updateError()
{
#if defined(ESP32)
    return Update.errorString();
#else
    static String error;
    error = Update.getErrorString();
    return error.c_str();
#endif
}

void SmartLib::startOTA()
{
    if (!_otaEnabled)
        return;

    if (_otaPassword[0] == '\0')
    {
        if (!_otaNoPassword)
        {
            ESP_LOGE("OTA", "No password set, OTA is off (setOTAPassword() or -DSMARTLIB_OTA_PASSWORD)");
            return;
        }
        ESP_LOGW("OTA", "Running WITHOUT a password, anyone on the network can flash this device");
    }

#ifndef WIFI_NONE
    if (ethOrWiFi)
    {
        ArduinoOTA.setHostname(_hostname);
        ArduinoOTA.setPort(SMARTLIB_OTA_PORT);
        if (_otaPassword[0] != '\0')
            ArduinoOTA.setPassword(_otaPassword);
        ArduinoOTA.onStart([this]() { otaStarting(); });
        ArduinoOTA.onProgress([](unsigned int done, unsigned int total) { logOtaProgress(done, total); });
        ArduinoOTA.onEnd([]() { ESP_LOGI("OTA", "Done, rebooting"); });
        ArduinoOTA.onError([this](ota_error_t error) {
            static const char *const reasons[] = {"wrong password", "cannot start", "cannot connect back",
                                                  "receive failed", "image rejected"};
            ESP_LOGE("OTA", "Failed: %s", (unsigned)error < 5 ? reasons[error] : "unknown error");
            _otaActive = false;
        });
        ArduinoOTA.begin();
        ESP_LOGI("OTA", "Ready at %s.local:%u", _hostname, (unsigned)SMARTLIB_OTA_PORT);
    }
    else
#endif
    {
        if (!_ethOtaUdp.begin(SMARTLIB_OTA_PORT))
        {
            ESP_LOGE("OTA", "UDP port %u unavailable", (unsigned)SMARTLIB_OTA_PORT);
            return;
        }
        IPAddress ip = Ethernet.localIP();
        ESP_LOGI("OTA", "Ready at %u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], (unsigned)SMARTLIB_OTA_PORT);
    }
    _otaRunning = true;
}

void SmartLib::otaStarting()
{
    _otaActive = true;
    ESP_LOGI("OTA", "Update starting");

    if (_otaStartCallback != nullptr)
        _otaStartCallback();

    // Nothing gets serviced while flashing, so say so instead of letting
    // the broker time the session out. A failed update reconnects from
    // loop() and publishes "online" again.
    if (client.connected())
    {
        if (_availEnabled)
            client.publish(_availTopic, _availOffline, true);
        client.disconnect();
    }
    setAct(true);
}

static void md5Hex(const char *text, char out[33])
{
    MD5Builder md5;
    md5.begin();
    md5.add(text);
    md5.calculate();
    md5.getChars(out);
}

// The espota protocol, as ArduinoOTA implements it: a UDP invitation
// "<cmd> <port> <size> <md5>", an optional MD5 challenge, then we connect
// back to the uploader over TCP and acknowledge every chunk with the
// number of bytes written.
void SmartLib::ethOtaHandle()
{
    if (_ethOtaState == ETH_OTA_WAITAUTH && millis() - _ethOtaAuthStart > 10000)
        _ethOtaState = ETH_OTA_IDLE; // uploader gave up

    if (_ethOtaUdp.parsePacket() <= 0)
        return;

    char msg[128];
    int n = _ethOtaUdp.read((uint8_t *)msg, sizeof(msg) - 1);
    if (n <= 0)
        return;
    msg[n] = '\0';

    IPAddress host = _ethOtaUdp.remoteIP();
    uint16_t hostPort = _ethOtaUdp.remotePort();
    auto reply = [&](const char *text) {
        _ethOtaUdp.beginPacket(host, hostPort);
        _ethOtaUdp.write((const uint8_t *)text, strlen(text));
        _ethOtaUdp.endPacket();
    };

    if (_ethOtaState == ETH_OTA_IDLE)
    {
        int command;
        unsigned int port;
        unsigned long size;
        char md5[33];
        if (sscanf(msg, "%d %u %lu %32s", &command, &port, &size, md5) != 4 || strlen(md5) != 32)
            return;
        if (command != 0 && command != 100) // U_FLASH, U_SPIFFS
            return;

        _ethOtaCommand = command;
        _ethOtaPort = (uint16_t)port;
        _ethOtaSize = size;
        memcpy(_ethOtaMd5, md5, sizeof(_ethOtaMd5));

        if (_otaPassword[0] != '\0')
        {
            char seed[24];
#if defined(ESP32)
            snprintf(seed, sizeof(seed), "%lu%lu", (unsigned long)esp_random(), (unsigned long)micros());
#else
            snprintf(seed, sizeof(seed), "%lu%lu", (unsigned long)ESP.random(), (unsigned long)micros());
#endif
            md5Hex(seed, _ethOtaNonce);

            char text[40];
            snprintf(text, sizeof(text), "AUTH %s", _ethOtaNonce);
            reply(text);
            _ethOtaState = ETH_OTA_WAITAUTH;
            _ethOtaAuthStart = millis();
            return;
        }

        reply("OK");
        ethOtaRun(host, _ethOtaPort, _ethOtaSize, _ethOtaCommand, _ethOtaMd5);
        return;
    }

    // ETH_OTA_WAITAUTH: "200 <cnonce> <md5(md5(pass):nonce:cnonce)>"
    _ethOtaState = ETH_OTA_IDLE;
    int command;
    char cnonce[33], response[33];
    if (sscanf(msg, "%d %32s %32s", &command, cnonce, response) != 3 || command != 200)
        return;

    char passMd5[33], expected[33];
    md5Hex(_otaPassword, passMd5);
    String challenge = String(passMd5) + ":" + _ethOtaNonce + ":" + cnonce;
    md5Hex(challenge.c_str(), expected);

    if (strcmp(expected, response) != 0)
    {
        ESP_LOGW("OTA", "Authentication failed");
        reply("Authentication Failed");
        return;
    }

    reply("OK");
    ethOtaRun(host, _ethOtaPort, _ethOtaSize, _ethOtaCommand, _ethOtaMd5);
}

void SmartLib::ethOtaRun(IPAddress host, uint16_t port, uint32_t size, int command, const char *md5)
{
    if (!Update.begin(size, command))
    {
        ESP_LOGE("OTA", "Cannot start: %s", updateError());
        return;
    }
    Update.setMD5(md5);
    otaStarting();

    EthernetClient ota;
    bool connected = false;
    for (uint8_t i = 0; i < 5 && !connected; i++)
    {
        connected = ota.connect(host, port);
        if (!connected)
            delay(100);
    }
    if (!connected)
    {
        ESP_LOGE("OTA", "Cannot connect back to %u.%u.%u.%u:%u", host[0], host[1], host[2], host[3], port);
        Update.end(); // not finished: just releases the partition
        _otaActive = false;
        return;
    }

    static uint8_t buf[1460];
    uint32_t total = 0;
    size_t written = 0;
    uint8_t nudges = 0;
    logOtaProgress(0, size);

    while (!Update.isFinished() && ota.connected())
    {
        uint32_t waitStart = millis();
        int available;
        while ((available = ota.available()) <= 0 && ota.connected() && millis() - waitStart < 1000)
            delay(1);

        if (available <= 0)
        {
            // Same as ArduinoOTA: re-send the last ack in case it was lost.
            if (written != 0 && nudges++ < 3)
            {
                ota.print((unsigned long)written);
                continue;
            }
            break;
        }
        nudges = 0;

        int got = ota.read(buf, available < (int)sizeof(buf) ? available : (int)sizeof(buf));
        if (got <= 0)
            continue;

        written = Update.write(buf, (size_t)got);
        if (written == 0)
        {
            ESP_LOGE("OTA", "Write failed: %s", updateError());
            break;
        }
        ota.print((unsigned long)written);
        total += written;
        logOtaProgress(total, size);
    }

    if (Update.end())
    {
        ota.print("OK");
        ota.stop();
        ESP_LOGI("OTA", "Done, rebooting");
        delay(100);
        ESP.restart();
    }

    ESP_LOGE("OTA", "Failed after %lu of %lu bytes: %s", (unsigned long)total, (unsigned long)size, updateError());
    Update.printError(ota);
    ota.stop();
    _otaActive = false;
}

// ---- time --------------------------------------------------------------------

void SmartLib::setTimezone(const char *posixTz)
{
    setStringSafe(_tz, sizeof(_tz), posixTz);
}

void SmartLib::setNTPServer(const char *server)
{
    setStringSafe(_ntpFallback, sizeof(_ntpFallback), server);
}

bool SmartLib::timeValid()
{
    return time(nullptr) > 1700000000; // Nov 2023
}

bool SmartLib::timeString(char *buf, size_t size, const char *fmt)
{
    if (size == 0)
        return false;
    buf[0] = '\0';
    if (!timeValid())
        return false;

    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    return strftime(buf, size, fmt, &tm) > 0;
}

const char *SmartLib::ntpServer()
{
    if (!_ntpEnabled)
        return "";

#ifndef WIFI_NONE
    // lwIP follows DHCP renewals on its own, so ask it rather than
    // remembering what it was at startup.
    if (ethOrWiFi && _ntpConfigured)
    {
#if defined(ESP32)
        const char *name = esp_sntp_getservername(0);
        const ip_addr_t *addr = esp_sntp_getserver(0);
#else
        const char *name = sntp_getservername(0);
        const ip_addr_t *addr = sntp_getserver(0);
#endif
        if (name != nullptr)
            return name;
        if (addr != nullptr && !ip_addr_isany(addr))
        {
            ipaddr_ntoa_r(addr, _ntpDhcp, sizeof(_ntpDhcp));
            return _ntpDhcp;
        }
    }
#endif
    return _ntpActive;
}

void SmartLib::startTime()
{
    if (!_ntpEnabled)
        return;

#ifndef WIFI_NONE
    if (ethOrWiFi)
    {
        // With DHCP server mode on, a lease carrying option 42 has already
        // put that server in slot 0 (as an address, with no name).
#if defined(ESP32)
        const char *name = esp_sntp_getservername(0);
        const ip_addr_t *addr = esp_sntp_getserver(0);
#else
        const char *name = sntp_getservername(0);
        const ip_addr_t *addr = sntp_getserver(0);
#endif
        if (name == nullptr && addr != nullptr && !ip_addr_isany(addr))
            ipaddr_ntoa_r(addr, _ntpDhcp, sizeof(_ntpDhcp));

        // configTzTime() keeps the pointers, hence the member buffers.
        if (_ntpDhcp[0] != '\0')
        {
            _ntpActive = _ntpDhcp;
#if defined(ESP32)
            configTzTime(_tz, _ntpDhcp, _ntpFallback);
#else
            configTime(_tz, _ntpDhcp, _ntpFallback);
#endif
            ESP_LOGI("NTP", "Using %s from DHCP, %s as backup", _ntpDhcp, _ntpFallback);
        }
        else
        {
            _ntpActive = _ntpFallback;
#if defined(ESP32)
            configTzTime(_tz, _ntpFallback);
#else
            configTime(_tz, _ntpFallback);
#endif
            ESP_LOGI("NTP", "No NTP server from DHCP, using %s", _ntpFallback);
        }
    }
#endif
    // Ethernet: serviceTime() polls on its own schedule.
    _ntpConfigured = true;
}

void SmartLib::ethNtpSend()
{
    _ethNtpLastAttempt = millis();
    if (_ethNtpLastAttempt == 0)
        _ethNtpLastAttempt = 1; // 0 means "never"

    IPAddress dhcp(0, 0, 0, 0);
#ifdef ETHERNET_FORK_VRATK0529
    dhcp = Ethernet.ntpServerIP();
#endif
    // Alternate with the fallback while the DHCP-offered server is not
    // answering.
    bool useDhcp = dhcp[0] != 0 && _ethNtpFailures % 2 == 0;
    if (useDhcp)
    {
        snprintf(_ntpDhcp, sizeof(_ntpDhcp), "%u.%u.%u.%u", dhcp[0], dhcp[1], dhcp[2], dhcp[3]);
        _ntpActive = _ntpDhcp;
    }
    else
    {
        _ntpActive = _ntpFallback;
    }

    if (!_ethNtpUdp.begin(NTP_LOCAL_PORT))
    {
        ESP_LOGW("NTP", "No free socket");
        return;
    }

    uint8_t packet[48] = {0};
    packet[0] = 0b11100011; // LI = 3 (unsynchronised), version 4, client mode

    int ok = useDhcp ? _ethNtpUdp.beginPacket(dhcp, 123) : _ethNtpUdp.beginPacket(_ntpFallback, 123);
    if (ok)
    {
        _ethNtpUdp.write(packet, sizeof(packet));
        ok = _ethNtpUdp.endPacket();
    }
    if (!ok)
    {
        ESP_LOGW("NTP", "Cannot send to %s", _ntpActive);
        _ethNtpUdp.stop();
        _ethNtpFailures++;
        return;
    }

    _ethNtpWaiting = true;
    _ethNtpSentAt = millis();
}

void SmartLib::ethNtpReceive()
{
    if (_ethNtpUdp.parsePacket() >= 48)
    {
        uint8_t packet[48];
        _ethNtpUdp.read(packet, sizeof(packet));
        _ethNtpUdp.stop();
        _ethNtpWaiting = false;

        uint8_t mode = packet[0] & 0x07;
        uint8_t stratum = packet[1];
        // Transmit timestamp at offset 40: seconds, then 2^-32 fractions.
        uint32_t seconds = ((uint32_t)packet[40] << 24) | ((uint32_t)packet[41] << 16) |
                           ((uint32_t)packet[42] << 8) | packet[43];
        uint32_t fraction = ((uint32_t)packet[44] << 24) | ((uint32_t)packet[45] << 16) |
                            ((uint32_t)packet[46] << 8) | packet[47];

        if (mode != 4 || stratum == 0 || seconds <= NTP_EPOCH_OFFSET) // 0 = kiss-o'-death
        {
            ESP_LOGW("NTP", "Bad reply from %s", _ntpActive);
            _ethNtpFailures++;
            return;
        }

        // Half the round trip is the best guess for the reply's age.
        uint64_t us = (((uint64_t)fraction * 1000000ULL) >> 32) + (uint64_t)(millis() - _ethNtpSentAt) * 500ULL;
        struct timeval tv = {};
        tv.tv_sec = (time_t)(seconds - NTP_EPOCH_OFFSET) + (time_t)(us / 1000000ULL);
        tv.tv_usec = (suseconds_t)(us % 1000000ULL);
        settimeofday(&tv, nullptr);

        _ethNtpSynced = true;
        _ethNtpFailures = 0;
        s_timeSynced = true;
        return;
    }

    if (millis() - _ethNtpSentAt < 2000)
        return;

    _ethNtpUdp.stop();
    _ethNtpWaiting = false;
    _ethNtpFailures++;
    ESP_LOGW("NTP", "No answer from %s", _ntpActive);

#ifdef ETHERNET_FORK_VRATK0529
    // The DHCP server did not answer: try the fallback right away.
    if (_ethNtpFailures % 2 == 1 && Ethernet.ntpServerIP()[0] != 0)
        _ethNtpLastAttempt = 0;
#endif
}

void SmartLib::serviceTime()
{
    if (!_ntpEnabled)
        return;

    if (s_timeSynced)
    {
        s_timeSynced = false;
        char now[32];
        timeString(now, sizeof(now));
        if (!s_timeSyncLogged)
            ESP_LOGI("NTP", "Time synced from %s: %s", ntpServer(), now);
        else
            ESP_LOGD("NTP", "Time synced from %s: %s", ntpServer(), now);
        s_timeSyncLogged = true;
    }

    if (ethOrWiFi)
        return; // lwIP SNTP runs by itself

    if (_ethNtpWaiting)
    {
        ethNtpReceive();
        return;
    }

    uint32_t interval = _ethNtpSynced ? NTP_RESYNC_MS : NTP_RETRY_MS;
    if (_ethNtpLastAttempt != 0 && millis() - _ethNtpLastAttempt < interval)
        return;
    ethNtpSend();
}
#endif // SMARTLIB_ESP

// ---- routing -----------------------------------------------------------------

bool SmartLib::on(const char *subTopicFilter, SmartTopicHandler handler)
{
    if (_handlerCount >= SMARTLIB_MAX_HANDLERS || handler == nullptr)
        return false; // raise SMARTLIB_MAX_HANDLERS if you need more

    _handlers[_handlerCount++] = {subTopicFilter, handler, false};
    return true;
}

bool SmartLib::onRaw(const char *topicFilter, SmartTopicHandler handler)
{
    if (_handlerCount >= SMARTLIB_MAX_HANDLERS || handler == nullptr)
        return false;

    _handlers[_handlerCount++] = {topicFilter, handler, true};
    if (client.connected())
        client.subscribe(topicFilter);
    return true;
}

void SmartLib::clearHandlers()
{
    _handlerCount = 0;
}

// MQTT-style single-level ('+') and multi-level ('#', last segment only)
// wildcards, matched in place with no allocation.
bool SmartLib::topicMatches(const char *filter, const char *topic)
{
    while (true)
    {
        const char *fSlash = strchr(filter, '/');
        const char *tSlash = strchr(topic, '/');
        size_t fLen = fSlash ? (size_t)(fSlash - filter) : strlen(filter);
        size_t tLen = tSlash ? (size_t)(tSlash - topic) : strlen(topic);

        if (fLen == 1 && filter[0] == '#')
            return true; // matches this level and everything after it

        bool segMatches;
        if (fLen == 1 && filter[0] == '+')
            segMatches = true; // '+' matches any single level
        else
            segMatches = (fLen == tLen) && (strncmp(filter, topic, fLen) == 0);

        if (!segMatches)
            return false;
        if (!fSlash && !tSlash)
            return true; // both ended on this level
        if (!fSlash || !tSlash)
            return false; // one ended before the other, and it was not '#'

        filter = fSlash + 1;
        topic = tSlash + 1;
    }
}

void SmartLib::dispatch(char *topic, uint8_t *payload, unsigned int length)
{
    // Both arguments point into PubSubClient's single receive buffer, which a
    // publish() from inside a handler overwrites -- so take a copy first.
    char fullTopic[128];
    setStringSafe(fullTopic, sizeof(fullTopic), topic);
#if defined(ESP8266)
    String message; // no (buffer, length) constructor on this core
    message.concat((const char *)payload, length);
#else
    String message((const char *)payload, length);
#endif

    const char *sub = rxSubTopic(fullTopic);

    // Built in: <device>/RX/Identify [seconds]
    if (sub != nullptr && strcmp(sub, "Identify") == 0)
    {
        String p = message;
        p.trim();
        long seconds = p.length() == 0 ? 10 : p.toInt();
        if (seconds == 0 && isOn(p))
            seconds = 10;
        identify((uint16_t)constrain(seconds, 0L, 3600L));
        return;
    }

    for (uint8_t i = 0; i < _handlerCount; i++)
    {
        const char *subject = _handlers[i].raw ? fullTopic : sub;
        if (subject != nullptr && topicMatches(_handlers[i].filter, subject))
            _handlers[i].handler(fullTopic, message);
    }
}

bool SmartLib::isOn(const String &payload)
{
    String value = payload;
    value.trim();
    value.toLowerCase();

    return value == "1" || value == "on" || value == "true" || value == "yes" || value == "open";
}

bool SmartLib::every(uint32_t &last, uint32_t period)
{
    uint32_t now = millis();
    if (now - last < period)
        return false;

    last = now;
    return true;
}

IPAddress SmartLib::getIP() const
{
#ifndef WIFI_NONE
    if (ethOrWiFi)
        return WiFi.localIP();
#endif
    return Ethernet.localIP();
}

int32_t SmartLib::getRSSI() const
{
#ifndef WIFI_NONE
    if (ethOrWiFi)
        return WiFi.RSSI();
#endif
    return 0; // no such thing on Ethernet
}

void SmartLib::loop()
{
    maintainConnection();
    client.loop();

    if (_netServicesStarted && networkUp())
    {
        serviceDiscovery();
#ifdef SMARTLIB_ESP
        if (_otaRunning)
        {
#ifndef WIFI_NONE
            if (ethOrWiFi)
                ArduinoOTA.handle();
            else
#endif
                ethOtaHandle();
        }
        serviceTime();
#endif
    }

    serviceIdentify();

#if defined(ESP8266) || defined(ESP32)
    if (ethOrWiFi && WiFi.isConnected())
    {
        int32_t rssi = WiFi.RSSI();

        // weaker than about usable/stable range
        if (rssi < -75)
        {
            uint32_t now = millis();

            if (now - _lastWeakWiFiLog >= 5000)
            {
                ESP_LOGD("WiFi", "Weak signal detected: %d dBm", rssi);
                _lastWeakWiFiLog = now;
            }
        }
    }
#endif
}

char *SmartLib::getRxTopic(const char *topic)
{
    if ((size_t)snprintf(_topic, sizeof(_topic), "%s/RX/%s", _deviceName, topic) >= (size_t)sizeof(_topic))
    {
        ESP_LOGE("SNPRINTF", "_topic too long");
        return nullptr;
    }

    return _topic;
}

const char *SmartLib::rxSubTopic(const char *fullTopic) const
{
    size_t nameLen = strlen(_deviceName);

    if (strncmp(fullTopic, _deviceName, nameLen) != 0)
        return nullptr;
    if (strncmp(fullTopic + nameLen, "/RX/", 4) != 0)
        return nullptr;

    const char *sub = fullTopic + nameLen + 4;
    return (*sub == '\0') ? nullptr : sub;
}

void SmartLib::setStringSafe(char *var, size_t size, const char *to)
{
    strncpy(var, to, size - 1);
    var[size - 1] = '\0';
}
void SmartLib::mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
    ESP_LOGD("MQTT", "Received topic: %s; Payload: %.*s; len: %u", topic, length, (char *)payload, length);
    if (_mqttCallback != nullptr)
        _mqttCallback(topic, payload, length);

    if (_instance != nullptr)
        _instance->dispatch(topic, payload, length);
}
