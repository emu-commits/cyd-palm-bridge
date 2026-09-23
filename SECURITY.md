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
