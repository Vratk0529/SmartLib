#include "SmartLib.h"

#include "esp_log_compat.h"

#include <stdarg.h>

#ifndef WIFI_NONE
WiFiClient SmartLib::_wifiClient;
#endif
EthernetClient SmartLib::_ethClient;

void (*SmartLib::_mqttCallback)(char *, uint8_t *, unsigned int) = nullptr;
SmartLib *SmartLib::_instance = nullptr;

void SmartLib::commonInit(const char *deviceName, const char *MQTT_NAME, const char *MQTT_PASS, int8_t ACT_LED,
                          bool ACT_HIGH)
{
    _instance = this;

    setStringSafe(_deviceName, sizeof(_deviceName), deviceName);
    setStringSafe(_MQTT_NAME, sizeof(_MQTT_NAME), MQTT_NAME);
    setStringSafe(_MQTT_PASS, sizeof(_MQTT_PASS), MQTT_PASS);

    if (ACT_LED != -1)
    {
        pinMode(ACT_LED, OUTPUT);
        digitalWrite(ACT_LED, !ACT_HIGH);
        _ACT_LED = ACT_LED;
        _ACT_HIGH = ACT_HIGH;
    }

    setAvailability("Availability");

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
#ifndef WIFI_NONE
    if (ethOrWiFi)
    {
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
    if (!client.connected())
    {
        // Reconnecting is slow and blocking, so do not hammer it from every
        // loop() iteration.
        if (_lastMqttAttempt != 0 && millis() - _lastMqttAttempt < 2000)
            return;
        _lastMqttAttempt = millis();

        setAct(true);
#ifndef WIFI_NONE
        String client_id = "esp32-client-";
        client_id += String(WiFi.macAddress());
#else
        String client_id = "eth-client-";
        client_id += String(_macAddress[4], HEX);
        client_id += String(_macAddress[5], HEX);
#endif
        ESP_LOGD("MQTT", "Connecting");

        bool connected;
        if (_availEnabled)
            // Last will: the broker publishes "offline" here if we vanish
            // without a clean disconnect.
            connected = client.connect(client_id.c_str(), _MQTT_NAME, _MQTT_PASS, _availTopic, 0, true,
                                       _availOffline);
        else
            connected = client.connect(client_id.c_str(), _MQTT_NAME, _MQTT_PASS);

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
    if (_handlerCount == 0)
        return;

    // Both arguments point into PubSubClient's single receive buffer, which a
    // publish() from inside a handler overwrites -- so take a copy first.
    char fullTopic[128];
    setStringSafe(fullTopic, sizeof(fullTopic), topic);
    String message((const char *)payload, length);

    const char *sub = rxSubTopic(fullTopic);

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