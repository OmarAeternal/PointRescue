import json
import time

lat = -7.953850
lon = 112.614955

while True:

    lat += 0.00001
    lon += 0.00001

    data = {
        "device_id": "001",
        "latitude": round(lat, 6),
        "longitude": round(lon, 6),
        "altitude": 535.5,
        "speed": 0.52,
        "satellites": 8
    }

    with open("gps.json", "w") as f:
        json.dump(data, f, indent=4)

    print(f"Updated: {lat}, {lon}")

    time.sleep(1)