# mqtt_notification

An ESP32 watches your Bambu Lab 3D printer over your local network and shows how
the print is going on a Philips Hue light. When the print **finishes** or
**fails**, it sends a push to your phone, and on a failure the push carries a
photo from the printer's camera so you can see what happened.

> The light tells you how the print is going when you are in the room.
> The phone tells you when you are not.

| Printer | Hue light | Push to your phone |
|---|---|---|
| Heating, levelling | white 40 % | - |
| Printing | blue, getting brighter with progress | - |
| Paused | yellow | - |
| **Failed** (real error) | red, flashing for 60 s, then red | urgent "Print failed: file" + camera photo |
| Cancelled by you | red for 10 s, then off | - |
| **Finished** | green for 30 min, then off | "Done: file in 2 h 14 min" |
| Idle | off | - |

Everything stays on your network except the push itself. No Bambu cloud
needed, and it works in LAN Only mode.

## What you need

- **A Bambu Lab printer** with LAN access: its IP address, LAN access code and
  serial number. The camera photo works on the **A1, A1 mini, P1P and P1S**;
  the X1, H2 and P2S get the push without a photo.
- **An ESP32 or ESP32-S3 board** (any dev board, about 10 CHF) and a USB cable.
- **A Philips Hue Bridge** (the square v2 bridge or the Bridge Pro) and one or more colour lights
  (light bar, light strip, bulb).
- **A phone with the free [ntfy](https://ntfy.sh) app** (Android or iPhone).
- **A computer** with the [Arduino IDE](https://www.arduino.cc/en/software)
  (or `arduino-cli`), and optionally [Claude Code](https://claude.com/claude-code).

## The easy way: let Claude set it up

```bash
git clone https://github.com/3dfarmers/mqtt_notification
cd mqtt_notification
claude
```

Then tell it: *"Help me set this up. My printer is a P1S, my board is plugged
in."* Claude reads [CLAUDE.md](CLAUDE.md), installs the toolchain, flashes the
board, walks you through pairing the Hue bridge and the ntfy app, and runs the
tests with you. It can also change the project for you: *"use my WLED strip
instead of Hue"*, *"send it to Telegram"*, *"make paused purple"*.

## The manual way

### 1. Flash the board

1. Arduino IDE 2: **Boards Manager** → install **esp32 by Espressif Systems** (3.x).
2. **Library Manager** → install **PubSubClient** by Nick O'Leary.
3. Open `mqtt_notification/mqtt_notification.ino`.
4. Board: **ESP32 Dev Module** (classic ESP32) or **ESP32S3 Dev Module**. On an
   S3 plugged into its native USB port, set **USB CDC On Boot: Enabled**,
   otherwise you see nothing in the Serial Monitor.
5. Pick the port, click **Upload**.

### 2. Configure it

Open the **Serial Monitor** at **115200 baud** with **Newline**, and type:

```
set wifi_ssid YourWiFi
set wifi_pass YourWiFiPassword
set printer_ip 192.168.1.50
set access_code 12345678
set serial 01P00A123456789
reboot
```

The IP and access code are on the printer's screen in the network / LAN
settings, the serial number in the device info (or in Bambu Studio → Device).

Then connect the Hue bridge, still in the Serial Monitor:

```
hue find                  finds the bridge and saves its IP
hue pair                  press the round button on the bridge first
hue lights                lists your lights: id + name
hue blink <id>            makes that light blink, to find the right one
set hue_lights <id>       several: <id>,<id>
```

And the phone: in the ntfy app, subscribe to a **long random topic name**
(20+ characters, for example `print-k8x2m9q4w7v1z3b6`). Anyone who knows the
name can read your pushes, so treat it like a password. On Android, turn on
*Instant delivery* for the topic and set the app's battery use to
*Unrestricted*, or pushes can arrive minutes late.

```
set ntfy_topic print-k8x2m9q4w7v1z3b6
```

`show` prints everything (secrets masked), the WiFi signal, the printer state
and the last light and push result.

### 3. Test it, without printing

```
hue red                   the light turns red
notify                    the phone buzzes
snap                      the phone gets a camera photo (A1 / P1)
sim RUNNING 40            light blue
sim PAUSE                 light yellow
sim FAILED                light red and flashing, urgent push with photo
sim FINISH                light green, "Done" push
live                      back to following the real printer
```

Then a real print. Cancel one on purpose and look at the log line
`[PRINT] FAILED, print_error ...`: if your printer reports a cancel code
that is not in `CANCEL_CODES` at the top of the sketch, add it there, or
every cancel will be treated as a failure.

## All commands

| Command | What it does |
|---|---|
| `show` | settings (secrets masked), WiFi signal, printer, light, last push |
| `set <key> <value>` | `wifi_ssid` `wifi_pass` `printer_ip` `access_code` `serial` `hue_ip` `hue_key` `hue_lights` `ntfy_server` `ntfy_topic`; `-` clears a value |
| `hue find` / `pair` / `lights` / `blink <id>` | set up the Hue bridge |
| `hue white\|blue\|yellow\|green\|red\|off` | light test |
| `notify` / `snap` | push test, without / with camera photo |
| `sim <state> [pct]` / `live` | pretend the printer is in a state / stop pretending |
| `reboot` | restart (needed after changing WiFi or printer settings) |

Settings are stored on the board, not in the code, so updating the firmware
keeps them.

## Make it yours

The colours, brightness, flash timing and how long green stays on are
constants at the top of `mqtt_notification.ino`. The [CLAUDE.md](CLAUDE.md)
file maps the code for bigger changes: another light system (WLED, Home
Assistant, Govee, Nanoleaf), another push service (Telegram, Pushover,
Discord), or another printer brand (Klipper / Moonraker, PrusaLink,
OctoPrint). Open the folder in Claude Code and describe what you have.

## Troubleshooting

- **`WiFi: weak signal`, or the light reacts only sometimes.** Every Hue command
  and push opens an encrypted connection, and those break below about -70 dBm
  while the printer link still works. Move the board closer to your router or
  mesh node. `show` prints the signal.
- **`[MQTT] failed`.** Check the printer IP and access code, and that LAN mode
  is on. On newer Bambu firmware, third-party tools may also need *Developer
  Mode* turned on in the printer's LAN settings.
- **`connected but no printer data`.** The serial number is wrong. It is case
  sensitive; the board uppercases it for you.
- **`camera ... not answering`.** Only the A1, A1 mini, P1P and P1S have the
  photo stream. On those, check that *LAN Only Liveview* is on.
- **Pushes arrive late on Android.** Enable *Instant delivery* and unrestricted
  battery use for ntfy (see step 2).
- **Hue `HTTP 403` / `404`.** Run `hue pair` again, or check the light ids with
  `hue lights`.

## How it works

The board subscribes to the printer's MQTT report over TLS (port 8883, user
`bblp`, your access code). P1 and A1 printers send only the fields that
changed, so the board keeps a merged copy of the state and acts only on
changes. Hue lights are driven through the bridge's local CLIP v2 API, with
the connection kept open while the light flashes. Pushes go to
[ntfy](https://ntfy.sh). For the failure photo, one JPEG frame is read from
the printer's camera stream on port 6000 and streamed straight into the
upload, so no extra memory chip is needed.

The certificates of the printer and the Hue bridge are self-signed and
accepted without checking, as most local Bambu and Hue tools do. The ntfy
certificate is not checked yet either (TODO).

## Credits

- Camera protocol: [OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI),
  cross-checked with [PrintSphere](https://github.com/cptkirki/PrintSphere)
- [PubSubClient](https://github.com/knolleary/pubsubclient) by Nick O'Leary
- [ntfy](https://ntfy.sh) by Philipp Heckel

## License

MIT, see [LICENSE](LICENSE). Made by 3D Farmers GmbH.
