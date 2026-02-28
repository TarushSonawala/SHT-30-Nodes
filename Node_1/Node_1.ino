#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ESPmDNS.h>

/* ================= CONFIG ================= */

#define NODE_ID "node_4"

struct WiFiCred {
  const char* ssid;
  const char* pass;
};

WiFiCred wifiList[] = {
  { "Laugh_Tale_2.4", "onepiece" },
  { "KinesthetIQ_2.4",   "KinesthetIQ@01" },
  { "Demo_SSID",      "Demo_PWD" }
};

const int WIFI_COUNT = sizeof(wifiList) / sizeof(wifiList[0]);


#define SAMPLE_INTERVAL_MS   60000UL
#define WIFI_RETRY_MS        1000UL
#define WIFI_SCAN_MS        3000UL
#define NTP_RETRY_MS         5000UL

#define LOG_FILE "/log.csv"

/* ========================================== */

Adafruit_SHT31 sht30 = Adafruit_SHT31();
WebServer server(80);

/* ============ NTP CLIENT ============ */

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800); // IST

/* ============ STATE ============ */

unsigned long lastSample = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastWiFiScan = 0;
unsigned long lastNtpAttempt = 0;
int currentWiFiIndex = -1;

bool wifiWasConnected = false;
bool timeValid = false;
bool loggingEnabled = true;
bool mdnsStarted = false;

String getDateTime12Hour() {
  time_t raw = timeClient.getEpochTime();
  struct tm *ti = localtime(&raw);

  char buf[32];
  strftime(buf, sizeof(buf), "%d-%m-%Y %I:%M:%S %p", ti);
  return String(buf);
}

/* ============ LOG MACROS ============ */

#define LOG(tag, msg) do { \
    Serial.print("["); Serial.print(tag); Serial.print("] "); Serial.println(msg); \
  } while (0)

#define LOGF(tag, fmt, ...) do { \
    char buf[128]; snprintf(buf, sizeof(buf), fmt, __VA_ARGS__); \
    Serial.print("["); Serial.print(tag); Serial.print("] "); Serial.println(buf); \
  } while (0)

/* ============ WIFI SCAN ============ */

int findBestKnownNetwork() {
  LOG("WIFI", "Scanning networks...");
  int n = WiFi.scanNetworks();
  int bestIndex = -1;
  int bestRSSI = -999;

  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);

    LOGF("WIFI", "%d: %s | RSSI %d", i + 1, ssid.c_str(), rssi);

    for (int j = 0; j < WIFI_COUNT; j++) {
      if (ssid == wifiList[j].ssid) {
        if (rssi > bestRSSI) {
          bestRSSI = rssi;
          bestIndex = j;
        }
      }
    }
  }

  if (bestIndex >= 0) {
    LOGF("WIFI", "Best known SSID found: %s (RSSI %d)",
         wifiList[bestIndex].ssid, bestRSSI);
  } else {
    LOG("WIFI", "No known SSIDs found");
  }

  return bestIndex;
}


/* ============ WIFI MAINTAIN ============ */

void maintainWiFi() {
if (WiFi.status() == WL_CONNECTED) {
  if (!wifiWasConnected) {
    wifiWasConnected = true;

    LOGF("WIFI", "Connected to SSID: %s", WiFi.SSID().c_str());
    LOGF("WIFI", "IP address: %s", WiFi.localIP().toString().c_str());

    // ----- START mDNS -----
    if (!mdnsStarted) {
if (MDNS.begin(NODE_ID)) {
  mdnsStarted = true;
  MDNS.addService("http", "tcp", 80);
  LOG("MDNS", "mDNS started");
}
else {
        LOG("MDNS", "mDNS failed");
      }
    }
    // ----------------------
  }
  return;
}


  wifiWasConnected = false;

  // Periodic scan
  if (millis() - lastWiFiScan > WIFI_SCAN_MS) {
    lastWiFiScan = millis();
    currentWiFiIndex = findBestKnownNetwork();
  }

  // Retry connect timer
  if (millis() - lastWiFiAttempt < WIFI_RETRY_MS) return;
  lastWiFiAttempt = millis();

  if (currentWiFiIndex < 0) return;

  LOGF("WIFI", "Attempting connection to %s",
       wifiList[currentWiFiIndex].ssid);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(50);

  WiFi.begin(
    wifiList[currentWiFiIndex].ssid,
    wifiList[currentWiFiIndex].pass
  );
}


/* ============ TIME MAINTAIN (NTPClient) ============ */

void maintainTime() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (timeValid) {
    timeClient.update();
    return;
  }

  if (millis() - lastNtpAttempt < NTP_RETRY_MS) return;
  lastNtpAttempt = millis();

  LOG("TIME", "Attempting NTP sync...");
  if (timeClient.update()) {
    unsigned long epoch = timeClient.getEpochTime();
    if (epoch > 1700000000UL) {
      timeValid = true;
      LOG("TIME", "Time synchronized");
      LOGF("TIME", "Now: %s", getDateTime12Hour().c_str());
    }
  }

}

/* ============ FILE LOGGING ============ */

void logData(float t, float h) {
  if (!loggingEnabled) return;

  if (!timeValid) {
    LOG("TIME", "Time not valid → skipping log");
    return;
  }

  String ts = getDateTime12Hour();

  File f = LittleFS.open(LOG_FILE, "a");
  if (!f) {
    LOG("ERROR", "Failed to open log file");
    return;
  }

  f.printf("%s,%.2f,%.2f\n", ts.c_str(), t, h);
  f.close();

  LOGF("LOG", "Saved → %s | temp=%.2f hum=%.2f",
       ts.c_str(), t, h);

}

/* ============ SENSOR ============ */

void sampleSensor() {
  LOG("SENSOR", "Reading SHT30...");
  float t = sht30.readTemperature();
  float h = sht30.readHumidity();

  if (isnan(t) || isnan(h)) {
    LOG("ERROR", "SHT30 read failed");
    return;
  }

  LOGF("SENSOR", "Temperature: %.2f C | Humidity: %.2f %%", t, h);
  logData(t, h);
}

/* ============ WEB SERVER ============ */

void handleRoot() {
  server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>ESP32 Sensor Graph</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>

<body>
<h3>
  Temp: <span id="temp">--</span> °C |
  Humidity: <span id="hum">--</span> %
</h3>

  <h2>ESP32 Temperature & Humidity</h2>
  <canvas id="chart" width="400" height="200"></canvas>

  <script>
    const ctx = document.getElementById('chart').getContext('2d');
    const chart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: [],
        datasets: [
          {
            label: 'Temperature (°C)',
            borderColor: 'red',
            data: [],
            yAxisID: 'y'
          },
          {
            label: 'Humidity (%)',
            borderColor: 'blue',
            data: [],
            yAxisID: 'y1'
          }
        ]
      },
      options: {
        scales: {
          y: {
            type: 'linear',
            position: 'left'
          },
          y1: {
            type: 'linear',
            position: 'right'
          }
        }
      }
    });
async function loadCurrent() {
  const res = await fetch('/current');
  const d = await res.json();
  document.getElementById('temp').innerText = d.t.toFixed(2);
  document.getElementById('hum').innerText = d.h.toFixed(2);
}

loadCurrent();
setInterval(loadCurrent, 2000);

    async function loadData() {
      const res = await fetch('/data');
      const data = await res.json();

chart.data.labels = data.map(d => d.ts);

      chart.data.datasets[0].data = data.map(d => d.t);
      chart.data.datasets[1].data = data.map(d => d.h);
      chart.update();
    }

    loadData();
    setInterval(loadData, 5000);
  </script>

  <p>
    <a href="/download">Download CSV</a> |
    <a href="/clear">Clear Log</a>
  </p>
</body>
</html>
)rawliteral");
}


void handleDownload() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) {
    server.send(404, "text/plain", "No log file");
    return;
  }
  server.streamFile(f, "text/csv");
  f.close();
}

void handleClear() {
  LittleFS.remove(LOG_FILE);
  server.send(200, "text/plain", "Log cleared");
}
void handleData() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) {
    server.send(404, "application/json", "[]");
    return;
  }

  String json = "[";
  bool first = true;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int c1 = line.indexOf(',');
    int c2 = line.lastIndexOf(',');

    if (c1 < 0 || c2 < 0) continue;

    String ts = line.substring(0, c1);
    String t  = line.substring(c1 + 1, c2);
    String h  = line.substring(c2 + 1);

    if (!first) json += ",";
    first = false;

    json += "{";
    json += "\"ts\":" + ts + ",";
    json += "\"t\":" + t + ",";
    json += "\"h\":" + h;
    json += "}";
  }

  json += "]";
  f.close();

  server.send(200, "application/json", json);
}

/* ============ UART MENU ============ */

#define CMD_BUF_SIZE 64
char cmdBuf[CMD_BUF_SIZE];
uint8_t cmdIndex = 0;

void handleCommand(char *cmd) {
  if (strcmp(cmd, "help") == 0) {
    Serial.println("Commands:");
    Serial.println(" status");
    Serial.println(" read");
    Serial.println(" log on");
    Serial.println(" log off");
    Serial.println(" reboot");
  }
  else if (strcmp(cmd, "status") == 0) {
    LOGF("STATUS", "WiFi: %s",
         WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    LOGF("STATUS", "SSID: %s", WiFi.SSID().c_str());
    LOGF("STATUS", "IP: %s", WiFi.localIP().toString().c_str());
    LOGF("STATUS", "Time valid: %s", timeValid ? "YES" : "NO");
  }
  else if (strcmp(cmd, "read") == 0) {
    sampleSensor();
  }
  else if (strcmp(cmd, "log on") == 0) {
    loggingEnabled = true;
    LOG("CMD", "Logging enabled");
  }
  else if (strcmp(cmd, "log off") == 0) {
    loggingEnabled = false;
    LOG("CMD", "Logging disabled");
  }
  else if (strcmp(cmd, "reboot") == 0) {
    LOG("CMD", "Rebooting...");
    ESP.restart();
  }
  else {
    LOG("CMD", "Unknown command (type help)");
  }
}

void handleUART() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdIndex > 0) {
        cmdBuf[cmdIndex] = '\0';
        handleCommand(cmdBuf);
        cmdIndex = 0;
      }
    } else if (cmdIndex < CMD_BUF_SIZE - 1) {
      cmdBuf[cmdIndex++] = c;
    }
  }
}
void handleCurrent() {
  float t = sht30.readTemperature();
  float h = sht30.readHumidity();

  if (isnan(t) || isnan(h)) {
    server.send(500, "application/json", "{}");
    return;
  }

  String json = "{";
  json += "\"t\":" + String(t, 2) + ",";
  json += "\"h\":" + String(h, 2);
  json += "}";

  server.send(200, "application/json", json);
}

/* ============ SETUP ============ */

void setup() {
  Serial.begin(115200);
  delay(1000);

  LOG("BOOT", "ESP32 starting");
  LOG("BOOT", NODE_ID);

  Wire.begin(6, 7);
  LittleFS.begin(true);
  sht30.begin(0x44);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(NODE_ID);

  timeClient.begin();

server.on("/", handleRoot);
server.on("/data", handleData);
server.on("/current", handleCurrent);
server.on("/download", handleDownload);
server.on("/clear", handleClear);
server.begin();


  LOG("WEB", "Web server started");
  
}

/* ============ LOOP ============ */

void loop() {
  maintainWiFi();
  maintainTime();
  handleUART();

  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = millis();
    LOG("TASK", "Sampling interval triggered");
    sampleSensor();
  }

  if (millis() - lastHeartbeat >= 10000) {
    lastHeartbeat = millis();
    LOG("HEARTBEAT", "Node alive");
  }

  server.handleClient();
}
