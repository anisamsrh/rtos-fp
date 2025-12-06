#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PZEM004Tv30.h>
#include <ArduinoJson.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <secrets.h>
 
// --- OTA ---
const char* currentVersion = "1.0.2";
const char* versionURL = VERSION_URL;
const char* firmwareURL = FIRMWARE_URl;

// --- KONFIGURASI WIFI & SERVER ---
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;
const char* serverName = NODERED_URL;

// --- KONFIGURASI PZEM ---
#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17
#define PZEM_SERIAL Serial2
PZEM004Tv30 pzem(PZEM_SERIAL, PZEM_RX_PIN, PZEM_TX_PIN);

// --- KONFIGURASI RTOS ---
TaskHandle_t TaskSensorHandle;
TaskHandle_t TaskNetworkHandle;
QueueHandle_t sensorQueue;

// Struktur data untuk dikirim antar Task
struct PowerData {
  float voltage;
  float current;
  float power;
  float energy;
  float frequency;
  float pf;
};

void performFirmwareUpdate(WiFiClientSecure &client) {
  httpUpdate.onProgress([](int cur, int total) {
    Serial.printf("Progress: %d%%\n", (cur * 100) / total);
  });
  httpUpdate.setLedPin(2, LOW); 
  t_httpUpdate_return ret = httpUpdate.update(client, firmwareURL);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      break;

    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("HTTP_UPDATE_NO_UPDATES");
      break;

    case HTTP_UPDATE_OK:
      Serial.println("HTTP_UPDATE_OK");
      break;
  }
}

void checkFirmwareUpdate() {
  Serial.println("Checking for firmware update...");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  
  http.begin(client, versionURL);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String newVersion = http.getString();
    newVersion.trim();

    Serial.print("Current version: "); Serial.println(currentVersion);
    Serial.print("Server version: "); Serial.println(newVersion);

    if (newVersion > currentVersion) {
      Serial.println("New version found! Updating...");

      if (TaskSensorHandle != NULL) vTaskSuspend(TaskSensorHandle);
      if (TaskNetworkHandle != NULL) vTaskSuspend(TaskNetworkHandle);

      performFirmwareUpdate(client);

      if (TaskSensorHandle != NULL) vTaskResume(TaskSensorHandle);
      if (TaskNetworkHandle != NULL) vTaskResume(TaskNetworkHandle);
    } else {
      Serial.println("Device is up to date.");
    }
  } else {
    Serial.print("Failed to check version update, error: ");
    Serial.println(httpCode);
  }
  http.end();
}

// --- TASK 1 (Core 1) ---
void TaskReadSensor(void *pvParameters) {
  for (;;) {
    PowerData data;

    data.voltage = pzem.voltage();
    data.current = pzem.current();
    data.power = pzem.power();
    data.energy = pzem.energy();
    data.frequency = pzem.frequency();
    data.pf = pzem.pf();

    if (isnan(data.voltage)) data.voltage = 0.0;
    if (isnan(data.current)) data.current = 0.0;
    if (isnan(data.power)) data.power = 0.0;
    if (isnan(data.energy)) data.energy = 0.0;

    xQueueSend(sensorQueue, &data, (TickType_t)10);

    vTaskDelay(2000 / portTICK_PERIOD_MS); 
  }
}

// --- TASK 2 (Core 0) ---
void TaskSendToNodeRED(void *pvParameters) {
  PowerData receivedData;

  for (;;) {
    // check for wifi, reconnect if not
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.reconnect();
      Serial.println("Wifi not connected");
      Serial.println("reconnecting...");
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    if (xQueueReceive(sensorQueue, &receivedData, (TickType_t)5000) == pdPASS) {
      
      HTTPClient http;
      http.begin(serverName);
      http.addHeader("Content-Type", "application/json");

      JsonDocument doc;
      doc["voltage"] = receivedData.voltage;
      doc["current"] = receivedData.current;
      doc["power"] = receivedData.power;
      doc["energy"] = receivedData.energy;
      doc["frequency"] = receivedData.frequency;
      doc["pf"] = receivedData.pf;

      String requestBody;
      serializeJson(doc, requestBody);

      int httpResponseCode = http.POST(requestBody);

      if (httpResponseCode > 0) {
        Serial.print("Data sent successfully. Code: ");
        Serial.println(httpResponseCode);

        String responseBody = http.getString();
        JsonDocument responseDoc;
        DeserializationError error = deserializeJson(responseDoc, responseBody);

        if (!error) {
          if (responseDoc["ota_update"] == true) {
            Serial.println("Node-RED requested OTA Update!");
            http.end(); 
            
            checkFirmwareUpdate();
            continue; 
          }
        }
      } else {
        Serial.print("Error sending POST: ");
        Serial.println(httpResponseCode);
      }
      
      Serial.println("OTA OKE PART 2");
      http.end();
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Setup WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected");

  checkFirmwareUpdate();

  // Capacity 10
  sensorQueue = xQueueCreate(10, sizeof(PowerData));

  if (sensorQueue == NULL) {
    Serial.println("Failed to make Queue");
    while(1);
  }

  xTaskCreatePinnedToCore(TaskReadSensor, "ReadSensor", 4096, NULL, 1, &TaskSensorHandle, 1);
  xTaskCreatePinnedToCore(TaskSendToNodeRED, "SendNodeRED", 8192, NULL, 1, &TaskNetworkHandle, 0);
}

void loop() {
  vTaskDelete(NULL);
}