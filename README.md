# SmartLib

WiFi/Ethernet + MQTT plumbing for custom smart devices. Topics are always

```
<deviceName>/TX/<subtopic>   device -> broker
<deviceName>/RX/<subtopic>   broker -> device   (subscribed automatically)
```

## A whole device

```cpp
#include <SmartLib.h>

#define RELAY 5

SmartLib Smart("KitchenLight", "ssid", "pass", "192.168.1.100", "user", "pass", LED_BUILTIN);

void publishState()
{
    Smart.sendToMQTTStr("Relay", digitalRead(RELAY) ? "on" : "off", true); // retained
}

void onRelay(const char *topic, const String &payload)
{
    digitalWrite(RELAY, SmartLib::isOn(payload));
    Smart.replyTo(topic, digitalRead(RELAY) ? "on" : "off"); // -> KitchenLight/TX/Relay
}

void setup()
{
    pinMode(RELAY, OUTPUT);

    Smart.on("Relay", onRelay);              // KitchenLight/RX/Relay
    Smart.setConnectedCallback(publishState); // re-announce after every reconnect
    Smart.begin();
}

void loop()
{
    Smart.loop();

    static uint32_t lastDiag = 0;
    if (Smart.getStatus() && SmartLib::every(lastDiag, 60000))
        Smart.publishDiagnostics();
}
```

## Routing

- `on(filter, handler)` — filter is relative to `<device>/RX/`, MQTT wildcards
  work: `on("Relay/+", ...)` catches `Relay/1`, `on("#", ...)` catches everything.
- `onRaw(filter, handler)` — absolute topic anywhere on the broker, e.g.
  `onRaw("Thermostat/TX/Temperature", ...)`. Resubscribed on every reconnect.
- `setMQTTCallback(cb)` — the old raw callback, still there, runs before routing.
- Filters are stored by pointer, so pass string literals or globals.

## Availability

On by default: `<device>/TX/Availability` is published `online` (retained) on
connect, and the broker publishes `offline` there via the last will if the
device drops off — that's what Home Assistant's `availability_topic` wants.
Change with `setAvailability("Status/Online", "1", "0")` or turn it off with
`disableAvailability()`, before `begin()`.

## Helpers

| | |
|---|---|
| `sendToMQTTStr(t, p, retain)` | publish a string |
| `sendToMQTT(t, fmt, ...)` / `sendToMQTTRetained` | printf-style publish |
| `replyTo(rxTopic, payload)` | answer on the TX topic mirroring an RX topic |
| `publishDiagnostics()` | uptime / heap / IP / RSSI as JSON |
| `rxSubTopic(topic)` | strip `<device>/RX/` off a received topic |
| `getIP()`, `getRSSI()`, `deviceName()`, `getStatus()` | state |
| `SmartLib::isOn(payload)` | `1`/`on`/`true`/`yes`/`open` -> true |
| `SmartLib::every(last, ms)` | non-blocking interval |
| `SmartLib::topicMatches(filter, topic)` | MQTT wildcard match |

`loop()` keeps WiFi/Ethernet and MQTT connected on its own; reconnects are
rate-limited and DHCP is retried if it failed at boot, so nothing blocks the
sketch for long. One SmartLib instance per sketch.
