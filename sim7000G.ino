#define TINY_GSM_MODEM_SIM7000
#define TINY_GSM_RX_BUFFER 1024 // Set RX buffer to 1Kb

#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <set>
#include <queue>
#include <algorithm>
#include <esp_timer.h>
#include <Adafruit_Sensor.h>
#include <DHT22.h>

// LilyGO T-SIM7000G Pinout
#define UART_BAUD 115200
#define PIN_TX 27
#define PIN_RX 26
#define PWR_PIN 4
#define LED_PIN 12
#define ADC_PIN 35


// DHT22 GPIO
#define DHTPIN 33
#define DHTTYPE DHT22
DHT22 dht(33);

#define MAX_CONNECTION_ATTEMPTS 5

#define SerialMon Serial
#define SerialAT Serial1

// Your GPRS credentials
const char apn[] = "";  // Search for this one, it is required
const char gprsUser[] = "";
const char gprsPass[] = "";

// Server details
const char server[] = "x.x.x.x";
const int port = 5000;

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
HttpClient http(client, server, port);

// Set to store unique MAC addresses
std::set<String> seenMacs;
// Queue to keep track of the order of MAC addresses
std::queue<String> macOrder;
const size_t MAX_MACS = 500;

// Heartbeat variables
unsigned long lastHeartbeatTime = 0;
const unsigned long heartbeatInterval = 5 * 60 * 1000; // 5 minutes in milliseconds

// Reboot timer
esp_timer_handle_t reboot_timer;

void IRAM_ATTR rebootNow(void* arg) {
  esp_restart();
}

void setup() {
  SerialMon.begin(115200);
  SerialMon.println("Starting up...");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  pinMode(PWR_PIN, OUTPUT);
  digitalWrite(PWR_PIN, HIGH);
  delay(300);
  digitalWrite(PWR_PIN, LOW);

  delay(1000);

  SerialAT.begin(UART_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  SerialMon.println("Initializing modem...");
  if (!modem.restart()) {
    SerialMon.println("Failed to restart modem, attempting to continue without restarting");
  }

  String modemInfo = modem.getModemInfo();
  SerialMon.print("Modem Info: ");
  SerialMon.println(modemInfo);

  // Set up reboot timer (1 hour)
  const esp_timer_create_args_t periodic_timer_args = {
    .callback = &rebootNow,
    .name = "reboot"
  };
  ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &reboot_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(reboot_timer, 3600000000)); // 1 hour in microseconds

  SerialMon.print("Waiting for network...");
  if (!modem.waitForNetwork()) {
    SerialMon.println(" fail");
    delay(10000);
    return;
  }
  SerialMon.println(" success");

  if (modem.isNetworkConnected()) {
    SerialMon.println("Network connected");
  }

  SerialMon.print(F("Connecting to "));
  SerialMon.print(apn);
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println(" fail");
    delay(10000);
    return;
  }
  SerialMon.println(" success");

  if (modem.isGprsConnected()) {
    SerialMon.println("GPRS connected");
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Turn on GPS power
  SerialMon.println("Turning on GPS power...");
  modem.sendAT("+SGPIO=0,4,1,1");
  if (modem.waitResponse(10000L) != 1) {
    SerialMon.println("Failed to turn on GPS power");
  }

  // Enable GPS
  SerialMon.println("Enabling GPS...");
  modem.enableGPS();

  SerialMon.println("Setup completed.");
}

void loop() {
  if (!modem.isNetworkConnected()) {
    SerialMon.println("Network disconnected. Reconnecting...");
    if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
      SerialMon.println(" fail");
      delay(10000);
      return;
    }
  }

  SerialMon.println("Starting WiFi scan...");
  WiFi.scanDelete();

  // WiFi.scanNetworks(async, show_hidden, passive_scan, max_ms_per_channel)
  int n = WiFi.scanNetworks(false, true, false, 110);

  if (n == 0) {
    SerialMon.println("No networks found");
  } else {
    SerialMon.printf("Found %d networks\n", n);
    delay(1000); // delay 1s between scans
    for (int i = 0; i < n; ++i) {
      String mac = WiFi.BSSIDstr(i);

      bool isNewNetwork = seenMacs.find(mac) == seenMacs.end();

      if (isNewNetwork) {
        SerialMon.printf("Processing new network %d: %s\n", i + 1, mac.c_str());

        // If we've reached MAX_MACS, remove the oldest one
        if (seenMacs.size() >= MAX_MACS) {
          String oldestMac = macOrder.front();
          macOrder.pop();
          seenMacs.erase(oldestMac);
          SerialMon.printf("Removed oldest network: %s\n", oldestMac.c_str());
        }

        // Add the new MAC to our set and queue
        seenMacs.insert(mac);
        macOrder.push(mac);

        JsonDocument doc = DynamicJsonDocument(1024);
        doc["mac"] = mac;
        doc["ssid"] = WiFi.SSID(i);
        doc["auth_mode"] = getAuthMode(WiFi.encryptionType(i));
        doc["channel"] = WiFi.channel(i);
        doc["rssi"] = WiFi.RSSI(i);

        // Try to get GPS data
        float lat, lon, speed, alt, accuracy;
        int vsat, usat;
        int year, month, day, hour, minute, second;

        if (getGPS(&lat, &lon, &speed, &alt, &vsat, &usat, &accuracy, &year, &month, &day, &hour, &minute, &second)) {
          doc["latitude"] = lat;
          doc["longitude"] = lon;
          doc["altitude"] = alt;
          doc["accuracy"] = accuracy;
        }

        String payload;
        serializeJson(doc, payload);

        SerialMon.println("Sending data to server...");
        http.beginRequest();
        http.post("/api/wigle_data");
        http.sendHeader("Content-Type", "application/json");
        http.sendHeader("Content-Length", payload.length());
        http.beginBody();
        http.print(payload);
        http.endRequest();

        int statusCode = http.responseStatusCode();
        String response = http.responseBody();

        SerialMon.print("Status code: ");
        SerialMon.println(statusCode);
        SerialMon.print("Response: ");
        SerialMon.println(response);
      } else {
        SerialMon.printf("Skipping known network %d: %s\n", i + 1, mac.c_str());
      }
    }
  }

  // Check if it's time to send a heartbeat
  unsigned long currentTime = millis();
  if (currentTime - lastHeartbeatTime >= heartbeatInterval) {
    sendHeartbeat();
    lastHeartbeatTime = currentTime;
  }

  SerialMon.println("Waiting before next scan...");
  delay(3000);
}

bool getGPS(float *lat, float *lon, float *speed, float *alt, int *vsat, int *usat, float *accuracy,
            int *year, int *month, int *day, int *hour, int *minute, int *second) {
  SerialMon.println("Getting GPS data...");
  if (modem.getGPS(lat, lon, speed, alt, vsat, usat, accuracy, year, month, day, hour, minute, second)) {
    SerialMon.println("GPS data acquired successfully.");
    SerialMon.printf("Lat: %f, Lon: %f, Alt: %f, Acc: %f\n", *lat, *lon, *alt, *accuracy);
    return true;
  } else {
    SerialMon.println("Couldn't get GPS location.");
    return false;
  }
}

bool connectGPRS() {
  int attempts = 0;

  // Make sure we're connected to the network first
  SerialMon.print("Waiting for network...");
  while (!modem.waitForNetwork(60000L) && attempts < MAX_CONNECTION_ATTEMPTS) {
    SerialMon.print(".");
    attempts++;
    delay(5000);
  }

  if (attempts >= MAX_CONNECTION_ATTEMPTS) {
    SerialMon.println(" fail");
    SerialMon.println("Network connection failed after multiple attempts");
    modemPowerCycle();
    return false;
  }
  SerialMon.println(" success");

  if (modem.isNetworkConnected()) {
    SerialMon.println("Network connected");
  }

  // Now try GPRS connection
  SerialMon.print(F("Connecting to "));
  SerialMon.print(apn);
  attempts = 0;

  while (!modem.gprsConnect(apn, gprsUser, gprsPass) && attempts < MAX_CONNECTION_ATTEMPTS) {
    SerialMon.print(".");
    attempts++;
    delay(5000);
  }

  if (attempts >= MAX_CONNECTION_ATTEMPTS) {
    SerialMon.println(" fail");
    SerialMon.println("GPRS connection failed after multiple attempts");
    return false;
  }

  SerialMon.println(" success");

  if (modem.isGprsConnected()) {
    SerialMon.println("GPRS connected");
    return true;
  } else {
    SerialMon.println("GPRS status check failed");
    return false;
  }
}

void modemPowerCycle() {
  SerialMon.println("Power cycling modem...");

  // Power off
  digitalWrite(PWR_PIN, HIGH);
  delay(1500);  // At least 1.2s according to datasheet

  // Power on
  digitalWrite(PWR_PIN, LOW);
  delay(300);
  digitalWrite(PWR_PIN, HIGH);

  delay(3000);  // Wait for modem to initialize
}

String getAuthMode(wifi_auth_mode_t encryptionType) {
  switch (encryptionType) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    default: return "unknown";
  }
}

float getBatteryVoltage() {
  uint16_t v = analogRead(ADC_PIN);
  float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (1100 / 1000.0);
  return battery_voltage;
}

void readDHT22(float &temperature, float &humidity) {
  temperature = dht.getTemperature();
  humidity = dht.getHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    SerialMon.println("Failed to read from DHT22 sensor!");
    temperature = -1;
    humidity = -1;
  } else {
    SerialMon.printf("Temperature: %.2f °C, Humidity: %.2f %%\n", temperature, humidity);
  }
}


void sendHeartbeat() {
  SerialMon.println("Sending heartbeat...");

  // Read DHT22 data
  float temperature = 0.0;
  float humidity = 0.0;
  readDHT22(temperature, humidity);

  // Prepare JSON payload
  JsonDocument doc = DynamicJsonDocument(1024);
  doc["type"] = "heartbeat";
  doc["mac"] = WiFi.macAddress();
  doc["battery"] = getBatteryVoltage();
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;

  String payload;
  serializeJson(doc, payload);

  // Send HTTP POST request
  http.beginRequest();
  http.post("/api/heartbeat");
  http.sendHeader("Content-Type", "application/json");
  http.sendHeader("Content-Length", payload.length());
  http.beginBody();
  http.print(payload);
  http.endRequest();

  int statusCode = http.responseStatusCode();
  String response = http.responseBody();

  SerialMon.print("Heartbeat status code: ");
  SerialMon.println(statusCode);
  SerialMon.print("Heartbeat response: ");
  SerialMon.println(response);
}