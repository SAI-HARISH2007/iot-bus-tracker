// ============================================================================
//  IoT College Bus Tracker - ESP32 firmware (Wokwi simulation build)
//  CS201 Introduction to IoT, IcfaiTech / IFHE Hyderabad
//
//  Hardware (real):  ESP32 DevKit V1 (30-pin), NEO-6M GPS on UART2, SSD1306 OLED on I2C
//  Hardware (Wokwi): ESP32 DevKit V1, SSD1306 OLED, one pushbutton on D4
//
//  SIMULATE_GPS = 1 : no GPS hardware. The firmware builds real NMEA sentences
//                     ($GPRMC/$GPGGA with checksums) from a dummy route and feeds
//                     them to TinyGPSPlus, exactly as a NEO-6M would over UART.
//                     Hold the pushbutton to simulate "GPS signal lost".
//  SIMULATE_GPS = 0 : read the NEO-6M on Serial2 (GPIO16 = RX2, GPIO17 = TX2).
//
//  Everything else (parsing, edge calculation, OLED, Wi-Fi, MQTT, Last Will)
//  is the same code path in both builds. This IS the "replay mode" from the plan.
// ============================================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>

// ----------------------------------------------------------------------------
// Configuration - CHANGE TEAM_PREFIX to something nobody else will use
// ----------------------------------------------------------------------------
#define SIMULATE_GPS 1
#define TEAM_PREFIX  "icfai/cs201/bustracker-CHANGEME"
#define BUS_ID       "101"

const char* WIFI_SSID = "Wokwi-GUEST";     // real hardware: your phone hotspot name
const char* WIFI_PASS = "";                // real hardware: hotspot password
const char* MQTT_HOST = "broker.hivemq.com"; // public test broker, no TLS. Fine for simulation only.
const int   MQTT_PORT = 1883;

const char* T_LOC    = TEAM_PREFIX "/bus/" BUS_ID "/loc";
const char* T_STATUS = TEAM_PREFIX "/bus/" BUS_ID "/status";

const unsigned long PUBLISH_MS   = 5000;  // publish interval
const unsigned long FIX_MAX_AGE  = 3000;  // ms without a fix before we say NO FIX
const double        STOP_RADIUS  = 40.0;  // metres: "arrived at stop"

const int PIN_BTN = 4;    // Wokwi pushbutton to GND (INPUT_PULLUP). Hold = GPS lost.
const int PIN_LED = 2;    // onboard LED
const int GPS_RX  = 16;   // ESP32 RX2  <- GPS TX
const int GPS_TX  = 17;   // ESP32 TX2  -> GPS RX

// ----------------------------------------------------------------------------
// Dummy route and stops (1 bus, 1 route, 5 stops). Coordinates are made up.
// Keep ONE copy of this and reuse it in the web page and the Python simulator.
// ----------------------------------------------------------------------------
struct Pt   { double lat, lon; };
struct Stop { const char* name; double lat, lon; };

const Pt ROUTE[] = {
  {17.4410, 78.3480},
  {17.4430, 78.3560},
  {17.4470, 78.3620},
  {17.4480, 78.3700},
  {17.4520, 78.3780},
  {17.4560, 78.3850},
};
const int N_ROUTE = sizeof(ROUTE) / sizeof(ROUTE[0]);

const Stop STOPS[] = {
  {"S1 Depot",   17.4410, 78.3480},
  {"S2 Market",  17.4430, 78.3560},
  {"S3 Circle",  17.4480, 78.3700},
  {"S4 Gate",    17.4520, 78.3780},
  {"S5 College", 17.4560, 78.3850},
};
const int N_STOPS = sizeof(STOPS) / sizeof(STOPS[0]);

// ----------------------------------------------------------------------------
// Globals
// ----------------------------------------------------------------------------
Adafruit_SSD1306 display(128, 64, &Wire, -1);
TinyGPSPlus      gps;
WiFiClient       net;
PubSubClient     mqtt(net);

bool          stopReached[N_STOPS];
int           nextStop  = 0;       // index into STOPS, N_STOPS = route finished
double        distNext  = 0;       // metres to next stop
bool          haveFix   = false;
unsigned long seq       = 0;

void resetStops() {
  for (int i = 0; i < N_STOPS; i++) stopReached[i] = false;
}

// ----------------------------------------------------------------------------
// GPS simulator: moves a virtual bus along ROUTE and emits NMEA once per second
// ----------------------------------------------------------------------------
#if SIMULATE_GPS
int           segIdx    = 0;
double        segFrac   = 0.0;
int           dwellLeft = 0;
double        simLat    = ROUTE[0].lat;
double        simLon    = ROUTE[0].lon;
double        simSpdKmh = 0.0;
double        simCourse = 0.0;
unsigned long simSecs   = 0;

void simStep() {                       // one simulated second
  const double STEP_M = 10.0;          // 10 m/s = 36 km/h
  simSecs++;
  if (dwellLeft > 0) { dwellLeft--; simSpdKmh = 0; return; }

  Pt a = ROUTE[segIdx];
  Pt b = ROUTE[(segIdx + 1) % N_ROUTE];
  double len = TinyGPSPlus::distanceBetween(a.lat, a.lon, b.lat, b.lon);
  segFrac += STEP_M / len;

  if (segFrac >= 1.0) {                // reached next waypoint: dwell 10 s
    segFrac = 0.0;
    segIdx  = (segIdx + 1) % N_ROUTE;
    simLat  = ROUTE[segIdx].lat;
    simLon  = ROUTE[segIdx].lon;
    dwellLeft = 10;
    if (segIdx == 0) resetStops();     // back at the depot: new trip
  } else {
    simLat = a.lat + (b.lat - a.lat) * segFrac;
    simLon = a.lon + (b.lon - a.lon) * segFrac;
  }
  simSpdKmh = 36.0;
  simCourse = TinyGPSPlus::courseTo(a.lat, a.lon, b.lat, b.lon);
}

// "$" + body + "*" + XOR checksum + CRLF, exactly what a GPS receiver sends
void feedNmea(const char* body) {
  uint8_t cs = 0;
  for (const char* p = body; *p; p++) cs ^= (uint8_t)*p;
  char line[160];
  snprintf(line, sizeof(line), "$%s*%02X\r\n", body, cs);
  for (const char* p = line; *p; p++) gps.encode(*p);
  // Serial.print(line);               // uncomment to see the raw NMEA
}

void emitNmea() {
  unsigned long s = 9600 + simSecs;    // 02:40:00 UTC = 08:10 IST
  char t[16];
  snprintf(t, sizeof(t), "%02lu%02lu%02lu.00", (s / 3600) % 24, (s / 60) % 60, s % 60);

  int    latD = (int)simLat;  double latM = (simLat - latD) * 60.0;
  int    lonD = (int)simLon;  double lonM = (simLon - lonD) * 60.0;

  char body[128];
  snprintf(body, sizeof(body), "GPRMC,%s,A,%02d%07.4f,N,%03d%07.4f,E,%.1f,%.1f,191026,,,A",
           t, latD, latM, lonD, lonM, simSpdKmh / 1.852, simCourse);
  feedNmea(body);
  snprintf(body, sizeof(body), "GPGGA,%s,%02d%07.4f,N,%03d%07.4f,E,1,08,0.9,545.0,M,46.9,M,,",
           t, latD, latM, lonD, lonM);
  feedNmea(body);
}
#endif

// ----------------------------------------------------------------------------
// Edge computation: fix validity, next stop, distance to it
// ----------------------------------------------------------------------------
void edgeCalc() {
  haveFix = gps.location.isValid() && gps.location.age() < FIX_MAX_AGE;
  if (!haveFix) return;

  double la = gps.location.lat(), lo = gps.location.lng();
  for (int i = 0; i < N_STOPS; i++) {
    if (!stopReached[i] &&
        TinyGPSPlus::distanceBetween(la, lo, STOPS[i].lat, STOPS[i].lon) < STOP_RADIUS) {
      stopReached[i] = true;
      Serial.printf("[edge] arrived at %s\n", STOPS[i].name);
    }
  }
  nextStop = N_STOPS;
  for (int i = 0; i < N_STOPS; i++) if (!stopReached[i]) { nextStop = i; break; }
  distNext = (nextStop < N_STOPS)
           ? TinyGPSPlus::distanceBetween(la, lo, STOPS[nextStop].lat, STOPS[nextStop].lon)
           : 0.0;
}

// ----------------------------------------------------------------------------
// Wi-Fi + MQTT
// ----------------------------------------------------------------------------
void wifiEnsure() {
  if (WiFi.status() == WL_CONNECTED) return;
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 5000) return;
  lastTry = millis();
  Serial.println("[wifi] connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);   // channel 6 hint: faster in Wokwi, harmless on real Wi-Fi
}

void mqttEnsure() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 3000) return;
  lastTry = millis();
  String id = String("bus") + BUS_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[mqtt] connecting as %s ...\n", id.c_str());
  // Last Will: broker publishes "offline" (retained) if we vanish without disconnecting
  if (mqtt.connect(id.c_str(), T_STATUS, 0, true, "offline")) {
    mqtt.publish(T_STATUS, "online", true);
    Serial.println("[mqtt] connected");
  } else {
    Serial.printf("[mqtt] failed, rc=%d\n", mqtt.state());
  }
}

void publishLoc() {
  if (!mqtt.connected()) return;
  char payload[240];
  snprintf(payload, sizeof(payload),
    "{\"lat\":%.6f,\"lon\":%.6f,\"spd\":%.1f,\"sats\":%u,\"fix\":%s,"
    "\"nxt\":%d,\"nxtName\":\"%s\",\"dist\":%d,\"seq\":%lu,\"t\":%lu}",
    haveFix ? gps.location.lat() : 0.0,
    haveFix ? gps.location.lng() : 0.0,
    haveFix ? gps.speed.kmph()   : 0.0,
    (unsigned)gps.satellites.value(),
    haveFix ? "true" : "false",
    nextStop,
    nextStop < N_STOPS ? STOPS[nextStop].name : "END",
    (int)distNext, ++seq, millis() / 1000);
  bool ok = mqtt.publish(T_LOC, payload, true);   // retained: a new page sees the last position
  Serial.printf("[mqtt] %s %s\n", ok ? "sent" : "FAILED", payload);
  digitalWrite(PIN_LED, HIGH); delay(20); digitalWrite(PIN_LED, LOW);
}

// ----------------------------------------------------------------------------
// OLED
// ----------------------------------------------------------------------------
void drawOled() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  display.printf("WiFi:%s MQTT:%s\n",
                 WiFi.status() == WL_CONNECTED ? "OK " : "---",
                 mqtt.connected() ? "OK" : "--");
  if (haveFix) {
    display.printf("FIX %2usat %3.0fkm/h\n", (unsigned)gps.satellites.value(), gps.speed.kmph());
    display.printf("%.5f %.5f\n", gps.location.lat(), gps.location.lng());
  } else {
    display.println("NO FIX  searching...");
    display.println("");
  }
  display.println("");
  if (haveFix && nextStop < N_STOPS) {
    display.printf("Next: %s\n", STOPS[nextStop].name);
    display.printf("      %d m\n", (int)distNext);
  } else if (haveFix) {
    display.println("Route complete");
    display.println("");
  } else {
    display.println("Next: ?");
    display.println("");
  }
  display.printf("seq %lu  %s", seq, SIMULATE_GPS ? "SIM" : "LIVE");
  display.display();
}

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  resetStops();

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found at 0x3C (try 0x3D or run an I2C scanner)");
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Bus Tracker " BUS_ID);
  display.println(SIMULATE_GPS ? "mode: SIMULATED GPS" : "mode: LIVE GPS");
  display.println("connecting WiFi...");
  display.display();

#if !SIMULATE_GPS
  Serial2.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
#endif

  WiFi.mode(WIFI_STA);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  Serial.printf("topics: %s | %s\n", T_LOC, T_STATUS);
}

void loop() {
  wifiEnsure();
  mqttEnsure();
  mqtt.loop();

#if SIMULATE_GPS
  static unsigned long lastSim = 0;
  if (millis() - lastSim >= 1000) {
    lastSim = millis();
    simStep();
    if (digitalRead(PIN_BTN) == HIGH) emitNmea();   // button held (LOW) = no sentences = GPS lost
  }
#else
  while (Serial2.available()) gps.encode(Serial2.read());
#endif

  edgeCalc();

  static unsigned long lastOled = 0;
  if (millis() - lastOled >= 1000) { lastOled = millis(); drawOled(); }

  static unsigned long lastPub = 0;
  if (millis() - lastPub >= PUBLISH_MS) { lastPub = millis(); publishLoc(); }
}
