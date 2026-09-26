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
    Smart.setOTAPassword("ota-secret");       // OTA stays off without one
    Smart.setFirmwareVersion("1.0.0");        // shown by smartscan and in /TX/Info
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

## Examples

| | |
|---|---|
| [Relay](examples/Relay/Relay.ino) | WiFi relay with a local button: retained state, republish on reconnect, safe state before OTA |
| [Sensor](examples/Sensor/Sensor.ino) | Timestamped readings (NTP), a period settable over MQTT, diagnostics |
| [EthernetW5500](examples/EthernetW5500/EthernetW5500.ino) | The same over a W5500 on an ESP32-S2: SPI pins, MAC, active-low LED |
| [Listener](examples/Listener/Listener.ino) | Following another device (`onRaw`), `+`/`#` wildcards, `rxSubTopic` |

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

## Finding devices

Every device calls itself by its name on the network: the device name,
reduced to letters, digits and `-` (`"Lora Gateway"` -> `Lora-Gateway`), is
used as

- the DHCP hostname, so the router's lease list shows `KitchenLight` instead
  of `esp32c3-CCC62C` (Ethernet too, via the Ethernet fork below),
- the mDNS name on WiFi: `KitchenLight.local`,
- the MQTT client id: `KitchenLight-CCC62C` (broker logs).

Override with `setHostname("...")` before `begin()`.

`tools/smartscan.py` lists every SmartLib device in an address range. Each
device answers a probe on UDP 3234 with its name, IP, MAC, chip, firmware,
uptime, RSSI and MQTT/OTA/NTP state:

```
tools/smartscan.py                  # local subnets, interactive
tools/smartscan.py 192.168.1.0/24   # or 192.168.1.20-80, or 10.0.0.5-10.0.1.40
tools/smartscan.py --once           # plain table; --json for scripts
```

In the list, `i` blinks the selected device's activity LED so you can find it
physically, and Enter shows the details and the OTA upload settings. More in
[tools/README.md](tools/README.md). The same
blink is `<device>/RX/Identify` (payload: seconds, default 10, `0` stops).

The same information is published retained on `<device>/TX/Info` on every
connect. `setFirmwareVersion("1.2.0")` adds your version to it; the `md5` field
tells builds apart even when the version doesn't change.

## OTA

On by default on ESP32 / S2 / S3 / C3 / ESP8266, over WiFi (ArduinoOTA) and
over W5x00 Ethernet (the same espota protocol, implemented by SmartLib). **A
password is required**: without one OTA stays off and says so in the log.

```cpp
Smart.setOTAPassword(OTA_PASSWORD);   // or build_flags = -DSMARTLIB_OTA_PASSWORD=\"...\"
Smart.allowOTAWithoutPassword();      // testing only
Smart.disableOTA();
Smart.setOTAStartCallback(safeState); // relays off etc. before flashing starts
```

Then upload with `upload_protocol = espota` and `upload_port = <hostname>.local`
(the IP on Ethernet). **[docs/OTA.md](docs/OTA.md)** covers the whole thing:
the first USB flash, PlatformIO / Arduino IDE / espota setups, the firewall
port the device connects back to, and troubleshooting.

Just before flashing, the device publishes `offline` on its availability topic
and disconnects from MQTT; a failed update reconnects. The partition table
needs two app slots (the default one has them; `min_spiffs.csv` if the
firmware is big).

## Time

NTP starts by itself once the network is up, using the NTP server the DHCP
server offers (option 42), or `pool.ntp.org` if it offers none. On WiFi lwIP
also picks up a changed server on lease renewal. Local time defaults to
Europe/Prague.

```cpp
Smart.setTimezone("UTC0");            // POSIX TZ, before begin()
Smart.setNTPServer("cz.pool.ntp.org"); // fallback when DHCP has none
Smart.disableNTP();

time(nullptr);                        // UTC epoch, as usual
SmartLib::timeValid();                // clock set?
char now[32];
SmartLib::timeString(now, sizeof(now)); // "2026-09-27T14:03:11+0200"
```

`-DSMARTLIB_DEFAULT_TZ=...` / `-DSMARTLIB_DEFAULT_NTP=...` change the defaults
for a whole project.

## Ethernet

SmartLib depends on [a fork of the Ethernet library](https://github.com/Vratk0529/Ethernet)
that fixes the send/flush loops that hang forever when the cable is pulled or
the peer disappears, sends the DHCP hostname and asks DHCP for an NTP server.
Remove `arduino-libraries/Ethernet` from a project's `lib_deps` (and any
`patch_ethernet.py`), otherwise the build warns that the upstream library was
picked.

Ethernet uses more W5x00 sockets now: MQTT, OTA and discovery stay open, and
NTP, DHCP and DNS open one briefly. That's fine on a W5500 (8 sockets); on a
W5100 (4) call `disableDiscovery()` if the sketch opens sockets of its own.

## Upgrading from 1.0

- Set an OTA password, or OTA stays off.
- The MQTT client id changed from `esp32-client-<MAC>` to `<hostname>-<MAC tail>`.
- `<device>/TX/Info` is new, and `<device>/RX/Identify` is now handled by
  SmartLib instead of reaching your `on("#")` handler.
- Drop your own NTP / ArduinoOTA code, and `arduino-libraries/Ethernet`.
