#!/usr/bin/env python3
"""Send commands to the mqtt_notification board over USB serial and print
what it answers. Works on Windows, macOS and Linux.

    pip install pyserial
    python tools/console.py --port COM5 show
    python tools/console.py --port /dev/ttyACM0 "set wifi_ssid MyWiFi" "set wifi_pass secret" reboot
    python tools/console.py --port COM5 --wait 35 "hue pair"
    python tools/console.py --port COM5 --listen 120        # just watch the log
    python tools/console.py --port COM5 "sim FAILED" @65 "sim FINISH"   # @N = read N s, send nothing

Find the port with `arduino-cli board list` (or the Arduino IDE's port menu).

Opening the port can reset an ESP32-S3 that uses its built-in USB port, so
the script first reads for --boot seconds (and reopens the port if it
disappears during the reset) before sending anything.
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: pip install pyserial")

SECRET_KEYS = ("wifi_pass", "access_code", "hue_key", "ntfy_topic")


def redact(cmd):
    """Never echo a secret value back into the terminal or a log."""
    parts = cmd.split(None, 2)
    if len(parts) == 3 and parts[0].lower() == "set" and parts[1].lower() in SECRET_KEYS:
        return f"set {parts[1]} ***"
    return cmd


def main():
    # A Windows console defaults to cp1252 and cannot print every character;
    # the board's output (or noise while it resets) must never crash us.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial port, e.g. COM5 or /dev/ttyACM0")
    ap.add_argument("--boot", type=float, default=4, help="seconds to read before the first command")
    ap.add_argument("--wait", type=float, default=4, help="seconds to read after each command")
    ap.add_argument("--listen", type=float, default=0, help="seconds to keep reading at the end")
    ap.add_argument("commands", nargs="*", help="commands to send, e.g. show; @N reads N more seconds")
    a = ap.parse_args()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 0.1
    s.dtr = s.rts = False          # do not hold the chip in reset or the bootloader
    s.open()

    def pump(seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = s.read(4096)
            except serial.SerialException:      # USB port vanishes while the chip reboots
                s.close()
                time.sleep(0.5)
                try:
                    s.open()
                except serial.SerialException:
                    pass
                continue
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()

    pump(a.boot)
    for cmd in a.commands:
        if cmd.startswith("@") and cmd[1:].replace(".", "", 1).isdigit():
            pump(float(cmd[1:]))        # a pause in the script: keep reading, send nothing
            continue
        print(f"\n>>> {redact(cmd)}", flush=True)
        try:
            s.write((cmd + "\n").encode())
        except serial.SerialException:
            pump(2)
            s.write((cmd + "\n").encode())
        pump(a.wait)
    pump(a.listen)
    s.close()


if __name__ == "__main__":
    main()
