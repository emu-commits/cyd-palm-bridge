#!/usr/bin/env python3
"""Assert the simulator's LVGL pool matches the device's.

sim/lv_conf.h claimed "device parity" with a number that had been stale since
the device was raised from 24 KB to 32 KB. Nothing caught it, because nothing
compared the two files -- so `make -C sim smoke32` spent months gating against a
configuration no hardware had, 8 KB tighter than reality, and logging image
decode failures the device never sees. A gate that cries wolf gets ignored,
which is worse than not having it.

This is the comparison nobody was doing:

    firmware/sdkconfig.defaults   CONFIG_LV_MEM_SIZE_KILOBYTES
    sim/lv_conf.h                 LV_MEM_SIZE, the 32-bit arm

The 64-bit arm is expected to be exactly twice the device's: on an LP64 host
every LVGL object roughly doubles (8-byte pointers), so double the bytes buys
approximately the same OBJECT capacity, which is what that build is gating.

    python3 sim/tools/check_pool_parity.py     # exits non-zero on a mismatch
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def die(msg):
    print("pool parity: FAIL -- %s" % msg)
    sys.exit(1)


def device_kb():
    p = ROOT / "firmware" / "sdkconfig.defaults"
    m = re.search(r"^CONFIG_LV_MEM_SIZE_KILOBYTES=(\d+)", p.read_text(), re.M)
    if not m:
        die("no CONFIG_LV_MEM_SIZE_KILOBYTES in %s" % p)
    return int(m.group(1))


def sim_kb():
    """the two LV_MEM_SIZE arms, in KB, as (lp64, ilp32)"""
    p = ROOT / "sim" / "lv_conf.h"
    found = re.findall(r"#\s*define\s+LV_MEM_SIZE\s*\((\d+)\s*\*\s*1024U\)",
                       p.read_text())
    if len(found) != 2:
        die("expected exactly two LV_MEM_SIZE arms in %s, found %d"
            % (p, len(found)))
    return int(found[0]), int(found[1])


def main():
    dev = device_kb()
    lp64, ilp32 = sim_kb()

    if ilp32 != dev:
        die("sim 32-bit pool is %d KB but the device is %d KB.\n"
            "       sim/lv_conf.h (the 32-bit arm) must track "
            "firmware/sdkconfig.defaults.\n"
            "       smoke32 and the wasm emulator both use that arm, so a "
            "mismatch means\n"
            "       they gate against hardware that does not exist."
            % (ilp32, dev))

    if lp64 != dev * 2:
        die("sim 64-bit pool is %d KB; expected %d KB (twice the device's %d KB, "
            "because\n       LVGL objects roughly double on an LP64 host)."
            % (lp64, dev * 2, dev))

    print("pool parity: OK -- device %d KB, sim 32-bit %d KB, sim 64-bit %d KB"
          % (dev, ilp32, lp64))


if __name__ == "__main__":
    main()
