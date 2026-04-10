# ESP32 OSDP Controller

ESP32-based OSDP control panel firmware for a single peripheral device (PD), with:

- OSDP polling over RS-485
- HID card reads from the connected reader
- local card whitelist stored in LittleFS
- simple web UI for card management and door control
- door relay output
- basic reconnect recovery when the PD is unplugged and reconnected

This project is built with PlatformIO and the Arduino framework.

## Features

- Single OSDP PD configured at address `1`
- Card whitelist stored in `/cards.bin` on LittleFS
- Recent scan log exposed through the web UI
- Learn mode to add a card by presenting it to the reader
- Manual add/delete card actions from the browser
- Manual door trigger from the browser
- Optional HTTP Basic Auth for the web UI/API
- Wi-Fi credentials kept out of git via a local secrets header

## Hardware Assumptions

Current pin configuration in [`src/main.cpp`](C:\Users\mbijw\OneDrive\Documenten\PlatformIO\Projects\OSDP_1\src\main.cpp):

- `DOOR_PIN = 5`
- `RXD2 = 16`
- `TXD2 = 17`
- `RS485_DE_PIN = 4`

Serial settings:

- `Serial2` at `9600 8N1`
- One OSDP peripheral device on RS-485

## Project Structure

- [`src/main.cpp`](C:\Users\mbijw\OneDrive\Documenten\PlatformIO\Projects\OSDP_1\src\main.cpp): firmware logic
- [`data/index.html`](C:\Users\mbijw\OneDrive\Documenten\PlatformIO\Projects\OSDP_1\data\index.html): LittleFS web UI
- [`include/wifi_secrets.example.h`](C:\Users\mbijw\OneDrive\Documenten\PlatformIO\Projects\OSDP_1\include\wifi_secrets.example.h): local Wi-Fi config template
- [`platformio.ini`](C:\Users\mbijw\OneDrive\Documenten\PlatformIO\Projects\OSDP_1\platformio.ini): PlatformIO environment

## Setup

### 1. Wi-Fi secrets

Create a local file:

`include/wifi_secrets.h`

Using this template:

```cpp
#pragma once

#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-password"
```

This file is gitignored.

### 2. Optional web authentication

If you want to protect the web UI, add build flags in [`platformio.ini`](C:\Users\mbijw\OneDrive\Documenten\PlatformIO\Projects\OSDP_1\platformio.ini):

```ini
build_flags =
    -DOSDP_PLATFORM_ARDUINO
    '-DWEB_USERNAME="admin"'
    '-DWEB_PASSWORD="change-me"'
```

If `WEB_USERNAME` and `WEB_PASSWORD` are left empty, the web UI stays open on the local network.

### 3. Build and upload

Typical PlatformIO flow:

```bash
pio run
pio run --target upload
pio run --target uploadfs
pio device monitor
```

`uploadfs` is needed for the web UI in `data/index.html`.

## Web UI / API

Routes currently exposed by the firmware:

- `/` - web UI
- `/cards` - list whitelisted cards as JSON
- `/add?uid=XXXXXXXXXX` - add a card
- `/delete?uid=XXXXXXXXXX` - delete a card
- `/log` - list recent scanned cards as JSON
- `/learn` - enable learn mode
- `/open` - trigger door open flow

UIDs are expected as 10 hex characters for a 5-byte card ID.

## OSDP Notes

- The control panel is configured for one PD at address `1`
- Notifications are enabled with `OSDP_FLAG_ENABLE_NOTIFICATION`
- The firmware listens for online/offline PD notifications
- When the PD goes offline, the firmware now attempts a quick disable/enable recovery cycle to help polling resume after reconnect

## Storage

LittleFS is used for:

- `/index.html`
- `/cards.bin`

Allowed cards are stored as raw binary records of fixed-size 5-byte UIDs.

## Security Notes

- Keep `include/wifi_secrets.h` local and out of source control
- The default SCBK in the current code is still the placeholder `303030...`
- If web auth is disabled, anyone on the same network can use the web endpoints
- `/open` triggers the same access-granted flow as a valid card read

## Known Limitations

- Single PD only
- No role-based users or audit backend
- No HTTPS/TLS on the built-in web server
- Card storage is simple binary file storage
- Build verification is not automated in this repository yet

## Acknowledgments

This project builds on [`libosdp`](https://github.com/goToMain/libosdp), originally written by Siddharth Chandrasekaran.

LibOSDP is a strong piece of work: well-structured, practical, and a big part of what makes projects like this straightforward to build on embedded hardware. A lot of the reliability and protocol heavy lifting here comes from that foundation.

## License

Licensed under Apache-2.0. See [`LICENSE`](C:\Users\mbijw\OneDrive\Documenten\PlatformIO\Projects\OSDP_1\LICENSE).
