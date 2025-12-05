#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PZEM004Tv30.h>
#include <ArduinoJson.h>
#include <secrets.h>

// --- KONFIGURASI WIFI & SERVER ---
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;
// Ganti dengan IP komputer yang menjalankan Node-RED
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

// --- TASK 1: BACA SENSOR (Core 1) ---
void TaskReadSensor(void *pvParameters) {
  for (;;) {
    PowerData data;

    // Baca data dari PZEM
    data.voltage = pzem.voltage();
    data.current = pzem.current();
    data.power = pzem.power();
    data.energy = pzem.energy();
    data.frequency = pzem.frequency();
    data.pf = pzem.pf();

    // Validasi sederhana (jika NaN, beri nilai 0)
    if (isnan(data.voltage)) data.voltage = 0.0;
    if (isnan(data.current)) data.current = 0.0;
    if (isnan(data.power)) data.power = 0.0;
    if (isnan(data.energy)) data.energy = 0.0;

    // Kirim ke Queue (Tunggu maksimal 10 tick jika penuh)
    xQueueSend(sensorQueue, &data, (TickType_t)10);

    // Delay task selama 2 detik (Non-blocking)
    vTaskDelay(2000 / portTICK_PERIOD_MS); 
  }
}

// --- TASK 2: KIRIM KE NODE-RED (Core 0) ---
void TaskSendToNodeRED(void *pvParameters) {
  PowerData receivedData;

  for (;;) {
    // Cek status WiFi, reconnect jika putus
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.reconnect();
      Serial.println("Wifi not connected");
      Serial.println("reconnecting...");
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    // Tunggu data dari Queue
    if (xQueueReceive(sensorQueue, &receivedData, (TickType_t)5000) == pdPASS) {
      
      HTTPClient http;
      http.begin(serverName);
      http.addHeader("Content-Type", "application/json");

      // Buat JSON String
      JsonDocument doc;
      doc["voltage"] = receivedData.voltage;
      doc["current"] = receivedData.current;
      doc["power"] = receivedData.power;
      doc["energy"] = receivedData.energy;
      doc["frequency"] = receivedData.frequency;
      doc["pf"] = receivedData.pf;

      String requestBody;
      serializeJson(doc, requestBody);

      // Kirim POST Request
      int httpResponseCode = http.POST(requestBody);

      if (httpResponseCode > 0) {
        Serial.print("Data Terkirim. Code: ");
        Serial.println(httpResponseCode);
      } else {
        Serial.print("Error sending POST: ");
        Serial.println(httpResponseCode);
      }
      
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

  // Buat Queue (Kapasitas 10 item struct PowerData)
  sensorQueue = xQueueCreate(10, sizeof(PowerData));

  if (sensorQueue == NULL) {
    Serial.println("Gagal membuat Queue");
    while(1);
  }

  // Buat Task
  // Task Sensor di Core 1 (User app default)
  xTaskCreatePinnedToCore(TaskReadSensor, "ReadSensor", 4096, NULL, 1, &TaskSensorHandle, 1);
  
  // Task Network di Core 0 (Biasanya untuk WiFi/System radio)
  xTaskCreatePinnedToCore(TaskSendToNodeRED, "SendNodeRED", 8192, NULL, 1, &TaskNetworkHandle, 0);
}

void loop() {
  // Loop kosong karena semua logic ada di Tasks
  vTaskDelete(NULL);
}