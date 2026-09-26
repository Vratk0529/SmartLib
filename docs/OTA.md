# Flashing over the network (OTA)

SmartLib devices on ESP32, ESP32-S2/S3/C3 and ESP8266 accept firmware over
the network, on WiFi and on W5x00 Ethernet alike. Uploads use `espota`, the
same tool the Arduino IDE and PlatformIO use for ArduinoOTA, so nothing extra
needs installing.

## 1. Once, over USB

OTA only exists in firmware that has it, so the first flash is over the cable
as usual. That firmware must:

- **set an OTA password.** Without one OTA stays off (the log says
  `[OTA] No password set, OTA is off`):

  ```cpp
  Smart.setOTAPassword(OTA_PASSWORD); // before Smart.begin()
  ```

  or for every device in a project, in `platformio.ini`:

  ```ini
  build_flags = -DSMARTLIB_OTA_PASSWORD=\"${sysenv.OTA_PASSWORD}\"
  ```

  `allowOTAWithoutPassword()` switches the check off, for a bench setup
  only: anyone on the network could then flash the device.

- **have room for two firmware images.** The update is written next to the
  running one and only swapped in after it checks out. The default partition
  table of ESP32 boards already has two app slots of 1.25 MB. If the firmware
  is bigger, use `board_build.partitions = min_spiffs.csv` (two 1.9 MB
  slots). The partition table itself can only be changed over USB.

After it boots, the log shows where it listens:

```
[OTA] Ready at KitchenLight.local:3232     (WiFi)
[OTA] Ready at 192.168.1.50:3232           (Ethernet)
```

## 2. Find the device

- WiFi devices have an mDNS name: the device name with anything but letters,
  digits and `-` removed. `"Kitchen Light"` is `Kitchen-Light.local`.
- Ethernet devices have no mDNS, so use the IP address.
- `tools/smartscan.py` lists both (see [tools/README.md](../tools/README.md)),
  and its detail view (Enter) prints the `upload_port` to use.

## 3. Upload

### PlatformIO

Add an environment next to the USB one:

```ini
[env:esp32c3]
platform = espressif32@6.10.0
board = esp32-c3-devkitm-1
framework = arduino
lib_deps = https://github.com/Vratk0529/SmartLib.git

[env:ota]
extends = env:esp32c3
upload_protocol = espota
upload_port = KitchenLight.local            ; or the IP
upload_flags =
    --auth=${sysenv.OTA_PASSWORD}
    --host_port=40000                       ; see "Firewall" below
```

```sh
export OTA_PASSWORD=...          # or write it into upload_flags directly
pio run -e ota -t upload
pio run -e ota -t upload --upload-port 192.168.1.57   # a different device
```

`${sysenv.OTA_PASSWORD}` reads an environment variable, which keeps the
password out of the repository. A literal `--auth=secret` works too.

### Arduino IDE

WiFi devices appear under *Tools > Port > Network ports* (via mDNS). Pick
one, upload as usual, and the IDE asks for the password. Ethernet devices
don't appear there; use espota directly.

### espota directly

```sh
python3 ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py \
    -i 192.168.1.50 -a <password> -P 40000 -f .pio/build/esp32c3/firmware.bin -d -r
```

`-i` is the device, `-P` the local port the device connects back to, `-d -r`
show progress. On ESP8266 add `-p 8266`.

## What happens during an update

1. espota sends an invitation to UDP 3232 (8266 on ESP8266), and the device
   answers with a challenge for the password.
2. The device runs the `setOTAStartCallback()` callback, if any. This is the
   place to switch relays off, stop motors, and so on.
3. It publishes `offline` on `<device>/TX/Availability` and disconnects from
   MQTT. Nothing else in `loop()` runs until the update is over.
4. It connects back to the computer over TCP and receives the image (a 1 MB
   image takes 5-10 s on WiFi), logging every 10 %.
5. It checks the image's MD5. If it matches, the device reboots into the new
   firmware, which comes back `online` with the new version in
   `<device>/TX/Info`.

If anything fails (wrong password, dropped connection, bad image), the old
firmware keeps running. It reconnects to MQTT and reports `online` again.

## Firewall

In step 4 **the device opens the connection to your computer**, on the port
given by `--host_port` (random if not set). A firewall that drops incoming
connections makes the upload hang at 0 % and then fail with
`No response from device`. Fix the port and open it for your device network:

```sh
# firewalld
sudo firewall-cmd --add-port=40000/tcp            # add --permanent to keep it
# ufw
sudo ufw allow from 192.168.1.0/24 to any port 40000 proto tcp
# nftables / iptables: accept tcp dport 40000 from the device subnet
```

The device must also be able to reach the computer's address at all. With
VLANs, the address it connects back to is the one the invitation came from,
so run the upload from a machine that is routable from the device network.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `No response from the ESP` right away | Wrong address; OTA off (no password, see the device log); UDP 3232 blocked between you and the device |
| `Authentication Failed` | Wrong `--auth` |
| Hangs at `Uploading: 0%`, then fails | The device can't connect back: firewall on your computer, or no route from the device network (see above) |
| `Not Enough Space` / `Cannot start` in the device log | The image doesn't fit the OTA slot: bigger partitions (over USB) |
| `image rejected` / MD5 error | Transfer corrupted or cut short; just retry |
| Uploaded, but the old version runs | You uploaded the wrong environment, or the new firmware reset before reaching `setup()` (e.g. hung in a global constructor) and the bootloader rolled back. Check `fw`/`md5` in smartscan or `<device>/TX/Info` |
| New firmware crashes in `setup()`/`loop()` | No rollback that late: the Arduino core marks the image good before `setup()`. Flash a fixed build over OTA if it stays up long enough, otherwise over USB |
| Ethernet device not found by name | No mDNS over Ethernet: use the IP |
