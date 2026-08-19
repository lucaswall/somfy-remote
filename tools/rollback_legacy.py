#!/usr/bin/env python3
"""Rebuild the 2023 firmware's EEPROM image from the MQTT rolling code mirror.

Only needed to roll *back* to the firmware this replaced. That build reads twelve uint16
counters at fixed offsets in the emulated EEPROM sector, which the current firmware no
longer writes — so the counters have to be put back into that shape first, or every
shutter ignores the first command and keeps ignoring it until the count catches up.

    tools/rollback_legacy.py --host <BROKER_IP> --user <U> --password <P> --out legacy.bin

Then, and this is the part that is easy to get wrong:

    esptool.py -p /dev/cu.usbserial-N erase_region 0x3FA000 0x1000   # BOTH store sectors
    esptool.py -p /dev/cu.usbserial-N erase_region 0x3FB000 0x1000
    esptool.py -p /dev/cu.usbserial-N write_flash  0x3FB000 legacy.bin

Erasing only the sector being overwritten leaves a valid header in the other one. A later
re-upgrade would find it, conclude migration had already happened, and adopt counters from
a sector that stopped being updated the day of the rollback.

The counter is a monotonic u32 in the mirror and 16 bits on the wire, so it is written
here modulo 65536 — the wrap is what the motor expects.
"""

import argparse
import json
import struct
import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("error: no paho-mqtt. fix: pip install paho-mqtt")

LEGACY_REMOTES = 12
SECTOR_SIZE = 4096
COUNT_OFFSET = 60


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--device-id", default="wemos_somfy_remote")
    ap.add_argument("--out", required=True)
    ap.add_argument("--wait", type=float, default=4.0)
    args = ap.parse_args()

    codes = {}

    def on_message(_client, _userdata, msg):
        suffix = msg.topic.rsplit("/", 1)[-1]
        if not suffix.startswith("remote"):
            return
        try:
            codes[int(suffix[len("remote"):])] = int(msg.payload)
        except ValueError:
            print(f"warning: {msg.topic} is not a number", file=sys.stderr)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if args.user:
        client.username_pw_set(args.user, args.password)
    client.on_message = on_message
    client.connect(args.host, args.port, 10)
    client.subscribe(f"{args.device_id}/code/+")
    client.loop_start()
    time.sleep(args.wait)
    client.loop_stop()
    client.disconnect()

    if not codes:
        sys.exit("error: the mirror is empty — nothing to rebuild from")

    missing = [i for i in range(LEGACY_REMOTES) if i not in codes]
    if missing:
        # Writing a zero for a missing counter would be the one unrecoverable mistake:
        # the motor rejects everything until the count climbs back.
        sys.exit(f"error: no mirrored counter for remotes {missing} — refusing to guess")

    image = bytearray(b"\xFF" * SECTOR_SIZE)
    for i in range(LEGACY_REMOTES):
        struct.pack_into("<H", image, i * 2, codes[i] % 0x10000)
        print(f"  remote {i:2d}: {codes[i]} -> 0x{codes[i] % 0x10000:04X}",
              file=sys.stderr)
    struct.pack_into("<H", image, COUNT_OFFSET, LEGACY_REMOTES)

    with open(args.out, "wb") as handle:
        handle.write(image)
    print(f"wrote {args.out} ({SECTOR_SIZE} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
