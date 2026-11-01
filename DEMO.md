---
origin: ai
date: 2026-10-30
---
# Demo script: IoT College Bus Tracker (Route 29, Katedan)

## Before the faculty arrives (2 min)
1. Phone hotspot ON (`motoedge60fusion_1672`, WPA2, 2.4 GHz).
2. Power the ESP32 (laptop, wall adapter or power bank). Wait for the OLED to show `MQTT:OK`.
3. Open `web/index.html`. Broker = **connected**, Bus = **online**.

## What to show (about 3 min)
1. **Hardware.** ESP32 + NEO-6M GPS (UART2) + SSD1306 OLED (I2C). The OLED shows mode, MQTT state, fix and satellites.
2. **Live mode.** Indoors the GPS shows `Fix:NO`; that is honest, since GPS needs open sky. If there is time, show a real fix near a window or outside (1–5 min cold start).
3. **Replay mode.** Press **BOOT** once. The OLED shows `[REPLAY]`, and the bus drives Route 29 stop by stop on the map, along real roads.
   Say clearly: *"This is replay mode, the stored route, so we can demo indoors."*
4. **Unplug from the laptop and plug into a power bank.** The board reboots, rejoins Wi-Fi and keeps publishing: no laptop needed.
5. **Last Will.** Unplug the power. Within ~1–2 min the map's Bus pill turns **offline**, which the broker publishes itself.

## Architecture (one line)
GPS → ESP32 (parse NMEA with TinyGPS++, next-stop + distance computed on the device) → Wi-Fi → MQTT (HiveMQ public broker, retained JSON every 5 s) → browser (Leaflet map over MQTT-over-WebSockets, OSRM road routing, ETA = distance / speed).

## Likely questions
- **Why MQTT, not HTTP?** Publish/subscribe, small packets, a retained last location for new viewers, Last Will for offline detection.
- **Why is processing on the ESP32?** Edge computing: the next stop and distance are computed on the device, so the server only relays.
- **Limitations (state them yourself).**
  - Needs a phone hotspot; a real bus would need a SIM module (SIM800L / A7670).
  - The public broker has no authentication.
  - Stop coordinates are approximate area locations.
  - The GPS needs open sky.
