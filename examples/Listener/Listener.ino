// Reacting to other devices, and topics with wildcards.
//
// A hallway light that turns on when the door sensor opens, and a bank of
// four outputs addressed as Hallway/RX/Output/1 .. /4.
//
//   onRaw("Door/TX/State")   an absolute topic: another device's output
//   on("Output/+")           '+' matches one level of our own RX topics
//   on("#")                  everything else under Hallway/RX/, for logging

#include <SmartLib.h>

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
#define MQTT_BROKER "192.168.1.100"
#define MQTT_USER ""
#define MQTT_PASS ""
#define OTA_PASSWORD "change-me"

#define LIGHT_PIN 4
static const uint8_t OUTPUT_PINS[] = {5, 6, 7, 10};
static const size_t OUTPUT_COUNT = sizeof(OUTPUT_PINS) / sizeof(OUTPUT_PINS[0]);

SmartLib Smart("Hallway", WIFI_SSID, WIFI_PASS, MQTT_BROKER, MQTT_USER, MQTT_PASS);

static uint32_t lightOffAt = 0;

// Door/TX/State: "open" or "closed", published by another SmartLib device.
static void onDoor(const char *topic, const String &payload)
{
    (void)topic;
    if (SmartLib::isOn(payload)) // "open" counts as on
    {
        digitalWrite(LIGHT_PIN, HIGH);
        lightOffAt = millis() + 120000;
        if (lightOffAt == 0)
            lightOffAt = 1;
        Smart.sendToMQTTStr("Light", "on", true);
    }
}

// Hallway/RX/Output/<n>
static void onOutput(const char *topic, const String &payload)
{
    const char *sub = Smart.rxSubTopic(topic); // "Output/3"
    int n = atoi(sub + strlen("Output/"));
    if (n < 1 || n > (int)OUTPUT_COUNT)
        return;

    bool on = SmartLib::isOn(payload);
    digitalWrite(OUTPUT_PINS[n - 1], on);
    Smart.replyTo(topic, on ? "on" : "off"); // -> Hallway/TX/Output/<n>
}

// Handlers are not exclusive: this one sees every RX message, including
// the ones handled above.
static void onAnything(const char *topic, const String &payload)
{
    Serial.printf("[rx] %s = %s\n", Smart.rxSubTopic(topic), payload.c_str());
}

void setup()
{
    Serial.begin(115200);

    pinMode(LIGHT_PIN, OUTPUT);
    for (uint8_t pin : OUTPUT_PINS)
        pinMode(pin, OUTPUT);

    Smart.setOTAPassword(OTA_PASSWORD);
    Smart.onRaw("Door/TX/State", onDoor);
    Smart.on("Output/+", onOutput);
    Smart.on("#", onAnything);
    Smart.begin();
}

void loop()
{
    Smart.loop();

    if (lightOffAt != 0 && (int32_t)(millis() - lightOffAt) >= 0)
    {
        lightOffAt = 0;
        digitalWrite(LIGHT_PIN, LOW);
        Smart.sendToMQTTStr("Light", "off", true);
    }
}
