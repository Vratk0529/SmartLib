// A sensor that publishes timestamped readings.
//
//   Sensor/TX/Temperature   {"t":23.4,"ts":1790000000,"time":"2026-09-27T14:03:11+0200"}
//   Sensor/TX/Diagnostics   uptime, heap, IP, RSSI (every minute)
//   Sensor/RX/Period        reporting period in seconds (retain it on the
//                           broker to make it stick across reboots)
//
// The clock comes from NTP (the server DHCP offers, else pool.ntp.org), so
// readings are only published once the time is known. Swap readSensor()
// for your real sensor; the chip's internal temperature is a stand-in.

#include <SmartLib.h>

#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
#define MQTT_BROKER "192.168.1.100"
#define MQTT_USER ""
#define MQTT_PASS ""
#define OTA_PASSWORD "change-me"

SmartLib Smart("Sensor", WIFI_SSID, WIFI_PASS, MQTT_BROKER, MQTT_USER, MQTT_PASS);

static uint32_t periodMs = 30000;

static float readSensor()
{
    return temperatureRead();
}

static void publishReading()
{
    char iso[32];
    if (!SmartLib::timeString(iso, sizeof(iso)))
        return; // no wall clock yet: a reading without a time is not much use

    Smart.sendToMQTT("Temperature", "{\"t\":%.1f,\"ts\":%lu,\"time\":\"%s\"}", readSensor(),
                     (unsigned long)time(nullptr), iso);
}

// Sensor/RX/Period
static void onPeriod(const char *topic, const String &payload)
{
    long seconds = payload.toInt();
    if (seconds < 1 || seconds > 86400)
    {
        Smart.replyTo(topic, "invalid"); // -> Sensor/TX/Period
        return;
    }
    periodMs = (uint32_t)seconds * 1000;
    Smart.replyTo(topic, payload.c_str());
}

void setup()
{
    Serial.begin(115200);

    // Europe/Prague is the default; any POSIX TZ string works.
    // Smart.setTimezone("GMT0BST,M3.5.0/1,M10.5.0");
    // Smart.setNTPServer("cz.pool.ntp.org"); // used when DHCP offers none

    Smart.setOTAPassword(OTA_PASSWORD);
    Smart.on("Period", onPeriod);
    Smart.begin();
}

void loop()
{
    Smart.loop();

    static uint32_t lastReading = 0;
    if (Smart.getStatus() && SmartLib::every(lastReading, periodMs))
        publishReading();

    static uint32_t lastDiag = 0;
    if (Smart.getStatus() && SmartLib::every(lastDiag, 60000))
        Smart.publishDiagnostics();
}
