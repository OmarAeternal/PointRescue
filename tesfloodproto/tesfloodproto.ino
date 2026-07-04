#include <SPI.h>
#include <LoRa.h>
#include <TinyGPS++.h>
#include <ArduinoJson.h>

// ============================================================
// KONFIGURASI — UBAH HANYA DI SINI
// ============================================================
#define DEVICE_ID        "001"   // Ganti "002" di device kedua
#define BROADCAST_INTERVAL_MS  5000   // Kirim lokasi sendiri tiap 5 detik

// --- PIN LORA SX1278 ---
#define LORA_SCK    5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_SS    18
#define LORA_RST   23
#define LORA_DI0   26
#define LORA_FREQ  433E6

// --- PIN GPS NEO-6M ---
#define GPS_RX_PIN  16   // ESP32 RX2 ← TX GPS
#define GPS_TX_PIN  17   // ESP32 TX2 → RX GPS
#define GPS_BAUD    9600

// ============================================================
// ANTI-FLOODING: Simpan packet ID yang sudah pernah diterima
// Mencegah re-broadcast paket yang sama (loop flooding)
// ============================================================
#define SEEN_CACHE_SIZE  20

struct SeenPacket {
  String   packetId;
  uint32_t timestamp;
};

SeenPacket seenCache[SEEN_CACHE_SIZE];
uint8_t    seenIndex = 0;

bool isPacketSeen(const String& pid) {
  for (int i = 0; i < SEEN_CACHE_SIZE; i++) {
    if (seenCache[i].packetId == pid) return true;
  }
  return false;
}

void markPacketSeen(const String& pid) {
  seenCache[seenIndex % SEEN_CACHE_SIZE] = { pid, millis() };
  seenIndex++;
}

// ============================================================
// TABEL NODE: Simpan data lokasi semua node yang pernah diterima
// ============================================================
#define MAX_NODES  10

struct NodeInfo {
  String   deviceId;
  float    latitude;
  float    longitude;
  float    altitude;
  float    speed;
  uint8_t  satellites;
  bool     gpsValid;
  uint32_t lastSeen;   // millis() saat terakhir diterima
};

NodeInfo knownNodes[MAX_NODES];
uint8_t  nodeCount = 0;

void updateNodeTable(const String& id, float lat, float lng,
                     float alt, float spd, uint8_t sats, bool valid) {
  for (int i = 0; i < nodeCount; i++) {
    if (knownNodes[i].deviceId == id) {
      knownNodes[i] = { id, lat, lng, alt, spd, sats, valid, millis() };
      return;
    }
  }
  if (nodeCount < MAX_NODES) {
    knownNodes[nodeCount++] = { id, lat, lng, alt, spd, sats, valid, millis() };
  }
}

void printNodeTable() {
  Serial.println(F("\n╔══════════════════════════════════════════════════════╗"));
  Serial.println(F("║              NODE TABLE (semua node dikenal)         ║"));
  Serial.println(F("╠══════════════════════════════════════════════════════╣"));
  for (int i = 0; i < nodeCount; i++) {
    NodeInfo& n = knownNodes[i];
    uint32_t ageSec = (millis() - n.lastSeen) / 1000;
    Serial.printf("║  [%s]  %s\n",
      n.deviceId.c_str(),
      (n.deviceId == DEVICE_ID) ? "← DEVICE INI" : "");
    Serial.printf("║    Lat: %.6f  Lng: %.6f\n", n.latitude, n.longitude);
    Serial.printf("║    Alt: %.1f m  Speed: %.2f m/s  Sats: %d\n",
      n.altitude, n.speed, n.satellites);
    Serial.printf("║    GPS Valid: %s  Last seen: %us ago\n",
      n.gpsValid ? "YES" : "NO (dummy)", ageSec);
    Serial.println(F("║  ─────────────────────────────────────────────────"));
  }
  Serial.println(F("╚══════════════════════════════════════════════════════╝\n"));
}

// ============================================================
// OBJEK GLOBAL
// ============================================================
TinyGPSPlus   gps;
HardwareSerial gpsSerial(2);

uint32_t lastBroadcastTime = 0;
uint32_t packetCounter     = 0;   // Untuk generate packet ID unik

// ============================================================
// BUAT DAN KIRIM PAKET LOKASI SENDIRI
// ============================================================
void broadcastOwnLocation() {
  JsonDocument doc;

  // Packet metadata
  packetCounter++;
  String packetId = String(DEVICE_ID) + "_" + String(packetCounter);
  doc["pid"]       = packetId;    // Packet ID (untuk deduplikasi flooding)
  doc["src"]       = DEVICE_ID;   // Sumber asli
  doc["hop"]       = 0;           // Jumlah hop (0 = langsung dari sumber)

  // Payload lokasi
  doc["device_id"] = DEVICE_ID;

  if (gps.location.isValid()) {
    doc["lat"]   = gps.location.lat();
    doc["lng"]   = gps.location.lng();
    doc["alt"]   = gps.altitude.meters();
    doc["spd"]   = gps.speed.mps();
    doc["sats"]  = gps.satellites.value();
    doc["valid"] = true;
  } else {
    // Data dummy — satelit belum fix
    doc["lat"]   = -7.953850;
    doc["lng"]   = 112.614955;
    doc["alt"]   = 535.5;
    doc["spd"]   = 0.52;
    doc["sats"]  = 8;
    doc["valid"] = false;
  }

  // Tandai sebagai sudah dilihat agar tidak re-broadcast milik sendiri
  markPacketSeen(packetId);

  // Update tabel node sendiri
  updateNodeTable(
    doc["device_id"].as<String>(),
    doc["lat"].as<float>(),
    doc["lng"].as<float>(),
    doc["alt"].as<float>(),
    doc["spd"].as<float>(),
    doc["sats"].as<uint8_t>(),
    doc["valid"].as<bool>()
  );

  // Serialisasi & kirim via LoRa
  String payload;
  serializeJson(doc, payload);

  LoRa.beginPacket();
  LoRa.print(payload);
  LoRa.endPacket();

  Serial.printf("[TX] Broadcast lokasi sendiri | PID: %s | Payload: %s\n",
    packetId.c_str(), payload.c_str());
}

// ============================================================
// TERIMA DAN FLOOD ULANG PAKET DARI NODE LAIN
// ============================================================
void handleIncomingPacket() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;

  // Baca raw string
  String raw = "";
  while (LoRa.available()) {
    raw += (char)LoRa.read();
  }
  int rssi = LoRa.packetRssi();

  Serial.printf("[RX] Raw: %s | RSSI: %d dBm\n", raw.c_str(), rssi);

  // Parse JSON
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) {
    Serial.printf("[RX] JSON error: %s — paket diabaikan\n", err.c_str());
    return;
  }

  // Validasi field wajib
  if (!doc["pid"].is<String>() || !doc["src"].is<String>()) {
    Serial.println(F("[RX] Paket tidak valid (tidak ada pid/src) — diabaikan"));
    return;
  }

  String packetId  = doc["pid"].as<String>();
  String srcId     = doc["src"].as<String>();
  int    hop       = doc["hop"].as<int>();

  // Abaikan paket dari diri sendiri
  if (srcId == DEVICE_ID) {
    Serial.println(F("[RX] Paket dari diri sendiri — diabaikan"));
    return;
  }

  // Cek apakah sudah pernah diterima (anti-loop flooding)
  if (isPacketSeen(packetId)) {
    Serial.printf("[RX] Paket duplikat PID: %s — tidak di-flood ulang\n", packetId.c_str());
    return;
  }
  markPacketSeen(packetId);

  // Update tabel node berdasarkan data yang diterima
  updateNodeTable(
    doc["device_id"].as<String>(),
    doc["lat"].as<float>(),
    doc["lng"].as<float>(),
    doc["alt"].as<float>(),
    doc["spd"].as<float>(),
    doc["sats"].as<uint8_t>(),
    doc["valid"].as<bool>()
  );

  Serial.printf("[RX] ✓ Data node [%s] diterima | Hop: %d | RSSI: %d\n",
    srcId.c_str(), hop, rssi);
  printNodeTable();

  // ── FLOODING: Broadcast ulang paket dengan hop + 1 ──
  doc["hop"] = hop + 1;

  String rePayload;
  serializeJson(doc, rePayload);

  // Delay kecil agar tidak tabrakan di udara (random 50–200ms)
  delay(random(50, 200));

  LoRa.beginPacket();
  LoRa.print(rePayload);
  LoRa.endPacket();

  Serial.printf("[FLOOD] Re-broadcast PID: %s (hop %d → %d)\n",
    packetId.c_str(), hop, hop + 1);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println(F("\n========================================"));
  Serial.printf("  LoRa GPS Flooding Node — ID: %s\n", DEVICE_ID);
  Serial.println(F("========================================\n"));

  // --- Inisialisasi GPS ---
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println(F("[GPS] Serial dimulai..."));

  // --- Inisialisasi LoRa ---
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DI0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println(F("[LORA] ✗ Gagal inisialisasi! Cek koneksi hardware."));
    while (true) { delay(1000); }
  }

  // Setting LoRa untuk jangkauan lebih jauh
  LoRa.setSpreadingFactor(10);       // SF10: jangkauan lebih jauh, lebih lambat
  LoRa.setSignalBandwidth(125E3);    // BW 125 kHz (standar)
  LoRa.setCodingRate4(5);            // CR 4/5
  LoRa.setTxPower(20);               // Max TX power (20 dBm)

  Serial.printf("[LORA] ✓ Siap @ %.0f MHz | SF10 | BW125 | CR4/5 | 20dBm\n",
    LORA_FREQ / 1E6);

  randomSeed(analogRead(0));   // Seed untuk delay anti-collision
  lastBroadcastTime = millis();

  Serial.println(F("\n[SYSTEM] Mulai operasi...\n"));
}

// ============================================================
// LOOP UTAMA
// ============================================================
void loop() {
  // 1) Feed data GPS ke parser TinyGPS++
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  // 2) Cek apakah sudah waktunya broadcast lokasi sendiri
  if (millis() - lastBroadcastTime >= BROADCAST_INTERVAL_MS) {
    lastBroadcastTime = millis();
    broadcastOwnLocation();
  }

  // 3) Cek paket masuk dari LoRa
  handleIncomingPacket();
}