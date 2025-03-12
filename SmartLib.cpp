#include "SmartLib.h"

#include "esp_log_compat.h"

#include <stdarg.h>

WiFiClient SmartLib::_mqttClient;

SmartLib::SmartLib(const char *deviceName, const char *SSID, const char *PASS, const char *MQTT_SRV,
                   const char *MQTT_NAME, const char *MQTT_PASS, int8_t ACT_LED, bool ACT_HIGH)
    : client(_mqttClient)
{
    setStringSafe(_deviceName, sizeof(_deviceName), deviceName);
    setStringSafe(_SSID, sizeof(_SSID), SSID);
    setStringSafe(_PASS, sizeof(_PASS), PASS);
    setStringSafe(_MQTT_NAME, sizeof(_MQTT_NAME), MQTT_NAME);
    setStringSafe(_MQTT_PASS, sizeof(_MQTT_PASS), MQTT_PASS);

    if (ACT_LED != -1)
    {
        pinMode(ACT_LED, OUTPUT);
        digitalWrite(ACT_LED, !ACT_HIGH);
        _ACT_LED = ACT_LED;
        _ACT_HIGH = ACT_HIGH;
    }

    client.setServer(MQTT_SRV, 1883);
}

void SmartLib::maintainConnection()
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
    if (!client.connected())
    {
        setAct(true);
        String client_id = "esp32-client-";
        client_id += String(WiFi.macAddress());
        ESP_LOGD("MQTT", "Connecting");

        if (!client.connect(client_id.c_str(), _MQTT_NAME, _MQTT_PASS))
        {
            ESP_LOGW("MQTT", "Connection failed: %d", client.state());
            delay(500);
        }
        if (client.state() == -4)
        {
            ESP_LOGW("MQTT", "Bad username or password");
            delay(30000);
        }
        if (client.connected())
        {
            ESP_LOGD("MQTT", "Connected");
            if (snprintf(_topic, sizeof(_topic), "%s/RX/#", _deviceName) >= sizeof(_topic))
            {
                ESP_LOGE("SNPRINTF", "_topic too long");
                setAct(false);
                return;
            }

            ESP_LOGD("MQTT", "Subscribed to: %s", _topic);
            client.subscribe(_topic);
        }
        setAct(false);
    }
}
bool SmartLib::getStatus()
{
    return WiFi.isConnected() && client.connected();
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

void SmartLib::setMQTTCallback(MQTT_CALLBACK_SIGNATURE)
{
    client.setCallback(callback);
}

void SmartLib::sendToMQTT(const char *topic, const char *fmt, ...)
{
    if (snprintf(_topic, sizeof(_topic), "%s/TX/%s", _deviceName, topic) >= sizeof(_topic))
    {
        ESP_LOGE("SNPRINTF", "_topic too long");
        return;
    }

    char payload[256];

    va_list args;
    va_start(args, fmt);
    if (vsnprintf(payload, sizeof(payload), fmt, args) >= sizeof(payload))
    {
        va_end(args);
        ESP_LOGE("VSNPRINTF", "payload too long");
        return;
    }

    setAct(true);
    client.publish(_topic, payload);
    setAct(false);
}
void SmartLib::sendToMQTTStr(const char *topic, const char *payload)
{
    if (snprintf(_topic, sizeof(_topic), "%s/TX/%s", _deviceName, topic) >= sizeof(_topic))
    {
        ESP_LOGE("SNPRINTF", "_topic too long");
        return;
    }
    
    setAct(true);
    client.publish(_topic, payload);
    setAct(false);
}

void SmartLib::loop()
{
    client.loop();
    maintainConnection();
}

char *SmartLib::getRxTopic(const char *topic)
{
    if (snprintf(_topic, sizeof(_topic), "%s/RX/%s", _deviceName, topic) >= sizeof(_topic))
    {
        ESP_LOGE("SNPRINTF", "_topic too long");
        return nullptr;
    }

    return _topic;
}

void SmartLib::setStringSafe(char *var, size_t size, const char *to)
{
    strncpy(var, to, size - 1);
    var[size - 1] = '\0';
}