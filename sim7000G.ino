#define TINY_GSM_MODEM_SIM7000
#define TINY_GSM_RX_BUFFER 1024

#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <Adafruit_Sensor.h>
#include <DHT22.h>

// ─── Pin definitions (LilyGO T-SIM7000G) ───
#define UART_BAUD   115200
#define PIN_TX      27
#define PIN_RX      26
#define PWR_PIN     4
#define LED_PIN     12
#define ADC_PIN     35

// DHT22 GPIO
#define DHTPIN 33
#define DHTTYPE DHT22
DHT22 dht(33);

#define SerialMon Serial
#define SerialAT  Serial1

// ─── Configuration ───
const char apn[]      = "";  // Search for this one, it is required
const char gprsUser[] = "";
const char gprsPass[] = "";
const char server[]   = "x.x.x.x";
const int  port       = 5000;

// Timing
const unsigned long UPLOAD_INTERVAL_MS   = 5UL * 60 * 1000;  // 5 minutes
const unsigned long REBOOT_INTERVAL_MS   = 6UL * 60 * 60 * 1000; // 6 hours
const unsigned long GPS_FIX_TIMEOUT_MS   = 90000; // 90s initial fix wait
const unsigned long WDT_TIMEOUT_S        = 120;   // watchdog timeout

// Buffer sizes
const uint16_t MAX_SEEN_BSSIDS   = 1000;
const uint16_t MAX_UPLOAD_BUFFER = 100;
const uint8_t  MAX_CONSECUTIVE_FAILURES = 3;

// ─── Data structures ───
struct WiFiNetwork {
    uint8_t bssid[6];
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    uint8_t authMode;
    float   lat, lon;
    float   alt, accuracy;
    bool    hasGps;
};

struct GPSData {
    float lat, lon, alt, accuracy;
    bool  valid;
};

// ─── Global state ───
// Dedup circular buffer
uint8_t  seenBssids[MAX_SEEN_BSSIDS][6];
uint16_t seenCount = 0;
uint16_t seenHead  = 0;

// Upload buffer
WiFiNetwork uploadBuffer[MAX_UPLOAD_BUFFER];
uint16_t    uploadBufferCount = 0;

// Cached GPS
GPSData gpsCache = {0, 0, 0, 0, false};

// State tracking
bool     scanInProgress    = false;
unsigned long lastUploadTime    = 0;
unsigned long bootTime          = 0;
uint8_t  consecutiveFailures   = 0;
uint8_t  cfunFailures          = 0;

// Modem & HTTP
TinyGsm       modem(SerialAT);
TinyGsmClient client(modem);
HttpClient    http(client, server, port);

// ─── Forward declarations ───
void    disableCellularRF();
bool    enableCellularAndConnect();
void    updateGPSCache();
bool    isSeen(const uint8_t *bssid);
void    addSeen(const uint8_t *bssid);
void    processScanResults(int count);
bool    uploadNetworks();
bool    sendHeartbeat();
void    performUploadWindow();
const char* getAuthMode(uint8_t encType);
String  bssidToString(const uint8_t *bssid);
float   getBatteryVoltage();
void    readDHT22(float &temperature, float &humidity);
void    modemPowerToggle();

// ═══════════════════════════════════════════════
// Modem cellular control
// ═══════════════════════════════════════════════

void disableCellularRF() {
    SerialMon.println("Disabling cellular RF (CFUN=0)...");
    modem.sendAT("+CFUN=0");
    modem.waitResponse(10000L);
}

bool enableCellularAndConnect() {
    SerialMon.println("Enabling cellular RF (CFUN=1)...");
    modem.sendAT("+CFUN=1");
    if (modem.waitResponse(10000L) != 1) {
        SerialMon.println("CFUN=1 failed");
        cfunFailures++;
        if (cfunFailures >= 3) {
            SerialMon.println("CFUN failed 3 times, power cycling modem...");
            modemPowerToggle();
            cfunFailures = 0;
            delay(3000);
            modem.sendAT("+CFUN=1");
            if (modem.waitResponse(10000L) != 1) {
                return false;
            }
        } else {
            return false;
        }
    }
    cfunFailures = 0;

    SerialMon.println("Waiting for network...");
    if (!modem.waitForNetwork(60000L)) {
        SerialMon.println("Network registration failed");
        return false;
    }

    SerialMon.println("Connecting GPRS...");
    if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
        SerialMon.println("GPRS connect failed");
        return false;
    }

    SerialMon.println("Cellular connected");
    return true;
}

void modemPowerToggle() {
    SerialMon.println("Power cycling modem via PWR_PIN...");
    digitalWrite(PWR_PIN, HIGH);
    delay(1500);
    digitalWrite(PWR_PIN, LOW);
    delay(3000);
    // Re-init modem
    modem.restart();
    // Re-enable GPS after restart
    modem.sendAT("+SGPIO=0,4,1,1");
    modem.waitResponse(10000L);
    modem.sendAT("+CGNSPWR=1");
    modem.waitResponse(10000L);
}

// ═══════════════════════════════════════════════
// GPS
// ═══════════════════════════════════════════════

void updateGPSCache() {
    float lat, lon, speed, alt, accuracy;
    int vsat, usat, year, month, day, hour, minute, second;

    if (modem.getGPS(&lat, &lon, &speed, &alt, &vsat, &usat, &accuracy,
                     &year, &month, &day, &hour, &minute, &second)) {
        gpsCache.lat      = lat;
        gpsCache.lon      = lon;
        gpsCache.alt      = alt;
        gpsCache.accuracy = accuracy;
        gpsCache.valid    = true;
    }
    // If getGPS fails, keep previous cached values (valid stays as-is)
}

// ═══════════════════════════════════════════════
// WiFi dedup & scan processing
// ═══════════════════════════════════════════════

bool isSeen(const uint8_t *bssid) {
    for (uint16_t i = 0; i < seenCount; i++) {
        if (memcmp(seenBssids[i], bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

void addSeen(const uint8_t *bssid) {
    if (seenCount < MAX_SEEN_BSSIDS) {
        memcpy(seenBssids[seenCount], bssid, 6);
        seenCount++;
    } else {
        // Circular overwrite
        memcpy(seenBssids[seenHead], bssid, 6);
        seenHead = (seenHead + 1) % MAX_SEEN_BSSIDS;
    }
}

void processScanResults(int count) {
    // Update GPS once for this scan cycle
    updateGPSCache();

    for (int i = 0; i < count; i++) {
        // WiFi.BSSID(i) returns pointer invalidated by scanDelete — copy immediately
        uint8_t *bssidPtr = WiFi.BSSID(i);
        if (!bssidPtr) continue;

        uint8_t bssid[6];
        memcpy(bssid, bssidPtr, 6);

        if (isSeen(bssid)) continue;

        addSeen(bssid);

        // If upload buffer is full, we can't store more — they'll be re-discovered later
        if (uploadBufferCount >= MAX_UPLOAD_BUFFER) {
            SerialMon.println("Upload buffer full, skipping remaining new networks");
            break;
        }

        WiFiNetwork *net = &uploadBuffer[uploadBufferCount];
        memcpy(net->bssid, bssid, 6);
        strncpy(net->ssid, WiFi.SSID(i).c_str(), 32);
        net->ssid[32] = '\0';
        net->rssi     = (int8_t)WiFi.RSSI(i);
        net->channel  = (uint8_t)WiFi.channel(i);
        net->authMode = (uint8_t)WiFi.encryptionType(i);
        net->lat      = gpsCache.lat;
        net->lon      = gpsCache.lon;
        net->alt      = gpsCache.alt;
        net->accuracy = gpsCache.accuracy;
        net->hasGps   = gpsCache.valid;

        uploadBufferCount++;

        SerialMon.printf("New: %s [%s] ch%d %ddBm (buf:%d)\n",
            bssidToString(bssid).c_str(), net->ssid, net->channel, net->rssi, uploadBufferCount);
    }

    WiFi.scanDelete();
}

// ═══════════════════════════════════════════════
// Upload functions
// ═══════════════════════════════════════════════

bool uploadNetworks() {
    if (uploadBufferCount == 0) return true;

    SerialMon.printf("Uploading %d networks...\n", uploadBufferCount);

    // Build JSON array manually to avoid large ArduinoJson allocation
    String payload = "[";
    for (uint16_t i = 0; i < uploadBufferCount; i++) {
        WiFiNetwork *net = &uploadBuffer[i];
        if (i > 0) payload += ",";

        payload += "{\"mac\":\"";
        payload += bssidToString(net->bssid);
        payload += "\",\"ssid\":\"";
        // Escape any quotes in SSID
        for (int j = 0; net->ssid[j] != '\0'; j++) {
            if (net->ssid[j] == '"') payload += "\\\"";
            else if (net->ssid[j] == '\\') payload += "\\\\";
            else payload += net->ssid[j];
        }
        payload += "\",\"auth_mode\":\"";
        payload += getAuthMode(net->authMode);
        payload += "\",\"channel\":";
        payload += String(net->channel);
        payload += ",\"rssi\":";
        payload += String(net->rssi);

        if (net->hasGps) {
            payload += ",\"latitude\":";
            payload += String(net->lat, 6);
            payload += ",\"longitude\":";
            payload += String(net->lon, 6);
            payload += ",\"altitude\":";
            payload += String(net->alt, 1);
            payload += ",\"accuracy\":";
            payload += String(net->accuracy, 1);
        }

        payload += "}";
    }
    payload += "]";

    SerialMon.printf("Payload size: %d bytes\n", payload.length());

    http.beginRequest();
    http.post("/api/wigle_data_batch");
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("Content-Length", payload.length());
    http.beginBody();
    http.print(payload);
    http.endRequest();

    int statusCode = http.responseStatusCode();
    String response = http.responseBody(); // Must consume body

    SerialMon.printf("Upload status: %d\n", statusCode);

    if (statusCode == 201 || statusCode == 200) {
        uploadBufferCount = 0;
        return true;
    }

    SerialMon.printf("Upload failed: %s\n", response.c_str());
    return false;
}

bool sendHeartbeat() {
    SerialMon.println("Sending heartbeat...");

    float batt = getBatteryVoltage();
    float temperature = 0.0;
    float humidity = 0.0;
    readDHT22(temperature, humidity);

    String payload = "{\"type\":\"heartbeat\",\"mac\":\"";
    payload += WiFi.macAddress();
    payload += "\",\"battery\":";
    payload += String(batt, 2);
    payload += ",\"temperature\":";
    payload += String(temperature, 2);
    payload += ",\"humidity\":";
    payload += String(humidity, 2);
    payload += "}";

    http.beginRequest();
    http.post("/api/heartbeat");
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("Content-Length", payload.length());
    http.beginBody();
    http.print(payload);
    http.endRequest();

    int statusCode = http.responseStatusCode();
    http.responseBody(); // Consume body

    SerialMon.printf("Heartbeat status: %d\n", statusCode);
    return (statusCode == 200 || statusCode == 201);
}

void performUploadWindow() {
    bool hasData = (uploadBufferCount > 0);

    if (!hasData) {
        SerialMon.println("Upload window: buffer empty, skipping");
        return;
    }

    SerialMon.printf("=== Upload window: %d networks to send ===\n", uploadBufferCount);

    if (!enableCellularAndConnect()) {
        SerialMon.println("Failed to connect cellular, will retry next window");
        consecutiveFailures++;
        disableCellularRF();
        if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
            SerialMon.println("Too many consecutive failures, rebooting...");
            ESP.restart();
        }
        return;
    }

    bool uploadOk = uploadNetworks();
    sendHeartbeat(); // Best effort

    if (uploadOk) {
        consecutiveFailures = 0;
    } else {
        consecutiveFailures++;
        if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
            SerialMon.println("Too many consecutive failures, rebooting...");
            ESP.restart();
        }
    }

    // Disconnect and disable cellular RF
    modem.gprsDisconnect();
    disableCellularRF();

    SerialMon.println("=== Upload window complete ===");
}

// ═══════════════════════════════════════════════
// Utility functions
// ═══════════════════════════════════════════════

String bssidToString(const uint8_t *bssid) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    return String(buf);
}

const char* getAuthMode(uint8_t encType) {
    switch (encType) {
        case WIFI_AUTH_OPEN:         return "open";
        case WIFI_AUTH_WEP:          return "wep";
        case WIFI_AUTH_WPA_PSK:      return "wpa";
        case WIFI_AUTH_WPA2_PSK:     return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
        default:                     return "unknown";
    }
}

float getBatteryVoltage() {
    uint16_t v = analogRead(ADC_PIN);
    return ((float)v / 4095.0) * 2.0 * 3.3 * (1100.0 / 1000.0);
}

void readDHT22(float &temperature, float &humidity) {
    temperature = dht.getTemperature();
    humidity = dht.getHumidity();

    if (dht.getLastError() != DHT22::OK) {
        SerialMon.println("Failed to read from DHT22 sensor!");
        temperature = -1;
        humidity = -1;
    } else {
        SerialMon.printf("Temperature: %.2f °C, Humidity: %.2f %%\n", temperature, humidity);
    }
}

// ═══════════════════════════════════════════════
// setup()
// ═══════════════════════════════════════════════

void setup() {
    SerialMon.begin(115200);
    SerialMon.println("Starting up...");

    bootTime = millis();

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);

    // Power on modem
    pinMode(PWR_PIN, OUTPUT);
    digitalWrite(PWR_PIN, HIGH);
    delay(300);
    digitalWrite(PWR_PIN, LOW);
    delay(1000);

    SerialAT.begin(UART_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

    SerialMon.println("Initializing modem...");
    if (!modem.restart()) {
        SerialMon.println("Failed to restart modem, continuing...");
    }

    String modemInfo = modem.getModemInfo();
    SerialMon.print("Modem Info: ");
    SerialMon.println(modemInfo);

    // Enable GPS antenna power
    SerialMon.println("Enabling GPS...");
    modem.sendAT("+SGPIO=0,4,1,1");
    modem.waitResponse(10000L);
    modem.sendAT("+CGNSPWR=1");
    modem.waitResponse(10000L);

    // Wait for initial GPS fix
    SerialMon.println("Waiting for GPS fix...");
    unsigned long gpsStart = millis();
    while (millis() - gpsStart < GPS_FIX_TIMEOUT_MS) {
        updateGPSCache();
        if (gpsCache.valid) {
            SerialMon.printf("GPS fix: %.6f, %.6f\n", gpsCache.lat, gpsCache.lon);
            break;
        }
        delay(2000);
    }
    if (!gpsCache.valid) {
        SerialMon.println("No GPS fix after timeout, continuing without");
    }

    // Send boot heartbeat over cellular (modem is already up from restart)
    SerialMon.println("Connecting cellular for boot heartbeat...");
    if (modem.waitForNetwork(60000L)) {
        if (modem.gprsConnect(apn, gprsUser, gprsPass)) {
            sendHeartbeat();
            modem.gprsDisconnect();
        }
    }

    // Disable cellular RF — GPS stays on via GNSS subsystem
    disableCellularRF();

    // Init WiFi for scanning
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    // Start first async scan
    WiFi.scanNetworks(true);
    scanInProgress = true;

    lastUploadTime = millis();

    // Enable hardware watchdog
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms     = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    wdtConfig.idle_core_mask |= (1 << 0);
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    wdtConfig.idle_core_mask |= (1 << 1);
#endif
#if CONFIG_ESP_TASK_WDT_INIT
    esp_task_wdt_reconfigure(&wdtConfig);
#else
    esp_task_wdt_init(&wdtConfig);
#endif
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL);

    SerialMon.println("Setup complete. Scanning...");
}

// ═══════════════════════════════════════════════
// loop()
// ═══════════════════════════════════════════════

void loop() {
    // Feed watchdog
    esp_task_wdt_reset();

    // Periodic reboot for long-term stability
    if (millis() - bootTime > REBOOT_INTERVAL_MS) {
        SerialMon.println("Scheduled reboot (6h)...");
        ESP.restart();
    }

    // ── Async WiFi scan state machine ──
    if (scanInProgress) {
        int result = WiFi.scanComplete();
        if (result == WIFI_SCAN_RUNNING) {
            // Still scanning, nothing to do
        } else if (result == WIFI_SCAN_FAILED || result < 0) {
            // Scan failed — clean up and restart
            WiFi.scanDelete();
            WiFi.scanNetworks(true);
        } else if (result >= 0) {
            // Scan complete with results
            if (result > 0) {
                processScanResults(result);
            } else {
                WiFi.scanDelete();
            }
            // Immediately start next scan
            WiFi.scanNetworks(true);
        }
    }

    // ── Check upload triggers ──
    unsigned long now = millis();
    bool timeToUpload  = (now - lastUploadTime >= UPLOAD_INTERVAL_MS);
    bool bufferFull    = (uploadBufferCount >= MAX_UPLOAD_BUFFER);

    if (timeToUpload || bufferFull) {
        performUploadWindow();
        lastUploadTime = millis();
    }

    delay(10); // Yield to system tasks
}
