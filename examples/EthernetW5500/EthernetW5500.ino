// SmartLib over a W5500 (or W5100/W5200) Ethernet module on an ESP32.
//
// Everything works as on WiFi -- MQTT, OTA, NTP, discovery -- except mDNS,
// so OTA and smartscan reach the device by IP address:
//   upload_protocol = espota
//   upload_port = 192.168.1.50
//   upload_flags = --auth=change-me
//
// The DHCP lease shows up in the router as "Gateway" (the fork of the
// Ethernet library SmartLib uses sends the hostname).
//
// Pins below are for an ESP32-S2 board; any SPI pins work.

#include <SmartLib.h>

#define MQTT_BROKER "192.168.1.100"
#define MQTT_USER ""
#define MQTT_PASS ""
#define OTA_PASSWORD "change-me"

#define PIN_MISO 41
#define PIN_MOSI 40
#define PIN_SCK 39
#define ETH_CS 2
#define ETH_RST 3 // -1 if not wired
#define ACT_LED 42

// Any locally administered MAC (second-lowest bit of the first byte set),
// unique on your network. W5x00 modules have none of their own.
static uint8_t mac[6] = {0x02, 0x53, 0x4C, 0x00, 0x00, 0x01};

// The last argument says the LED is active low.
SmartLib Smart("Gateway", mac, ETH_CS, ETH_RST, MQTT_BROKER, MQTT_USER, MQTT_PASS, ACT_LED, false);

static void onCommand(const char *topic, const String &payload)
{
    Serial.printf("%s: %s\n", topic, payload.c_str());
    Smart.replyTo(topic, "ok", false);
}

void setup()
{
    Serial.begin(115200);

    // SmartLib does not know your SPI pins; start the bus before begin().
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);

    Smart.setOTAPassword(OTA_PASSWORD);
    Smart.on("Command/#", onCommand); // Gateway/RX/Command/anything
    Smart.begin();
}

void loop()
{
    Smart.loop();

    static uint32_t lastDiag = 0;
    if (Smart.getStatus() && SmartLib::every(lastDiag, 60000))
    {
        Smart.publishDiagnostics();

        char now[32];
        if (SmartLib::timeString(now, sizeof(now), "%H:%M:%S"))
            Serial.printf("%s, NTP from %s\n", now, Smart.ntpServer());
    }
}
