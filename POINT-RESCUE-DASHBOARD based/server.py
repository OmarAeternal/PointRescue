"""
server.py — Point Rescue Local HTTP Server
==========================================
Serve dashboard HTML + gps.json di localhost
agar browser bisa membacanya (file:// tidak bisa
fetch JSON karena CORS).

Cara pakai:
    python server.py            # default: port 8000
    python server.py --port 8080
    
Lalu buka browser: http://localhost:8000
"""

import http.server
import socketserver
import argparse
import webbrowser
import os
from pathlib import Path

PORT_DEFAULT = 8000
SERVE_DIR = Path(__file__).parent   # Folder yang sama dengan script ini


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    """Handler dengan header no-cache agar gps.json selalu fresh di browser."""

    def end_headers(self):
        # Nonaktifkan cache untuk semua response
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        # CORS header agar fetch() dari JS tidak diblokir
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()

    def log_message(self, format, *args):
        # Sembunyikan log request gps.json agar tidak spam terminal
        if "gps.json" not in args[0]:
            super().log_message(format, *args)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Point Rescue — Local HTTP server untuk dashboard GPS"
    )
    parser.add_argument(
        "--port", "-p",
        type=int,
        default=PORT_DEFAULT,
        help=f"Port HTTP (default: {PORT_DEFAULT})",
    )
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="Jangan buka browser otomatis",
    )
    args = parser.parse_args()

    # Pindah ke direktori dashboard
    os.chdir(SERVE_DIR)

    url = f"http://localhost:{args.port}"
    print(f"\n[SERVER] Point Rescue Dashboard Server")
    print(f"[SERVER] Serving: {SERVE_DIR.resolve()}")
    print(f"[SERVER] URL    : {url}")
    print(f"[SERVER] Tekan Ctrl+C untuk berhenti.\n")

    if not args.no_browser:
        webbrowser.open(url)

    with socketserver.TCPServer(("", args.port), NoCacheHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[STOP]  Server dihentikan.")