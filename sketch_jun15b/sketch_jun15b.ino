#include <SPI.h>
#include <LoRa.h>
#include <TinyGPS++.h>
#include <ArduinoJson.h>

// --- KONFIGURASI PIN LORA SX1278 ---
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_SS 18
#define LORA_RST 23
#define LORA_DI0 26
#define LORA_FREQ 433E6 // Sesuai dengan modul fisik 433 MHz

// --- KONFIGURASI PIN GPS ---
#define GPS_RX_PIN 16 // ESP32 RX membaca TX GPS
#define GPS_TX_PIN 17 // ESP32 TX mengirim ke RX GPS

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println("\n--- Memulai Sistem Tracker LoRa ---");

  // 1. Mulai Serial GPS
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  // 2. Mulai Modul LoRa
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DI0);
  
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("[ERROR] Modul LoRa gagal dimulai. Cek jalur kabel SPI!");
    while (1); // Berhenti di sini jika LoRa gagal
  }
  Serial.println("[OK] Modul LoRa SX1278 Siap!");
  Serial.println("[OK] Menunggu data GPS...\n");
}

void loop() {
  // Membaca data GPS terus-menerus di background
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // Atur pengiriman data ke LoRa setiap 5 detik
  static unsigned long lastTransmit = 0;
  if (millis() - lastTransmit > 5000) {
    lastTransmit = millis();

    // Buat wadah JSON menggunakan ArduinoJson v7
    JsonDocument doc;
    doc["device_id"] = "001";

    // Masukkan data asli jika satelit fix, atau data dummy jika belum fix
    if (gps.location.isValid()) {
      doc["latitude"] = gps.location.lat();
      doc["longitude"] = gps.location.lng();
      doc["altitude"] = gps.altitude.meters();
      doc["speed"] = gps.speed.mps();
      doc["satellites"] = gps.satellites.value();
    } else {
      // Data fallback sesuai permintaan
      doc["latitude"] = -7.953850;
      doc["longitude"] = 112.614955;
      doc["altitude"] = 535.5;
      doc["speed"] = 0.52;
      doc["satellites"] = 8;
    }

    // Ubah format JSON menjadi String teks murni
    String payload;
    serializeJson(doc, payload);

    // Tampilkan di layar laptop
    Serial.print("Mengirim: ");
    Serial.println(payload);

    // Pancarkan ke udara via antena LoRa
    LoRa.beginPacket();
    LoRa.print(payload);
    LoRa.endPacket();
  }
}