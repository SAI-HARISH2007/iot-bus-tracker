// IoT College Bus Tracker - ESP32 + NEO-6M GPS + SSD1306 OLED + MQTT
// Works with the live map at ../web/index.html (same topics, same JSON fields, same stops).
//
// Wiring: GPS TX -> RX2 (GPIO16), GPS RX -> TX2 (GPIO17), GPS VCC -> VIN (5V), GND -> GND
//         OLED SDA -> D21, OLED SCL -> D22, OLED VDD -> 3V3, GND -> GND
//         Nothing on TX0 / RX0 (those are the USB upload pins).
// BOOT button (GPIO0) toggles LIVE (real GPS) / REPLAY (drives the stored route).

#include <WiFi.h>
#include <PubSubClient.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==================== CONFIGURATION ====================
const char* WIFI_SSID     = "motoedge60fusion_1672";
const char* WIFI_PASSWORD = "saiharish";   // exact, case-sensitive

const char* MQTT_BROKER  = "broker.hivemq.com";        // public test broker, no login
const int   MQTT_PORT    = 1883;
const char* TOPIC_LOC    = "icfai/bus/101/loc";        // must match the map page
const char* TOPIC_STATUS = "icfai/bus/101/status";

#define BUTTON_PIN 0                  // on-board BOOT button
#define LED_PIN    2                  // on-board blue LED
const unsigned long PUBLISH_MS = 5000;

// Same stops as STOPS[] in web/index.html
struct Waypoint { double lat; double lon; };
const Waypoint route[] = {      // IFHE Route #29 (Katedan), approximate coordinates
  {17.312664, 78.446796},  // Stop 1 Katedan - Durga Nagar  7:45
  {17.321680, 78.431943},  // Stop 2 Aramgarh Petrol Pump   7:50
  {17.333641, 78.429529},  // Stop 3 Shivarampally Pillar 290 7:53
  {17.357680, 78.420001},  // Stop 4 Upparpally Pillar 174  8:00
  {17.370782, 78.422044},  // Stop 5 Rajapushpa, Attapur    8:05
  {17.422209, 78.218220},  // Stop 6 IFHE Campus
};
const int TOTAL_STOPS = sizeof(route) / sizeof(route[0]);
const double ARRIVED_M = 50;          // within 50 m of a stop counts as "reached"

// ==================== GLOBALS ====================
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledOk = false;
TinyGPSPlus gps;
WiFiClient espClient;
PubSubClient client(espClient);

bool isReplayMode = false;
int replayIndex = 0;
int nextStop = 0;                     // index of the stop the bus is heading to (live mode)
unsigned long lastPublishTime = 0, lastButtonTime = 0, lastMqttTry = 0;
uint32_t messageSeq = 0;

void oledMsg(const char* l1, const char* l2 = "") {
  Serial.printf("%s %s\n", l1, l2);
  if (!oledOk) return;
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(l1);
  display.println(l2);
  display.display();
}

void setupOLED() {
  // If the OLED is mis-wired, keep running without it instead of freezing (Gemini's version hung here)
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOk) { Serial.println("OLED not found at 0x3C - check SDA=D21, SCL=D22, VDD/GND"); return; }
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  oledMsg("IoT Bus Tracker", "Starting...");
}

void connectWiFi() {
  oledMsg("Connecting Wi-Fi", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) oledMsg("WiFi OK", WiFi.localIP().toString().c_str());
  else oledMsg("WiFi FAILED", "check name/pass/2.4GHz");   // loop() keeps retrying
}

// Non-blocking: one attempt every 3 s, so GPS reading and the OLED keep working while offline
void ensureMQTT() {
  if (client.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttTry < 3000) return;
  lastMqttTry = millis();
  String clientId = "ESP32_Bus_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  // Last Will: the broker publishes "offline" by itself if the board loses power or Wi-Fi
  if (client.connect(clientId.c_str(), TOPIC_STATUS, 0, true, "offline")) {
    client.publish(TOPIC_STATUS, "online", true);
    Serial.println("MQTT connected");
  } else {
    Serial.printf("MQTT failed, rc=%d\n", client.state());
  }
}

void checkButton() {
  if (digitalRead(BUTTON_PIN) == LOW && millis() - lastButtonTime > 400) {
    isReplayMode = !isReplayMode;
    lastButtonTime = millis();
    Serial.println(isReplayMode ? "Mode: REPLAY" : "Mode: LIVE");
  }
}

void updateOLED(double lat, double lon, double speed, int sats, bool fix, int stopIdx, double dist) {
  if (!oledOk) return;
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print(isReplayMode ? "[REPLAY] " : "[LIVE] ");
  display.println(client.connected() ? "MQTT:OK" : (WiFi.status() == WL_CONNECTED ? "MQTT:--" : "WiFi:--"));
  display.printf("Fix:%s Sats:%d\n", fix ? "YES" : "NO", sats);
  if (fix) {
    display.printf("%.5f\n%.5f\n", lat, lon);
    display.printf("Next: Stop %d\n", stopIdx + 1);
    display.printf("%dm  %.1fkm/h\n", (int)dist, speed);
  } else {
    display.println("Searching sky...");
    display.println("(go near a window)");
  }
  display.display();
}

void publishLocationData() {
  double lat = 0, lon = 0, speed = 0;
  int sats = 0;
  bool fix = false;
  int stopIdx;

  if (isReplayMode) {
    lat = route[replayIndex].lat;
    lon = route[replayIndex].lon;
    speed = 35.0; sats = 10; fix = true;
    replayIndex = (replayIndex + 1) % TOTAL_STOPS;
    stopIdx = replayIndex;                       // heading to the next waypoint
  } else {
    fix = gps.location.isValid() && gps.location.age() < 3000;
    if (fix) { lat = gps.location.lat(); lon = gps.location.lng(); }
    speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
    sats  = gps.satellites.isValid() ? gps.satellites.value() : 0;
    // Edge logic: once within ARRIVED_M of the target stop, move on to the next one
    if (fix && TinyGPSPlus::distanceBetween(lat, lon, route[nextStop].lat, route[nextStop].lon) < ARRIVED_M
        && nextStop < TOTAL_STOPS - 1) nextStop++;
    stopIdx = nextStop;
  }

  double dist = fix ? TinyGPSPlus::distanceBetween(lat, lon, route[stopIdx].lat, route[stopIdx].lon) : 0;

  char json[220];
  snprintf(json, sizeof(json),
    "{\"id\":101,\"lat\":%.6f,\"lon\":%.6f,\"spd\":%.1f,\"sats\":%d,\"fix\":%s,"
    "\"nxt\":%d,\"dist\":%d,\"seq\":%lu,\"mode\":\"%s\"}",
    lat, lon, speed, sats, fix ? "true" : "false",
    stopIdx + 1, (int)dist, (unsigned long)++messageSeq, isReplayMode ? "replay" : "live");

  if (client.connected()) client.publish(TOPIC_LOC, json, true);
  Serial.println(json);
  updateOLED(lat, lon, speed, sats, fix, stopIdx, dist);
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  Serial2.begin(9600, SERIAL_8N1, 16, 17);   // GPS on UART2: RX=16, TX=17
  Wire.begin(21, 22);
  setupOLED();
  client.setServer(MQTT_BROKER, MQTT_PORT);
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED && millis() - lastMqttTry > 10000) {
    lastMqttTry = millis();
    WiFi.reconnect();
  }
  ensureMQTT();
  client.loop();

  while (Serial2.available() > 0) gps.encode(Serial2.read());
  checkButton();

  if (millis() - lastPublishTime > PUBLISH_MS) {
    lastPublishTime = millis();
    publishLocationData();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));   // heartbeat

    // After 10 s with zero bytes from the GPS, the wiring is wrong (TX/RX swapped or no power)
    if (millis() > 10000 && gps.charsProcessed() < 10)
      Serial.println("WARNING: no data from GPS - check GPS TX -> RX2 (GPIO16) and power");
  }
}
