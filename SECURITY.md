# Security

## What the device holds, and where

| What | Where | Protected by |
|---|---|---|
| Wi-Fi passwords (up to four networks) | the ESP32's own flash (NVS, namespace `cydsec`) | not being on the removable card |
| The Apple ID app-specific password | the ESP32's own flash (NVS) | the same |
| Everything else in `config.ini` — the Apple ID, collection paths, location, settings | the SD card, plain text | nothing: anyone with the card can read it |
| Your calendar, contacts, to dos and memos | the SD card, as Palm databases | nothing: anyone with the card can read them |

**The passwords are not encrypted.** They were moved off the SD card because a
card comes out of the device and into any computer; the device's flash does not.
But NVS is plain flash unless ESP32 flash encryption is enabled, which this
project does not do, and someone with the device and a USB cable can read it.
Treat a lost device as a lost password: revoke the app-specific password at
[appleid.apple.com](https://appleid.apple.com) (it is scoped and revocable by
design — never use your main Apple ID password here, and iCloud will refuse it
anyway) and change the Wi-Fi passwords that matter to you.

A password typed into `config.ini` on a computer still works: on its next boot
the device moves it into its flash and rewrites the file without it. If the
device cannot write its flash, the password is left in the file rather than
lost, and the boot log says so (`appcfg: ... could NOT move them off the card`).

## Passwords in RAM

**A password is in RAM only while it is being used.** The device's running
configuration holds a "has a password" flag per network and for the account,
never a password:

- **A Wi-Fi join** reads the network's password from flash into the driver's
  config, hands it to the driver, and wipes its own copy. The driver is told to
  keep its copy in RAM only (`WIFI_STORAGE_RAM`), not to write a second one to
  flash, and it is freed when the radio goes down after the sync.
- **A sync or "Find my calendars"** reads the account password into the request
  context for that run, and wipes it before the radio goes down. The
  `Authorization` header built from it is wiped after each request.
- **The password screens never show a stored password**, not even masked into a
  field: they start empty and say "(saved -- type to replace)". What you type
  goes into a buffer of the screen's own, never into the UI toolkit's memory,
  and is wiped when the screen closes, however it closes. It shows as `*`,
  except that the character just typed shows for a second, and **Show** shows
  all of it (mind who can see the screen). What is drawn comes from that same
  buffer, which the toolkit draws from without copying.
- **`make -C sim secretscan`** checks this in CI. It types two test passwords
  through the real Settings screens, runs the simulator's sync (which reads and
  wipes them the way the device does), and after each step searches every
  writable page of the process, freed memory included, for any 12-byte piece of
  either one.

What that cannot cover: the Wi-Fi driver and the HTTP client each hold a copy of
what they were given for as long as the sync runs, and free it afterwards
without wiping; a buffer on a task's stack can outlive its function until that
stack is reused (the code wipes its own; the gate cannot see one that is
missed); and none of this helps against someone who has the device and reads
its flash, where the passwords are stored unencrypted (above).

## What is never done

- **No credential is compiled into any build.** A developer's `secrets.h` used
  to be; it is gone, and `make -C sim nosecrets` fails if anything reintroduces
  one.
- **Passwords are never logged.** The serial log names the Wi-Fi network being
  tried and says how many passwords are held, never their values.
- **The browser emulator never stores a password.** They live in memory for the
  page's lifetime only.
- **The account sync is TLS with certificate validation** (the ESP-IDF
  certificate bundle), which is why the device sets its clock over NTP before a
  sync. Two things are not: the IP location lookup is plain HTTP (it sends
  nothing but the request, and the reply only ever moves an *approximate*
  location), and a news feed is fetched over whatever its URL says.

## Reporting a problem

Please report security problems privately through GitHub's
**Security → Report a vulnerability** on this repository rather than in a public
issue. Say what an attacker needs (the card, the device, the network) and what
they get.
