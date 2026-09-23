#!/usr/bin/env python3
"""Package an ESP-IDF build for flashing from a browser (ESP Web Tools).

    package_firmware.py <firmware/build> <out_dir> <version>

Writes into out_dir:
  manifest.json            what the web page's Install button reads
  bootloader.bin, partition-table.bin, cyd_palm_bridge.bin
                           the three images, each at its own offset
  cyd-palm-<version>-full.bin
                           one image for `esptool.py write_flash 0x0`, attached
                           to a GitHub release

The offsets come from the build's flasher_args.json -- the same file
`idf.py flash` reads -- so the browser writes exactly what the bench does.

Why three parts and not the one merged image: the Wi-Fi and account passwords
live in the NVS partition (secretstore.h), which sits between the partition
table and the app. A merged image pads that gap with 0xFF, so installing it
erases every saved password. The parts skip the gap, and an update keeps them.
This script refuses to write a manifest that would touch NVS.
"""
import csv, json, os, shutil, subprocess, sys

def partitions(csv_path):
    """name -> (offset, size) from partitions.csv"""
    out = {}
    with open(csv_path) as f:
        for row in csv.reader(l for l in f if l.strip() and not l.lstrip().startswith('#')):
            row = [c.strip() for c in row]
            out[row[0]] = (int(row[3], 0), int(row[4], 0))
    return out

def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    build, out, version = sys.argv[1:]
    with open(os.path.join(build, 'flasher_args.json')) as f:
        fa = json.load(f)
    chip = fa['extra_esptool_args']['chip']
    if chip != 'esp32':
        sys.exit(f'package_firmware: built for {chip}, the CYD is an esp32')

    nvs_off, nvs_size = partitions(os.path.join(build, '..', 'partitions.csv'))['nvs']

    os.makedirs(out, exist_ok=True)
    parts = []
    for off_s, rel in sorted(fa['flash_files'].items(), key=lambda kv: int(kv[0], 16)):
        off, src = int(off_s, 16), os.path.join(build, rel)
        end = off + os.path.getsize(src)
        if off < nvs_off + nvs_size and end > nvs_off:
            sys.exit(f'package_firmware: {rel} at {off:#x}..{end:#x} overlaps NVS '
                     f'({nvs_off:#x}..{nvs_off + nvs_size:#x}); an update would erase '
                     f'the saved passwords')
        name = os.path.basename(rel)
        shutil.copyfile(src, os.path.join(out, name))
        parts.append({'path': name, 'offset': off})
        print(f'  {off:#08x}  {name}  ({end - off} bytes)')

    manifest = {
        'name': 'CYD Palm',
        'version': version,
        # Ticking "erase" on a first install wipes the saved passwords; the page
        # says so. An update (the box left clear) keeps them.
        'new_install_prompt_erase': True,
        'builds': [{'chipFamily': 'ESP32', 'parts': parts}],
    }
    with open(os.path.join(out, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=2)
        f.write('\n')

    full = os.path.join(out, f'cyd-palm-{version}-full.bin')
    subprocess.run([sys.executable, '-m', 'esptool', '--chip', chip, 'merge_bin',
                    '-o', full, *fa['write_flash_args'],
                    *[a for kv in fa['flash_files'].items() for a in kv]],
                   cwd=build, check=True, stdout=subprocess.DEVNULL)
    print(f'package_firmware: {out}/manifest.json, version {version}')

if __name__ == '__main__':
    main()
