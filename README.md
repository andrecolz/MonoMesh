# MonoMesh

**Standalone Meshtastic client for the M5Stack Paper Mono** — chat, node list and
offline maps directly on the e-paper device, with no phone, no Bluetooth and no
Wi-Fi required.

[![CI](https://github.com/andrecolz/MonoMesh/actions/workflows/ci.yml/badge.svg)](https://github.com/andrecolz/MonoMesh/actions/workflows/ci.yml)

Current release: **v1.0b (beta)**

## What it can do

- **Mesh messaging** — broadcast channels and end-to-end encrypted direct
  messages (X25519 + AES-256-CCM, Meshtastic 2.5+ PKI), ACK/NAK, retries and
  managed-flooding relay, with unread badges and message history on screen.
- **Node list** — name, model, MAC, PKI lock, RSSI/SNR and hop count; telemetry
  (device, environment, air quality), traceroute, waypoints received and
  NeighborInfo transmitted.
- **Offline maps** — a single `.mmap` container per region (recommended) or a
  PNG tile tree on MicroSD; pan, zoom, node pins, waypoints and "go to my
  position".
- **Touch keyboard** — compose messages with uppercase/symbols and a scrolling
  text field; quick menu and physical buttons for page navigation.
- **On-screen settings** — region, modem preset, frequency, TX power, hop
  limit, channel name and PSK, node identity and role, broadcast intervals,
  TX duty-cycle budget, rebroadcast mode, buzzer mode, timezone/RTC, clock
  orientation and storage info.
- **Power modes** — power off, standby with the radio always listening, giant
  clock, clock + RX; battery protection and configurable power-button action.
- **Works without MicroSD** — settings, keys and recent messages persist in
  NVS; the card adds full chat history, node database and maps.

## Hardware

M5Stack Paper Mono: ESP32-S3, 4.7" 480×800 e-paper (SSD1677), Semtech SX1262
LoRa (EU868 by default, region and preset configurable on screen), PMIC and IO
expander, capacitive touch, RTC and MicroSD slot.

## Build and flash

Requires [PlatformIO](https://platformio.org/) and a USB-C data cable.

```bash
pio run -e m5paper-mono                                              # build
pio run -e m5paper-mono -t upload --upload-port /dev/ttyACM0        # flash
pio device monitor -p /dev/ttyACM0 -b 115200                        # serial log
```

## Offline maps

The tools generate tiles from a local `.osm.pbf` file (for example from
[Geofabrik](https://download.geofabrik.de/)), completely offline and without API
keys:

```bash
# one-time
python3 -m pip install -r requirements.txt

# one .mmap container for the area around a point (recommended)
python3 tools/mmap_converter.py --pbf region.osm.pbf \
    --lat 45.4642 --lon 9.1900 --radius_km 10 --zooms 12,13,14

# inspect, verify or convert an existing container / PNG tree
python3 tools/mono_map.py info   maps/region.mmap
python3 tools/mono_map.py verify maps/region.mmap --tiles-dir tiles --sample 400
```

Copy the resulting `.mmap` file into `/maps` on the MicroSD (or an old-style PNG
tree into `/tiles`).

## Repository layout

- `src/` — firmware sources (Arduino framework)
- `platformio.ini`, `partitions_16mb.csv` — build and flash layout
- `tools/` — offline map pipeline and `.mmap` container tools
- `tools/tests/` — Python test suite (run by CI)
- `.github/workflows/ci.yml` — firmware build + tests on every push/PR

## Test suite

```bash
python3 -m unittest discover -s tools/tests -v
```

## License and attribution

GNU General Public License v3.0 — see [LICENSE](LICENSE).

MonoMesh is an independent implementation interoperable with the
[Meshtastic](https://meshtastic.org/) wire protocol; "Meshtastic" is a trademark
of Meshtastic LLC and this project is neither affiliated with nor endorsed by
Meshtastic LLC. Map data derives from **© OpenStreetMap contributors** (ODbL) and
the tiles must be attributed accordingly. Third-party components (map style,
clock font) are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
