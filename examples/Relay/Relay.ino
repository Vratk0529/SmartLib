// A WiFi relay with a local push button -- the typical SmartLib device.
//
//   Relay/TX/Relay          "on" / "off", retained, whenever it changes
//   Relay/RX/Relay          "on" / "off" / "toggle" to switch it
//   Relay/TX/Availability   "online" / "offline" (last will)
//   Relay/TX/Info           hostname, IP, firmware... (retained)
//
// The button keeps working without WiFi or the broker; the state is
// published once the connection comes back.
//
// First flash over USB. After that, OTA (see docs/OTA.md):
//   upload_protocol = espota
//   upload_port = Relay.local
//   upload_flags = --auth=change-me

#include <SmartLib.h>

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
#define MQTT_BROKER "192.168.1.100"
#define MQTT_USER ""
#define MQTT_PASS ""
#define OTA_PASSWORD "change-me"

#define FW_VERSION "1.0.0"

#define RELAY_PIN 5
#define BUTTON_PIN 9 // to GND, internal pull-up
#define LED_PIN 8    // activity LED: blinks on traffic and for Identify

SmartLib Smart("Relay", WIFI_SSID, WIFI_PASS, MQTT_BROKER, MQTT_USER, MQTT_PASS, LED_PIN);

static bool relayOn = false;

static void publishRelay()
{
    // Retained, so a dashboard that connects later still sees the state.
    Smart.sendToMQTTStr("Relay", relayOn ? "on" : "off", true);
}

static void setRelay(bool on)
{
    relayOn = on;
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
    if (Smart.getStatus())
        publishRelay();
}

// Relay/RX/Relay
static void onRelay(const char *topic, const String &payload)
{
    (void)topic;
    if (payload.equalsIgnoreCase("toggle"))
        setRelay(!relayOn);
    else
        setRelay(SmartLib::isOn(payload));
}

// After every (re)connect: the broker may have restarted and lost
// retained messages, and changes made offline were not published.
static void onConnected()
{
    publishRelay();
}

// Right before an OTA update starts writing flash: leave the load in a
// known state rather than whatever it is when the device reboots.
static void onOTAStart()
{
    setRelay(false);
}

static void serviceButton()
{
    static bool lastPressed = false;
    static uint32_t lastChange = 0;

    bool pressed = digitalRead(BUTTON_PIN) == LOW;
    if (pressed != lastPressed && millis() - lastChange > 30) // debounce
    {
        lastChange = millis();
        lastPressed = pressed;
        if (pressed)
            setRelay(!relayOn);
    }
}

void setup()
{
    Serial.begin(115200);

    pinMode(RELAY_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    setRelay(false);

    Smart.setFirmwareVersion(FW_VERSION);
    Smart.setOTAPassword(OTA_PASSWORD);
    Smart.setOTAStartCallback(onOTAStart);
    Smart.setConnectedCallback(onConnected);
    Smart.on("Relay", onRelay);
    Smart.begin();
}

void loop()
{
    Smart.loop();
    serviceButton();

    static uint32_t lastDiag = 0;
    if (Smart.getStatus() && SmartLib::every(lastDiag, 60000))
        Smart.publishDiagnostics();
}
