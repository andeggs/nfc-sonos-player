# NFC Sonos Player

Tap an NFC tag to play a Spotify track, album, or artist on a Sonos speaker.
Built on an ESP32 using an RC522 NFC reader. No app, no phone, no button — just tap and play.

---

## How it works

1. An NFC tag is written with a Spotify share URL (e.g. `https://open.spotify.com/album/...`)
2. When the tag is held near the RC522 reader, the ESP32 reads the NDEF record and extracts the URL
3. The URL is parsed to identify the Spotify type (track, album, or artist) and ID
4. A UPnP/SOAP command is sent directly to the Sonos speaker over your local network
5. Music plays

---

## Hardware

- ESP32 DevKit e.g. [this one from PiHut](https://thepihut.com/products/olimex-esp32-devkit-lipo-esp32-development-board)
- Lithium Polymer battery e.g. [this one from Amazon](https://www.amazon.co.uk/dp/B08214DJLJ?ref=ppx_yo2ov_dt_b_fed_asin_title&th=1).
- 2x breadboards e.g. [this one from Amazon](https://www.amazon.co.uk/dp/B0CPJP9YSN?ref=ppx_yo2ov_dt_b_fed_asin_title&th=1)
- RC522 RFID/NFC module e.g. [pre-soldered from this eBay store](https://www.ebay.co.uk/str/themicrohut)
- NFC tags — NTAG213, NTAG215, or NTAG216 stickers work best e.g. [these from Amazon](https://www.amazon.co.uk/dp/B0CSJST6KZ?ref=ppx_yo2ov_dt_b_fed_asin_title)

Note there is no convention among suppliers as to the polarity on the JST Connector for the battery. So you may need to swap the wires.

### Wiring

Connect the RC522 to the ESP32 using the VSPI bus:

| RC522 Pin | ESP32 GPIO | Notes                            |
|-----------|------------|----------------------------------|
| SDA (SS)  | GPIO 5     | Chip select                      |
| SCK       | GPIO 18    | SPI clock                        |
| MOSI      | GPIO 23    | SPI data in                      |
| MISO      | GPIO 19    | SPI data out                     |
| RST       | GPIO 22    | Reset                            |
| 3.3V      | 3.3V       | **Not 5V — module is 3.3V only** |
| GND       | GND        |                                  |

> The IRQ pin on the RC522 is not used — leave it unconnected.

---

## Prerequisites

### Software

- [VS Code](https://code.visualstudio.com/)
- [PlatformIO IDE extension for VS Code](https://platformio.org/install/ide?install=vscode)
- [Git](https://git-scm.com/)

### VS Code and PlatformIO setup

1. Install VS Code from the link above
2. Open VS Code, go to **Extensions** (`Ctrl+Shift+X`), search for **PlatformIO IDE** and install it
3. Restart VS Code — PlatformIO will take a minute to finish initialising
4. Click the PlatformIO icon in the left sidebar (the ant head icon)
5. Click **New Project**:
   - Name: `nfc-sonos-player`
   - Board: `Espressif ESP32 Dev Module`
   - Framework: `Arduino`
6. PlatformIO creates the project folder with a `src/main.cpp` and `platformio.ini`

### Clone this repository

Rather than creating a new project, you can clone this repo directly:

```bash
git clone https://github.com/andeggs/nfc-sonos-player
cd nfc-sonos-player
```

Then open the folder in VS Code with **File → Open Folder**.

---

## Project structure

```
nfc-sonos-player/
├── src/
│   └── main.cpp              # Main sketch
├── include/
│   ├── secrets.h             # Your credentials — never committed
│   └── secrets.h.example     # Template — copy this to secrets.h
├── platformio.ini            # Board and library configuration
├── .gitignore
└── README.md
```

---

## Configuration

### 1. Copy the secrets template

```bash
cp include/secrets.h.example include/secrets.h
```

Then open `include/secrets.h` and fill in your values:

```cpp
#define WIFI_SSID     "your-network-name"
#define WIFI_PASSWORD "your-wifi-password"
#define SONOS_IP      "192.168.x.x"
#define SONOS_UDN     "RINCON_xxxxxxxxxxxx"
#define SONOS_TOKEN   "SA_RINCON2311_X_#Svc2311-xxxxxxxx-Token"
#define SPOTIFY_SN    11
```

> `secrets.h` is listed in `.gitignore` and will never be committed to Git.

### 2. Finding your values

#### `WIFI_SSID` and `WIFI_PASSWORD`
Your home network name and password — the same credentials you use to connect any device to your WiFi. Note that the ESP32 only supports 2.4GHz networks.

#### `SONOS_IP`
The local IP address of your Sonos speaker.

1. Open the **Sonos** app on your phone
2. Go to **Settings → System → About My System**
3. Find your speaker in the list — the IP address is shown beneath it
4. Alternatively, log in to your router's admin page (usually `192.168.1.1`) and look for the Sonos device in the connected devices list

#### `SONOS_UDN` and `SONOS_TOKEN`
These are obtained by capturing the network traffic between the Sonos app and your speaker using **Wireshark**.

1. Download and install [Wireshark](https://www.wireshark.org/)
2. Open Wireshark and start a capture on your WiFi interface
3. Open the Sonos app on your phone and play any Spotify track
4. In Wireshark, filter traffic using: `http && ip.dst == <your-sonos-ip>`
5. Look for a `POST` request to `/MediaRenderer/AVTransport/Control`
6. Click on that request and inspect the body — you will find:
   - A string starting with `RINCON_` — this is your `SONOS_UDN`
   - A string starting with `SA_RINCON2311_X_#Svc2311-` — this is your `SONOS_TOKEN`

These values are tied to your Sonos account's Spotify authorisation. The token may expire after a long period of inactivity — if Spotify playback stops working, repeat the Wireshark capture to obtain a fresh token.

#### `SPOTIFY_SN`
The Sonos service number for Spotify. Check the `sn=` parameter in the URI captured by Wireshark.

---

## Library dependencies

Dependencies are declared in `platformio.ini` and downloaded automatically on first build:

```ini
[env:esp32dev]
platform  = espressif32
board     = esp32dev
framework = arduino
monitor_speed = 115200

lib_deps =
    miguelbalboa/MFRC522@^1.4.10
```

WiFi and HTTPClient are part of the ESP32 Arduino core and require no separate installation.

---

## Building and flashing

| Action         | Shortcut     |
|----------------|--------------|
| Build/compile  | `Ctrl+Alt+B` |
| Upload to ESP32| `Ctrl+Alt+U` |
| Serial monitor | `Ctrl+Alt+S` |

On first build, PlatformIO downloads the ESP32 toolchain and the MFRC522 library automatically — this takes a minute or two.

---

## Writing NFC tags

Use any NFC writing app to write a Spotify share URL onto a blank tag:

- **Android**: [NFC Tools](https://play.google.com/store/apps/details?id=com.wakdev.wdnfc)
- **iOS**: [NFC Tools](https://apps.apple.com/app/nfc-tools/id1252962749)

1. Open Spotify and navigate to any track, album, or artist
2. Tap **Share → Copy Link** — you get a URL like `https://open.spotify.com/album/3mH6qwIy9crq0I9YQbOuDf`
3. Open NFC Tools → **Write** → **Add a record** → **URL**
4. Paste the Spotify link and tap **Write**
5. Hold a blank NTAG sticker against the back of your phone to write it

Supported URL formats:
- `https://open.spotify.com/track/<id>`
- `https://open.spotify.com/album/<id>`
- `https://open.spotify.com/artist/<id>`
- Spotify URI format also works: `spotify:track:<id>` etc.

---

## Serial monitor output

When working correctly you should see:

```
=============================
  NFC → Spotify → Sonos
=============================
Connecting to WiFi........ connected!
IP: 192.168.1.45
✅ RC522 firmware 0x92 ready.

Hold an NFC tag near the reader...

─────────────────────────────
UID      : 04:A1:B2:C3:D4:E5:F6
Tag type : MIFARE Ultralight
URL: https://open.spotify.com/album/3mH6qwIy9crq0I9YQbOuDf
Spotify album → 3mH6qwIy9crq0I9YQbOuDf
Loading album...
▶ Playing album: 3mH6qwIy9crq0I9YQbOuDf
```

---

## Troubleshooting

**RC522 not detected (`0x00` or `0xFF` firmware version)**
Check wiring, confirm you are on 3.3V not 5V, and reseat all jumper connections.

**WiFi timeout at startup**
Check `WIFI_SSID` and `WIFI_PASSWORD` in `secrets.h`. The ESP32 only connects to 2.4GHz networks — a 5GHz-only band will not work.

**Tag reads UID but no URL found**
The tag may be empty or written in a non-NDEF format. Use NFC Tools to inspect the tag contents and rewrite it with a plain URL record.

**Sonos returns a SOAP error (non-200 response)**
The `SONOS_TOKEN` may have expired. Recapture it using Wireshark as described above.

**Same tag keeps retriggering playback**
A 5-second debounce is built in. To adjust it, change the `DEBOUNCE_MS` value near the top of `main.cpp`.

---

## Supported tag types

- NTAG213 / NTAG215 / NTAG216 (recommended)
- MIFARE Ultralight
- MIFARE Classic 1K / 4K (with default or NFC Forum key)

---

## Licence

MIT — do what you like with it.