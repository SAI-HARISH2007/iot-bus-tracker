# IoT College Bus Tracker (ESP32 + GPS + MQTT)

Live bus tracker for IcfaiTech Route #29 (Katedan → IFHE campus). An ESP32 on the bus reads GPS, computes the next stop and distance on the device, and publishes to an MQTT broker every 5 s. Anyone opens the map page in a browser and sees the bus on a real map with an ETA.

**[Open the live map](https://sai-harish2007.github.io/iot-bus-tracker/web/)** — works on any phone/laptop, no install.

## Architecture

```
GPS → ESP32 (TinyGPS++ NMEA parse, next-stop + distance on-device)
     → Wi-Fi hotspot → MQTT (broker.hivemq.com, retained JSON every 5 s, Last Will for offline)
     → Browser (Leaflet map over MQTT-over-WebSockets, OSRM road routing, ETA = distance / speed)
```

## Repo layout

| Path | What |
|---|---|
| `bus_tracker/bus_tracker.ino` | ESP32 firmware (LIVE via real GPS / REPLAY modes) |
| `docs/index.html` | Live map — single self-contained HTML page |
| `wokwi/` | Wokwi simulation (no hardware needed to test the idea) |
| `Abstract_Bus_Tracker.pdf` | Project abstract |
| `DEMO.md` | 5-minute demo script used for the faculty evaluation |

## Hardware

- ESP32 DevKit V1
- NEO-6M GPS module (UART2)
- SSD1306 128×64 OLED (I2C)

### Wiring

| GPS | ESP32 | | OLED | ESP32 |
|---|---|---|---|---|
| TX | RX2 (GPIO16) | | SDA | D21 |
| RX | TX2 (GPIO17) | | SCL | D22 |
| VCC | VIN (5V) | | VDD | 3V3 |
| GND | GND | | GND | GND |

Leave TX0/RX0 (USB upload pins) unconnected. The on-board **BOOT** button toggles LIVE / REPLAY.

## Flash it

1. Install the ESP32 board package in Arduino IDE ([instructions](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)).
2. Install libraries: **PubSubClient**, **TinyGPS++**, **Adafruit SSD1306**, **Adafruit GFX**.
3. Edit `WIFI_SSID` / `WIFI_PASSWORD` in `bus_tracker.ino` (2.4 GHz hotspot).
4. Board: *ESP32 Dev Module* → Upload.
5. Open `docs/index.html` (or the Pages link). When the OLED shows `MQTT:OK`, the pill turns **online** and the bus marker moves.

## Modes

- **LIVE** — real NEO-6M fix (needs open sky, 1–5 min cold start).
- **REPLAY** — press BOOT once; the bus drives the stored Route 29 stops, for indoor demos.

Unplug the USB after it boots — a power bank keeps it publishing with no laptop.

## Topics

```
icfai/bus/101/loc       position JSON (retained)
icfai/bus/101/status    online / offline (offline via MQTT Last Will)
```

## Limitations (known and stated)

- Needs a phone hotspot; a real deployment needs a SIM module (SIM800L / A7670).
- Public HiveMQ broker has no authentication — fine for a demo, change it for production.
- Stop coordinates are approximate area locations; the route uses straight-line geo points with OSRM road routing for display.
