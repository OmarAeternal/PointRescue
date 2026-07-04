let map;
let marker;

async function loadGPS() {

    const response = await fetch(
        "gps.json?t=" + new Date().getTime()
    );

    const data = await response.json();

    document.getElementById("lat").innerText =
        Number(data.latitude).toFixed(6);

    document.getElementById("lon").innerText =
        Number(data.longitude).toFixed(6);

    document.getElementById("alt").innerText =
        data.altitude + " m";

    document.getElementById("speed").innerText =
        data.speed + " km/h";

    document.getElementById("sat").innerText =
        data.satellites;

    if (!map) {

        map = L.map('map').setView(
            [data.latitude, data.longitude],
            18
        );

        L.tileLayer(
            'https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',
            {
                attribution:
                    '&copy; OpenStreetMap contributors'
            }
        ).addTo(map);

        marker = L.marker(
            [data.latitude, data.longitude]
        ).addTo(map);

        marker.bindPopup(
            "POINTRESCUE GPS"
        );

    } else {

        marker.setLatLng([
            data.latitude,
            data.longitude
        ]);

        map.panTo([
            data.latitude,
            data.longitude
        ]);

    }
}

loadGPS();

setInterval(loadGPS, 1000);