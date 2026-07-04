"""
serial_listener.py — Point Rescue GPS Serial Listener
======================================================
Membaca output serial dari ESP32 (LoRa node), mengekstrak
JSON payload GPS, dan menulis ke gps.json agar bisa
dibaca oleh dashboard HTML.

Cara pakai:
    pip install pyserial
    python serial_listener.py --port COM3 --baud 115200

    # Linux/Mac:
    python serial_listener.py --port /dev/ttyUSB0 --baud 115200

    # Scan port otomatis (tidak perlu --port):
    python serial_listener.py
"""

import serial
import serial.tools.list_ports
import json
import argparse
import sys
import time
import re
from pathlib import Path

# ── Konfigurasi ──────────────────────────────────────────────
GPS_JSON_PATH = Path(__file__).parent / "gps.json"
OFFLINE_TIMEOUT = 15          # detik — hapus device dari JSON jika tidak ada update
BAUD_RATE_DEFAULT = 115200
NET_ID = "PR01"               # HARUS sama dengan NET_ID di firmware ESP32 — paket beda net diabaikan

# ── Packet type dari firmware (lihat enum PacketType di ESP32) ──
PKT_HEARTBEAT = 0
PKT_TRACKING  = 1
PKT_SOS       = 2

# ── Role dari Node ID: id / 1000 (lihat enum Role di ESP32) ──
ROLE_NAMES = {0: "GATEWAY", 1: "SAR", 2: "KORBAN"}

# ── Field mapping: key pendek firmware → key panjang yang dipakai dashboard ──
# Dashboard membaca: device_id, latitude, longitude, altitude, speed, satellites
FIELD_MAP = {
    "lat":   "latitude",
    "lng":   "longitude",
    "alt":   "altitude",
    "spd":   "speed",
    "sats":  "satellites",
    "valid": "gps_valid",
}

# ── State: simpan data terakhir tiap device ───────────────────
node_table: dict[str, dict] = {}   # { device_id: { ...data, "_last_seen": timestamp } }


def scan_ports() -> list[str]:
    """Kembalikan daftar port serial yang tersedia."""
    ports = serial.tools.list_ports.comports()
    return [p.device for p in ports]


def pick_port(preferred: str | None) -> str:
    """Pilih port: gunakan preferred jika ada, atau tanyakan ke user."""
    available = scan_ports()

    if not available:
        print("[ERROR] Tidak ada port serial yang ditemukan.")
        print("        Pastikan ESP32 terhubung dan driver terinstall.")
        sys.exit(1)

    if preferred:
        if preferred in available:
            return preferred
        print(f"[WARN]  Port '{preferred}' tidak ditemukan.")

    if len(available) == 1:
        print(f"[AUTO]  Menggunakan satu-satunya port: {available[0]}")
        return available[0]

    print("\n[INFO]  Port serial yang tersedia:")
    for i, p in enumerate(available):
        print(f"         [{i}] {p}")
    while True:
        try:
            idx = int(input("Pilih nomor port: "))
            return available[idx]
        except (ValueError, IndexError):
            print("       Masukkan nomor yang valid.")


def parse_payload(raw: str) -> dict | None:
    """
    Cari dan parse JSON dari satu baris serial.
    Firmware mengeluarkan banyak log ([RX], [TX], [RELAY], dst);
    kita cari substring JSON {...} di baris manapun.
    """
    match = re.search(r'\{.*\}', raw)
    if not match:
        return None

    try:
        obj = json.loads(match.group())
    except json.JSONDecodeError:
        return None

    # Paket firmware baru wajib punya: net, id, seq, type, hop
    required = ("net", "id", "seq", "type", "hop")
    if not all(k in obj for k in required):
        return None

    # Abaikan paket dari jaringan lain
    if obj.get("net") != NET_ID:
        return None

    return obj


def normalize(raw: dict) -> dict | None:
    """
    Konversi payload firmware baru ke format yang dipakai dashboard.

    Firmware: {"net","id","seq","type","hop","lat","lng","alt","spd","sats","valid"}
    Dashboard butuh: device_id, latitude, longitude, altitude, speed, satellites

    Heartbeat (type=0) tidak membawa GPS → tidak dijadikan update posisi,
    supaya marker di peta tidak ditarik ke koordinat dummy/kosong.
    """
    pkt_type = raw.get("type")

    if pkt_type == PKT_HEARTBEAT:
        return None   # Tidak ada data lokasi untuk ditampilkan di peta

    node_id = raw["id"]
    role = ROLE_NAMES.get(node_id // 1000, "UNKNOWN")

    out = {"device_id": f"{role}-{node_id}"}
    for src_key, dst_key in FIELD_MAP.items():
        if src_key in raw:
            out[dst_key] = raw[src_key]

    out["_role"] = role
    out["_seq"] = raw.get("seq")
    out["_is_sos"] = (pkt_type == PKT_SOS)

    return out


def write_gps_json() -> None:
    """
    Tulis node_table ke gps.json sebagai array.
    Hanya tulis device yang masih 'online' (seen dalam OFFLINE_TIMEOUT detik).
    """
    now = time.time()
    active = [
        {k: v for k, v in data.items() if not k.startswith("_")}
        for data in node_table.values()
        if now - data.get("_last_seen", 0) < OFFLINE_TIMEOUT
    ]

    GPS_JSON_PATH.write_text(json.dumps(active, indent=2), encoding="utf-8")


def listen(port: str, baud: int) -> None:
    """Loop utama: baca serial, parse, update node_table, tulis JSON."""
    print(f"\n[START] Membuka {port} @ {baud} baud...")
    print(f"[INFO]  Output → {GPS_JSON_PATH.resolve()}")
    print(f"[INFO]  Tekan Ctrl+C untuk berhenti.\n")

    # Inisialisasi gps.json kosong
    GPS_JSON_PATH.write_text("[]", encoding="utf-8")

    try:
        ser = serial.Serial(port, baud, timeout=2)
    except serial.SerialException as e:
        print(f"[ERROR] Gagal membuka port: {e}")
        sys.exit(1)

    time.sleep(2)   # Beri waktu ESP32 reset setelah koneksi
    ser.reset_input_buffer()

    print("[READY] Mendengarkan data GPS dari ESP32...\n")

    while True:
        try:
            raw_bytes = ser.readline()
            if not raw_bytes:
                continue

            line = raw_bytes.decode("utf-8", errors="replace").strip()
            if not line:
                continue

            # Tampilkan semua output serial (opsional — komen jika terlalu ramai)
            print(f"[SERIAL] {line}")

            payload = parse_payload(line)
            if payload is None:
                continue   # Bukan baris JSON valid / beda net, skip

            normalized = normalize(payload)
            if normalized is None:
                continue   # Heartbeat — tidak ada lokasi, skip

            dev_id = normalized["device_id"]

            # Buang paket yang urutannya lebih lama dari yang sudah tersimpan
            # (bisa terjadi karena flooding — paket sama sampai lewat jalur berbeda)
            prev = node_table.get(dev_id)
            if prev and normalized.get("_seq") is not None and prev.get("_seq") is not None:
                if normalized["_seq"] <= prev["_seq"]:
                    continue

            if normalized.get("_is_sos"):
                print(f"  ⚠️  SOS diterima dari {dev_id} !!")

            # Update node table
            node_table[dev_id] = {**normalized, "_last_seen": time.time()}

            write_gps_json()

            print(f"  → [{dev_id}] lat={normalized.get('latitude')}, "
                  f"lng={normalized.get('longitude')}, "
                  f"sats={normalized.get('satellites')}, "
                  f"valid={normalized.get('gps_valid')}")

        except serial.SerialException as e:
            print(f"\n[ERROR] Koneksi serial terputus: {e}")
            print("[INFO]  Mencoba reconnect dalam 3 detik...")
            time.sleep(3)
            try:
                ser.close()
                ser = serial.Serial(port, baud, timeout=2)
                print("[OK]    Reconnect berhasil.\n")
            except serial.SerialException:
                print("[ERROR] Reconnect gagal. Coba cabut-pasang USB.\n")

        except KeyboardInterrupt:
            print("\n[STOP]  Dihentikan oleh user.")
            ser.close()
            sys.exit(0)


# ── Entry point ───────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Point Rescue — Serial listener untuk ESP32 LoRa GPS node"
    )
    parser.add_argument(
        "--port", "-p",
        help="Port serial (misal: COM3 atau /dev/ttyUSB0). "
             "Kosongkan untuk pilih otomatis.",
        default=None,
    )
    parser.add_argument(
        "--baud", "-b",
        help=f"Baud rate (default: {BAUD_RATE_DEFAULT})",
        type=int,
        default=BAUD_RATE_DEFAULT,
    )
    args = parser.parse_args()

    port = pick_port(args.port)
    listen(port, args.baud)