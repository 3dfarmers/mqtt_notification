# CLAUDE.md

Guidance for Claude Code when helping someone set up or change this project.
The person is usually a maker who saw it on YouTube: explain in plain words,
do the technical work for them, and ask only for what you cannot find out.

## What this is

One Arduino sketch, `mqtt_notification/mqtt_notification.ino`, for any ESP32 or
ESP32-S3. It reads a Bambu Lab printer's state over local MQTT, shows it on
Philips Hue lights and sends an ntfy push on finish and on a real failure (with
one camera photo on A1 / A1 mini / P1P / P1S). README.md has the user-facing
description and the state -> light -> push table.

```
mqtt_notification/mqtt_notification.ino   the firmware (single file)
tools/console.py                          send serial commands, read the log
README.md                                 for people
LICENSE                                   MIT
```

## Rules

- **Never write the user's secrets into a file in this repo**, and never commit
  them: WiFi password, printer access code, Hue key, ntfy topic. Settings live
  on the board (NVS) and are set over serial with `set <key> <value>`. If you
  add a setting, add it to `KEYS[]` (`secret = true` for anything private),
  not as a constant.
- Do not repeat secrets back in chat. `show` masks them; `tools/console.py`
  masks them when it echoes a command.
- Keep it one sketch that compiles for both `esp32:esp32:esp32` and
  `esp32:esp32:esp32s3`. Compile both before saying a change is done.
- Ask before flashing a board: it replaces whatever firmware is on it.

## Toolchain

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32          # core 3.x
arduino-cli lib install PubSubClient
arduino-cli board list                        # find the port

# ESP32-S3 on its native USB port (most S3 dev boards' "USB" connector):
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc mqtt_notification
arduino-cli upload  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc -p <PORT> mqtt_notification
# Classic ESP32 (an S3 on its USB-UART "COM"/"UART" connector: esp32:esp32:esp32s3):
arduino-cli compile --fqbn esp32:esp32:esp32 mqtt_notification
```

Without `CDCOnBoot=cdc`, an S3 on its native USB port prints nothing on serial.
3D Farmers FarmBoard 2 owners add
`--build-property "compiler.cpp.extra_flags=-DFARMBOARD2=1"` so the board's
motor drivers are held off.

Talking to the board (needs `pip install pyserial`):

```bash
python tools/console.py --port <PORT> show
python tools/console.py --port <PORT> "set wifi_ssid X" "set wifi_pass Y" reboot
python tools/console.py --port <PORT> --wait 35 "hue pair"
python tools/console.py --port <PORT> --listen 120      # watch the log
```

Opening the port resets a native-USB ESP32-S3 on Windows: the script waits
`--boot` seconds first. Each run is a fresh connection, so wait for
`WiFi: connected` / `[MQTT] connected` in the output before testing.

## Guided setup (when someone says "set this up for me")

Go step by step and check each result in the log before moving on.

1. **Ask** for the printer model, its IP, LAN access code and serial number
   (printer screen: network / LAN settings and device info), and the WiFi name
   and password. Tell them these only go onto the board, never into the repo.
2. **Board**: `arduino-cli board list` for the port. Chip unknown? The esptool
   that ships with the ESP32 core (`<arduino data dir>/packages/esp32/tools/esptool_py/<version>/esptool`,
   run with `--port <PORT> flash-id`) tells ESP32 from ESP32-S3. Pick the FQBN
   above, compile, ask, then upload.
3. **WiFi + printer**: `set wifi_ssid`, `set wifi_pass`, `set printer_ip`,
   `set access_code`, `set serial`, `reboot`. Expect `WiFi: connected ...
   dBm` and `[MQTT] connected`, then `[PRINT] unknown -> <state>`.
   - Below about -70 dBm the Hue and push connections fail intermittently:
     have them move the board, do not try to fix it in code.
   - `[MQTT] failed` -> IP / access code / LAN mode; on newer Bambu firmware
     also Developer Mode. `no printer data` -> serial number wrong.
4. **Hue**: `hue find` (sets `hue_ip` when exactly one bridge answers). Tell
   them to press the round link button on the bridge, then run `hue pair`
   right away (it retries for 30 s). `hue lights` lists id + name; if names
   do not say which light is the one by the printer, `hue blink <id>` and ask.
   `set hue_lights <id>[,<id>]`, then `hue red` and ask whether it turned red.
5. **Phone**: suggest a random topic, e.g.
   `python -c "import secrets; print('print-' + secrets.token_urlsafe(15))"`.
   They install ntfy, subscribe to it (Android: Instant delivery, battery
   Unrestricted), then `set ntfy_topic ...` and `notify`. Ask whether it
   arrived. A1 / P1: `snap` should arrive with a photo.
6. **Dry run**: `sim PREPARE`, `sim RUNNING 40`, `sim PAUSE`, `sim FAILED`
   (wait ~65 s: flash + push), `sim CANCEL`, `sim FINISH`, `live`. Read the log
   for `[HUE] command failed` lines; the user only sees the light.
7. **Real print**: suggest a small test print; cancel it once and read
   `[PRINT] FAILED, print_error N (XXXX_XXXX): ...`. If a real cancel is
   classified as an error, add that code to `CANCEL_CODES`.

## Code map

The sketch is single-threaded; `loop()` runs, in order:
`serviceSerial()` -> `serviceMqtt()` -> copy `m*` printer fields into the
working fields (unless `sim`) -> `feed(parsePhase(...))` -> `serviceTimers()`
-> `serviceLight()` -> `servicePush()`. Light goes before push on purpose: the
light should change before the phone buzzes.

| Part | Where | Notes |
|---|---|---|
| Printer input | `onMqtt()`, `serviceMqtt()` | fills `mState`, `mPct`, `mLayer`, `mErr`, `mJob` (merged: P1/A1 send deltas) |
| State machine | `feed()`, `onTransition()`, `serviceTimers()` | transitions only; old FINISH/FAILED at boot are ignored |
| What the light shows | `showForPhase()`, `onTransition()` | colours via `L(color, brightness)` |
| Colours | `COLOR_X/Y`, `WHITE_MIREK`, `enum Color` | CIE xy for Hue gamut C |
| Timings | constants at the top | `FLASH_MS`, `FINISH_SHOW_MS`, `CANCEL_SHOW_MS`, ... |
| Cancel vs error | `SETTLE_MS`, `CANCEL_CODES`, `printErrorNow()`, late-error watch | print_error 0 or a cancel code = user cancel |
| Hue output | `huePut(const Light&)` -> `huePutOne()` | keep-alive TLS; `serviceLight()` retries with back-off |
| Push output | `queuePush()` -> `servicePush()` -> `sendText()` / `sendSnapshot()` | photo falls back to text |
| Messages | `notifyFail()`, `notifyFinish()` | title, priority, tags, text |
| Settings | `Config`, `KEYS[]`, `cmdSet()` | NVS namespace `pstatus`, key names max 15 chars |

### Common changes

- **Different colours or behaviour per state**: `showForPhase()` and the
  `PH_FINISH` / `PH_FAILED` cases in `onTransition()`. Add a colour by
  extending `enum Color`, `COLOR_NAME`, `COLOR_X`, `COLOR_Y` together.
- **Push on pause too**: call `queuePush(...)` in the `PH_PAUSE` branch of
  `onTransition()` (only when `prev` was RUNNING, or it fires on every resume).
- **Another light system**: replace the body of `huePut(const Light&)`. Keep
  its contract: return 2xx when all lights took it, 4xx when they refused for
  good (wrong id/key: no retry), anything else to have `serviceLight()` retry
  later. `Light` is `{color, bri 0..100, instant}`; `C_OFF` means off. Add the
  new connection settings to `KEYS[]`. Examples:
  - WLED: plain HTTP `POST http://<ip>/json/state` with
    `{"on":true,"bri":<0..255>,"seg":[{"col":[[r,g,b]]}]}` (use `WiFiClient`,
    add an RGB table next to `COLOR_X/Y`).
  - Home Assistant: `POST http://<ha>:8123/api/services/light/turn_on` with
    `Authorization: Bearer <token>` (secret key) and
    `{"entity_id":"light.x","rgb_color":[r,g,b],"brightness_pct":n}`.
- **Another push service**: replace `sendText()` and `sendSnapshot()` (keep
  "photo failed -> text" in `servicePush()`). Telegram `sendMessage` /
  `sendPhoto` and Pushover need multipart/form-data for the photo; stream the
  JPEG between the multipart head and tail the way `sendSnapshot()` streams it
  into the ntfy PUT, with the total `Content-Length` computed up front.
- **Another printer brand**: write a transport that fills `mState`, `mPct`,
  `mLayer`, `mErr`, `mJob` in Bambu vocabulary (`IDLE PREPARE RUNNING PAUSE
  FINISH FAILED`; user cancel = FAILED with `mErr` 0, machine error = FAILED
  with a non-zero code) and call it from `loop()` instead of `serviceMqtt()`.
  Klipper/Moonraker: poll `GET /printer/objects/query?print_stats&display_status`
  (`print_stats.state`: standby/printing/paused/complete/cancelled/error).
  PrusaLink: `GET /api/v1/status` with the API key. The camera photo is Bambu
  only; set `push.snap` false or write a new grabber.
- **Other Bambu models**: X1 / H2 / P2S work for light and push; their camera is
  RTSP only, so `sendSnapshot()` fails fast and the text goes out. Big reports
  (H2S ~18 KB) are why `MQTT_BUF` is 20 KB: never lower it below the largest
  report, PubSubClient drops bigger packets without a word.

## Pitfalls (learned on real hardware)

- **Arduino prototypes**: the builder inserts generated prototypes before the
  first function. Every `struct` / `enum` used in a function signature must be
  defined above the first function in the file, or the build fails with
  "'X' does not name a type".
- **WiFi signal** decides whether TLS works: the printer link can be fine at
  -82 dBm while every Hue request fails. `WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN)`
  makes it join the strongest mesh node; the ESP32 never roams afterwards.
- **IPs move**: routers reassign addresses, and in a farm the old printer IP
  then belongs to ANOTHER Bambu printer, which answers `access code rejected`
  (MQTT state 5). Find the printer by its serial: Bambu printers announce
  themselves by SSDP on UDP port 2021 (multicast 239.255.255.250) every few
  seconds, with `USN: <serial>` and the IP as sender; a short Python listener
  on the user's computer lists them all. The bridge: `hue find`. Then suggest
  DHCP reservations in the router.
- **The camera's first frame can be garbage** (not starting `FF D8`):
  `sendSnapshot()` skips up to 3 frames. Keep that.
- **Opening the serial port resets a native-USB S3** and every reset drops the
  MQTT session; the printer may refuse the next connect for ~10 s. The board
  retries every 10 s on its own.
- **Blocking**: TLS connects block `loop()`. Keep timeouts short
  (`HUE_CONNECT_MS`, handshake 5 s) or a dropout freezes everything.
- **ntfy**: a body that is not valid UTF-8 becomes an attachment, hence
  `trimUtf8()`. Message text for the photo upload goes in the `?message=`
  query parameter, URL-encoded.
- **millis() wrap**: compare durations as `now - start >= length`, never
  `now >= deadline`.
