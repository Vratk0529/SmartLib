# smartscan: find SmartLib devices

A terminal app that lists every SmartLib device in an address range, with
what it is and how it's doing:

```
 SmartLib devices: 3  |  192.168.1.0/24  |  scanned 4s ago
NAME                 IP              HOST               LINK     RSSI  UPTIME  MQTT OTA  FW         CHIP           MAC
Gateway              192.168.1.40    Gateway            ethernet       12h31m  yes  yes  2.1        ESP32-S2 rev0  02:53:4C:00:00:01
Kitchen Light        192.168.1.31    Kitchen-Light      wifi     -58   3d04h   yes  yes  1.0.0      ESP32-C3 rev4  34:85:18:00:00:2C
Sensor               192.168.1.57    Sensor             wifi     -71   0m53s   yes  yes  1.2.0      ESP32-S3 rev0  F4:12:FA:00:00:70
 j/k select  Enter details  i identify  r rescan  s sort:name  q quit
```

Python 3 standard library only (curses), Linux.

## Running it

```sh
tools/smartscan.py                    # every local IPv4 subnet (from `ip addr`)
tools/smartscan.py 192.168.1.0/24     # a subnet
tools/smartscan.py 192.168.1.20-80    # a range within one /24
tools/smartscan.py 10.0.0.5-10.0.1.40 # a range across subnets
tools/smartscan.py 192.168.1.0/24 10.10.0.0/22    # several
```

To have it on your `PATH`:

```sh
ln -s "$PWD/tools/smartscan.py" ~/.local/bin/smartscan
```

Ranges are limited to 4096 addresses. Local subnets larger than a /20 (VPNs,
Docker's /16) are skipped when scanning automatically; name them explicitly
if you want them.

### Keys

| Key | |
|---|---|
| `j`/`k`, arrows | select |
| Enter | details: every field the device reports, plus the OTA `upload_port` to use (Esc goes back) |
| `i` | identify: the selected device blinks its activity LED for 10 s (`--identify N` to change) |
| `r` | rescan now (it also rescans every 15 s, `--interval`) |
| `s` | sort by name / IP / uptime / RSSI |
| `q` | quit |

A device that stops answering stays in the list, dimmed, so a device that
just went offline is easy to spot.

### Without the UI

```sh
tools/smartscan.py --once 192.168.1.0/24          # plain table
tools/smartscan.py --json 192.168.1.0/24 | jq ... # for scripts
```

Output that isn't a terminal (a pipe, a file) gets the plain table too.

## Columns

| Column | |
|---|---|
| NAME | device name, the one in the MQTT topics |
| HOST | network name: DHCP hostname and, on WiFi, `<HOST>.local` |
| LINK | `wifi` or `ethernet` |
| RSSI | WiFi signal in dBm (above -67 is good, below -75 is trouble) |
| UPTIME | since the last reboot. A short uptime on a device you didn't touch means it is crashing or losing power (the `reset` field in the details says why) |
| MQTT | connected to the broker right now |
| OTA | accepting updates (no = no password set, or disabled) |
| FW | `setFirmwareVersion()`; the details also show `md5`, which is different for every build |
| NTP | the time server in use: an IP means DHCP offered it |

## How it works

Each SmartLib device listens on UDP port 3234. smartscan sends `SMARTLIB?` to
every address in the range, twice (a sleeping WiFi station can miss the
first), plus the subnet's broadcast address. Each device answers with a JSON
object, the same as its retained `<device>/TX/Info` topic plus live values
(uptime, RSSI, MQTT, heap, time). `SMARTLIB IDENTIFY <seconds>` makes it blink.

The scan never touches devices that aren't SmartLib; they just don't answer.

## When a device doesn't show up

- **Other VLAN / subnet:** broadcasts stop at the router, but the unicast
  scan works as long as the device network is routed to you and the router's
  firewall lets UDP 3234 through. The answers come back to a random high port
  on your computer, which a stateful firewall allows.
- **Firmware without discovery:** SmartLib older than 1.1, or
  `disableDiscovery()` was called.
- **Not on the network:** check the router's DHCP lease list for the HOST
  name.
