#include <SPI.h>
#include <LoRa.h>
#include <TinyGPS++.h>
#include <ArduinoJson.h>

// ============================================================
// IDENTITAS NODE — UBAH HANYA INI PER DEVICE
// Node ID = (Role x 1000) + Nomor Device
//   Role 0 = Gateway  → contoh: 0000
//   Role 1 = Tim SAR  → contoh: 1001, 1002, ...
//   Role 2 = Korban   → contoh: 2007, 2008, ...
// ============================================================
#define NODE_ID   2001
#define NET_ID    "PR01"   // HARUS sama di semua device 1 jaringan

// --- PIN LORA SX1278 (SPI) ---
#define LORA_SCK   5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_SS   18
#define LORA_RST  23
#define LORA_DI0  26
#define LORA_FREQ 433E6

// --- PIN GPS M10 (UART2) ---
// Pin 16 (RX2 ESP32) ← TX GPS | Pin 17 (TX2 ESP32) → RX GPS
#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
#define GPS_BAUD   9600

#define TRACKING_INTERVAL_MS   5000    // Broadcast lokasi (dengan GPS)
#define HEARTBEAT_INTERVAL_MS  10000   // Broadcast "masih hidup" (tanpa GPS)

// ============================================================
// ROLE & PACKET TYPE
// ============================================================
enum Role : uint8_t { ROLE_GATEWAY = 0, ROLE_SAR = 1, ROLE_VICTIM = 2 };
enum PacketType : uint8_t { PKT_HEARTBEAT = 0, PKT_TRACKING = 1, PKT_SOS = 2 };

const uint8_t MY_ROLE = NODE_ID / 1000;   // Diturunkan otomatis dari NODE_ID

uint8_t hopLimitFor(PacketType t) {           // Hop limit adaptif berdasar JENIS PAKET
  switch (t) {
    case PKT_HEARTBEAT: return 1;
    case PKT_TRACKING:  return 3;
    case PKT_SOS:       return 5;
  }
  return 3;
}

uint8_t priorityFor(PacketType t) {           // Prioritas berdasar JENIS PAKET
  switch (t) {
    case PKT_SOS:       return 2;   // Tertinggi
    case PKT_TRACKING:  return 1;
    case PKT_HEARTBEAT: return 0;   // Terendah
  }
  return 0;
}

bool canRelay(uint8_t role) {                 // Hak relay berdasar ROLE
  return role != ROLE_VICTIM;                 // Korban tidak relay
}

void delayRangeFor(uint8_t role, uint16_t &minMs, uint16_t &maxMs) {
  switch (role) {
    case ROLE_GATEWAY: minMs = 200; maxMs = 300; break;
    case ROLE_SAR:      minMs = 400; maxMs = 700; break;
    case ROLE_VICTIM:   minMs = 700; maxMs = 900; break;
    default:            minMs = 400; maxMs = 700; break;
  }
}

const char* roleName(uint8_t role) {
  switch (role) {
    case ROLE_GATEWAY: return "GATEWAY";
    case ROLE_SAR:      return "SAR";
    case ROLE_VICTIM:   return "KORBAN";
  }
  return "UNKNOWN";
}

const char* typeName(PacketType t) {
  switch (t) {
    case PKT_HEARTBEAT: return "HEARTBEAT";
    case PKT_TRACKING:  return "TRACKING";
    case PKT_SOS:       return "SOS";
  }
  return "UNKNOWN";
}

// ============================================================
// DUPLICATE DETECTION + TTL housekeeping
// pid = "<node_id>-<seq>" → unik tanpa perlu ID acak terpisah
// ============================================================
#define SEEN_CACHE_SIZE   30
#define SEEN_CACHE_TTL_MS 30000

struct SeenEntry { String pid; uint32_t seenAt; bool used = false; };
SeenEntry seenCache[SEEN_CACHE_SIZE];

bool isPacketSeen(const String& pid) {
  uint32_t now = millis();
  for (int i = 0; i < SEEN_CACHE_SIZE; i++) {
    if (seenCache[i].used && (now - seenCache[i].seenAt) < SEEN_CACHE_TTL_MS
        && seenCache[i].pid == pid) return true;
  }
  return false;
}

void markPacketSeen(const String& pid) {
  uint32_t now = millis();
  for (int i = 0; i < SEEN_CACHE_SIZE; i++) {
    if (!seenCache[i].used || (now - seenCache[i].seenAt) >= SEEN_CACHE_TTL_MS) {
      seenCache[i] = { pid, now, true };
      return;
    }
  }
  int oldest = 0;
  for (int i = 1; i < SEEN_CACHE_SIZE; i++)
    if (seenCache[i].seenAt < seenCache[oldest].seenAt) oldest = i;
  seenCache[oldest] = { pid, now, true };
}

// ============================================================
// PRIORITY TX QUEUE
// ============================================================
#define TX_QUEUE_SIZE 8
struct QueuedPacket { String payload; uint8_t priority; uint32_t queuedAt; bool used = false; };
QueuedPacket txQueue[TX_QUEUE_SIZE];
uint32_t nextTxAllowedAt = 0;

bool enqueueTx(const String& payload, uint8_t priority) {
  for (int i = 0; i < TX_QUEUE_SIZE; i++) {
    if (!txQueue[i].used) { txQueue[i] = { payload, priority, millis(), true }; return true; }
  }
  Serial.println(F("[QUEUE] Penuh — paket dibuang"));
  return false;
}

int pickHighestPriorityIdx() {
  int best = -1;
  for (int i = 0; i < TX_QUEUE_SIZE; i++) {
    if (!txQueue[i].used) continue;
    if (best == -1 || txQueue[i].priority > txQueue[best].priority ||
       (txQueue[i].priority == txQueue[best].priority && txQueue[i].queuedAt < txQueue[best].queuedAt))
      best = i;
  }
  return best;
}

void processTxQueue() {
  if (millis() < nextTxAllowedAt) return;
  int idx = pickHighestPriorityIdx();
  if (idx == -1) return;

  LoRa.beginPacket();
  LoRa.print(txQueue[idx].payload);
  LoRa.endPacket();

  Serial.printf("[TX] (prio %d) %s\n", txQueue[idx].priority, txQueue[idx].payload.c_str());
  txQueue[idx].used = false;

  uint16_t minMs, maxMs;
  delayRangeFor(MY_ROLE, minMs, maxMs);
  nextTxAllowedAt = millis() + random(minMs, maxMs + 1);   // Adaptive delay + backoff jadi satu
}

// ============================================================
// GPS & LORA OBJECTS
// ============================================================
TinyGPSPlus    gps;
HardwareSerial gpsSerial(2);
uint32_t seqCounter = 0;

// ============================================================
// TABEL NODE (monitoring via Serial)
// ============================================================
#define MAX_NODES 15
struct NodeInfo {
  int id; uint8_t role; PacketType lastType;
  float lat, lng, alt, spd; int sats; bool gpsValid;
  uint32_t lastSeen; bool used = false;
};
NodeInfo nodeTable[MAX_NODES];

void updateNodeTable(int id, uint8_t role, PacketType type,
                      float lat, float lng, float alt, float spd, int sats, bool valid) {
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodeTable[i].used && nodeTable[i].id == id) {
      nodeTable[i] = { id, role, type, lat, lng, alt, spd, sats, valid, millis(), true };
      return;
    }
  }
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodeTable[i].used) {
      nodeTable[i] = { id, role, type, lat, lng, alt, spd, sats, valid, millis(), true };
      return;
    }
  }
}

// ============================================================
// BUILD & ORIGINATE PACKET
// ============================================================
String buildPacket(PacketType type, bool includeGPS) {
  JsonDocument doc;
  seqCounter++;
  doc["net"]  = NET_ID;
  doc["id"]   = NODE_ID;
  doc["seq"]  = seqCounter;
  doc["type"] = (int)type;
  doc["hop"]  = 0;

  if (includeGPS) {
    if (gps.location.isValid()) {
      doc["lat"] = gps.location.lat();
      doc["lng"] = gps.location.lng();
      doc["alt"] = gps.altitude.meters();
      doc["spd"] = gps.speed.mps();
      doc["sats"] = gps.satellites.value();
      doc["valid"] = true;
    } else {
      doc["lat"] = -7.953850; doc["lng"] = 112.614955;
      doc["alt"] = 535.5; doc["spd"] = 0.0; doc["sats"] = 0;
      doc["valid"] = false;
    }
  }

  String out;
  serializeJson(doc, out);
  markPacketSeen(String(NODE_ID) + "-" + String(seqCounter));
  return out;
}

void originatePacket(PacketType type, bool includeGPS) {
  String payload = buildPacket(type, includeGPS);
  enqueueTx(payload, priorityFor(type));
}

void triggerSOS() {
  Serial.println(F("\n[SOS] !!! TRIGGER SOS !!!\n"));
  originatePacket(PKT_SOS, true);
}

// ============================================================
// VALIDASI PAKET MASUK
// ============================================================
bool validatePacket(JsonDocument& doc) {
  if (!doc["net"].is<const char*>() || !doc["id"].is<int>() ||
      !doc["seq"].is<int>() || !doc["type"].is<int>() || !doc["hop"].is<int>()) return false;

  if (String(doc["net"].as<const char*>()) != NET_ID) return false;   // Beda jaringan → abaikan

  int id = doc["id"].as<int>();
  if (id < 0 || id > 2999) return false;

  int type = doc["type"].as<int>();
  if (type < 0 || type > 2) return false;

  int hop = doc["hop"].as<int>();
  if (hop < 0 || hop > 10) return false;

  return true;
}

// ============================================================
// HANDLE PAKET MASUK
// ============================================================
void handleIncomingPacket() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return;

  String raw;
  while (LoRa.available()) raw += (char)LoRa.read();
  int rssi = LoRa.packetRssi();

  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
  if (!validatePacket(doc)) return;

  int        originId = doc["id"].as<int>();
  uint32_t   seq       = doc["seq"].as<uint32_t>();
  PacketType type      = (PacketType)doc["type"].as<int>();
  int        hop       = doc["hop"].as<int>();

  if (originId == NODE_ID) return;   // Paket sendiri yang muter balik

  String pid = String(originId) + "-" + String(seq);
  if (isPacketSeen(pid)) return;     // Duplikat
  markPacketSeen(pid);

  uint8_t originRole = originId / 1000;
  Serial.printf("[RX] dari %d (%s) | %s | hop %d | RSSI %d\n",
    originId, roleName(originRole), typeName(type), hop, rssi);

  if (type == PKT_SOS) Serial.printf("  ⚠️  SOS DITERIMA dari node %d !!\n", originId);

  if (doc["lat"].is<float>() && doc["lng"].is<float>()) {
    updateNodeTable(originId, originRole, type,
      doc["lat"].as<float>(), doc["lng"].as<float>(),
      doc["alt"] | 0.0f, doc["spd"] | 0.0f, doc["sats"] | 0, doc["valid"] | false);
  }

  if (!canRelay(MY_ROLE)) return;    // Korban: tidak relay

  int maxHop = hopLimitFor(type);
  if (hop + 1 > maxHop) {
    Serial.println(F("  [RELAY] Hop limit tercapai — stop"));
    return;
  }

  doc["hop"] = hop + 1;
  String rePayload;
  serializeJson(doc, rePayload);
  enqueueTx(rePayload, priorityFor(type));
  Serial.printf("  [RELAY] hop %d → %d, prio %d\n", hop, hop + 1, priorityFor(type));
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println(F("\n========================================"));
  Serial.printf("  Node ID   : %d\n", NODE_ID);
  Serial.printf("  Role      : %s\n", roleName(MY_ROLE));
  Serial.printf("  Net ID    : %s\n", NET_ID);
  Serial.printf("  Can Relay : %s\n", canRelay(MY_ROLE) ? "YA" : "TIDAK");
  Serial.println(F("========================================\n"));

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DI0);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println(F("[LORA] Gagal init! Cek wiring."));
    while (true) delay(1000);
  }

  LoRa.setSpreadingFactor(10);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setTxPower(20);

  Serial.println(F("[LORA] Siap.\n"));
  randomSeed(analogRead(0));
}

// ============================================================
// LOOP
// ============================================================
uint32_t lastTracking = 0;
uint32_t lastHeartbeat = 0;

void loop() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  uint32_t now = millis();

  if (now - lastTracking >= TRACKING_INTERVAL_MS) {
    lastTracking = now;
    originatePacket(PKT_TRACKING, true);
  }

  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = now;
    originatePacket(PKT_HEARTBEAT, false);
  }

  // Ketik "SOS" di Serial Monitor untuk simulasi trigger (belum ada tombol fisik)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("SOS")) triggerSOS();
  }

  handleIncomingPacket();
  processTxQueue();
}