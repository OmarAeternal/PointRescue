import json
import time
import random
import math

# === Multi-Device GPS Dummy Generator ===
# Simulates multiple GPS devices moving around a center point.
# Each device moves in a different pattern for realistic demo.

devices = [
    {
        "device_id": "XX1",
        "latitude": -7.953850,
        "longitude": 112.614955,
        "altitude": 535.5,
        "speed": 0.52,
        "satellites": 8,
        "angle": 0.0,
    },
    {
        "device_id": "XX2",
        "latitude": -7.954500,
        "longitude": 112.616000,
        "altitude": 540.0,
        "speed": 0.80,
        "satellites": 10,
        "angle": 2.0,
    },
    {
        "device_id": "XX3",
        "latitude": -7.952800,
        "longitude": 112.613500,
        "altitude": 530.0,
        "speed": 1.20,
        "satellites": 12,
        "angle": 4.0,
    },
]

step = 0

while True:
    output = []

    for dev in devices:
        # Simulate circular/wandering movement
        dev["angle"] += random.uniform(0.05, 0.15)
        radius = 0.0003 + random.uniform(-0.00005, 0.00005)

        dev["latitude"] += math.sin(dev["angle"]) * radius * 0.1
        dev["longitude"] += math.cos(dev["angle"]) * radius * 0.1

        # Randomize speed and satellites slightly
        dev["speed"] = round(random.uniform(0.3, 2.5), 2)
        dev["satellites"] = random.randint(6, 14)
        dev["altitude"] = round(dev["altitude"] + random.uniform(-0.5, 0.5), 1)

        output.append({
            "device_id": dev["device_id"],
            "latitude": round(dev["latitude"], 6),
            "longitude": round(dev["longitude"], 6),
            "altitude": dev["altitude"],
            "speed": dev["speed"],
            "satellites": dev["satellites"],
        })

    with open("gps.json", "w") as f:
        json.dump(output, f, indent=4)

    step += 1
    print(f"[Step {step}] Updated {len(devices)} devices")

    time.sleep(1)