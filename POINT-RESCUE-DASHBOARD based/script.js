/* ============================================
   POINT RESCUE — Tracking Command Center
   script.js — Real-time GPS Dashboard Engine
   ============================================
   
   TRACKING LOGIC IS PRESERVED FROM ORIGINAL.
   Only UI rendering updated for new layout.
   ============================================ */

(function () {
    'use strict';

    /* === Configuration === */
    const CONFIG = {
        FETCH_INTERVAL: 1000,       // Poll GPS data every 1 second
        OFFLINE_TIMEOUT: 10000,     // Device offline after 10 seconds
        GPS_URL: 'gps.json',
        DEFAULT_CENTER: [-7.953850, 112.614955],
        DEFAULT_ZOOM: 16,
        MARKER_ZOOM: 18,
    };

    /* === Device Color Palette === */
    const DEVICE_COLORS = [
        '#3b82f6', // Blue
        '#10b981', // Emerald
        '#f59e0b', // Amber
        '#ef4444', // Red
        '#8b5cf6', // Violet
        '#ec4899', // Pink
        '#06b6d4', // Cyan
        '#f97316', // Orange
    ];

    /* === State === */
    let map = null;
    let mapInitialized = false;
    const devices = {};         // { device_id: { data, marker, lastUpdate, colorIndex } }
    let selectedDeviceId = null;
    let colorCounter = 0;
    let fetchErrorCount = 0;
    let lastGlobalUpdate = null;

    /* === DOM References === */
    const DOM = {
        // Map
        map: document.getElementById('map'),
        mapOverlayText: document.getElementById('mapOverlayText'),

        // Sidebar stats
        totalDevices: document.getElementById('totalDevices'),
        onlineDevices: document.getElementById('onlineDevices'),
        offlineDevices: document.getElementById('offlineDevices'),

        // Sidebar device list
        deviceList: document.getElementById('deviceList'),

        // Right panel - Status cards
        scardTotal: document.getElementById('scardTotal'),
        scardOnline: document.getElementById('scardOnline'),
        scardOffline: document.getElementById('scardOffline'),
        scardLastUpdate: document.getElementById('scardLastUpdate'),

        // Right panel - Detail
        detailPanel: document.getElementById('detailPanel'),
        detailPlaceholder: document.getElementById('detailPlaceholder'),
        detailContent: document.getElementById('detailContent'),
        detailDeviceId: document.getElementById('detailDeviceId'),
        detailDeviceIcon: document.getElementById('detailDeviceIcon'),
        detailStatus: document.getElementById('detailStatus'),
        detailLat: document.getElementById('detailLat'),
        detailLon: document.getElementById('detailLon'),
        detailAlt: document.getElementById('detailAlt'),
        detailSpeed: document.getElementById('detailSpeed'),
        detailSat: document.getElementById('detailSat'),
        detailLastUpdate: document.getElementById('detailLastUpdate'),

        // Header
        connectionStatus: document.getElementById('connectionStatus'),
        headerDeviceCount: document.getElementById('headerDeviceCount'),
        clock: document.getElementById('clock'),
        sidebarToggle: document.getElementById('sidebarToggle'),
        sidebar: document.getElementById('sidebar'),
    };


    /* ============================================
       MAP INITIALIZATION (called once, never again)
       ============================================ */
    function initMap() {
        if (mapInitialized) return;

        map = L.map('map', {
            center: CONFIG.DEFAULT_CENTER,
            zoom: CONFIG.DEFAULT_ZOOM,
            zoomControl: true,
            attributionControl: true,
        });

        L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
            attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
            maxZoom: 19,
        }).addTo(map);

        mapInitialized = true;
        DOM.mapOverlayText.textContent = 'Waiting for GPS data...';
    }


    /* ============================================
       CUSTOM MARKER ICON
       ============================================ */
    function createMarkerIcon(color) {
        const svg = `
            <svg xmlns="http://www.w3.org/2000/svg" width="32" height="42" viewBox="0 0 32 42">
                <defs>
                    <filter id="shadow" x="-20%" y="-10%" width="140%" height="130%">
                        <feDropShadow dx="0" dy="2" stdDeviation="2" flood-color="rgba(0,0,0,0.3)"/>
                    </filter>
                </defs>
                <path d="M16 0 C7.16 0 0 7.16 0 16 C0 28 16 42 16 42 S32 28 32 16 C32 7.16 24.84 0 16 0Z"
                      fill="${color}" filter="url(#shadow)"/>
                <circle cx="16" cy="16" r="7" fill="white" opacity="0.9"/>
                <circle cx="16" cy="16" r="4" fill="${color}"/>
            </svg>`;

        return L.divIcon({
            html: svg,
            className: 'custom-marker',
            iconSize: [32, 42],
            iconAnchor: [16, 42],
            popupAnchor: [0, -42],
        });
    }


    /* ============================================
       DEVICE MANAGEMENT
       ============================================ */

    function getDeviceColor(index) {
        return DEVICE_COLORS[index % DEVICE_COLORS.length];
    }

    function isDeviceOnline(device) {
        if (!device.lastUpdate) return false;
        return (Date.now() - device.lastUpdate) < CONFIG.OFFLINE_TIMEOUT;
    }

    function formatTime(timestamp) {
        if (!timestamp) return '—';
        const d = new Date(timestamp);
        return d.toLocaleTimeString('id-ID', {
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit',
        });
    }


    /* ============================================
       PROCESS GPS DATA
       ============================================ */
    function processGPSData(dataArray) {
        const now = Date.now();
        lastGlobalUpdate = now;

        dataArray.forEach(function (entry) {
            const id = entry.device_id;

            if (!devices[id]) {
                /* --- New Device --- */
                const ci = colorCounter++;
                const color = getDeviceColor(ci);
                const icon = createMarkerIcon(color);

                const marker = L.marker(
                    [entry.latitude, entry.longitude],
                    { icon: icon }
                ).addTo(map);

                devices[id] = {
                    data: entry,
                    marker: marker,
                    lastUpdate: now,
                    colorIndex: ci,
                    color: color,
                };

                // Bind popup
                marker.on('click', function () {
                    selectDevice(id);
                });

                // First device? Center map on it
                if (Object.keys(devices).length === 1) {
                    map.setView([entry.latitude, entry.longitude], CONFIG.DEFAULT_ZOOM);
                }

            } else {
                /* --- Existing Device: update position only --- */
                const device = devices[id];
                device.data = entry;
                device.lastUpdate = now;
                device.marker.setLatLng([entry.latitude, entry.longitude]);
            }
        });

        // Update marker popups
        Object.keys(devices).forEach(function (id) {
            const device = devices[id];
            const online = isDeviceOnline(device);
            const d = device.data;
            const popupHTML = buildPopupHTML(id, d, device.color, online);
            device.marker.bindPopup(popupHTML, { closeButton: true, maxWidth: 260 });
        });

        // Update UI
        updateSidebar();
        updateStats();
        updateDetailPanel();
        updateMapOverlay();
    }


    /* ============================================
       POPUP HTML
       ============================================ */
    function buildPopupHTML(id, data, color, online) {
        const statusClass = online ? 'online' : 'offline';
        const statusText = online ? 'Online' : 'Offline';

        return `
            <div class="popup-content">
                <div class="popup-header">
                    <span class="popup-color-dot" style="background:${color}; box-shadow: 0 0 6px ${color};"></span>
                    <span class="popup-device-name">Device ${id}</span>
                    <span class="popup-status ${statusClass}">${statusText}</span>
                </div>
                <div class="popup-row">
                    <span class="popup-row-label">Latitude</span>
                    <span class="popup-row-value">${Number(data.latitude).toFixed(6)}</span>
                </div>
                <div class="popup-row">
                    <span class="popup-row-label">Longitude</span>
                    <span class="popup-row-value">${Number(data.longitude).toFixed(6)}</span>
                </div>
                <div class="popup-row">
                    <span class="popup-row-label">Altitude</span>
                    <span class="popup-row-value">${data.altitude} m</span>
                </div>
                <div class="popup-row">
                    <span class="popup-row-label">Speed</span>
                    <span class="popup-row-value">${data.speed} km/h</span>
                </div>
                <div class="popup-row">
                    <span class="popup-row-label">Satellites</span>
                    <span class="popup-row-value">${data.satellites}</span>
                </div>
            </div>`;
    }


    /* ============================================
       SIDEBAR: DEVICE LIST
       ============================================ */
    function updateSidebar() {
        const ids = Object.keys(devices).sort();

        if (ids.length === 0) {
            DOM.deviceList.innerHTML = `
                <div class="device-list-empty">
                    <span class="material-icons-round">search_off</span>
                    <span>No devices detected</span>
                </div>`;
            return;
        }

        // Build device list items
        let html = '';
        ids.forEach(function (id) {
            const device = devices[id];
            const online = isDeviceOnline(device);
            const activeClass = (selectedDeviceId === id) ? ' active' : '';
            const statusClass = online ? 'online' : 'offline';
            const d = device.data;

            html += `
                <div class="device-item${activeClass}" data-device-id="${id}" onclick="window.__selectDevice('${id}')">
                    <span class="device-item-color" style="color:${device.color}; background:${device.color};"></span>
                    <div class="device-item-info">
                        <div class="device-item-name">Device ${id}</div>
                        <div class="device-item-coords">${Number(d.latitude).toFixed(4)}, ${Number(d.longitude).toFixed(4)}</div>
                    </div>
                    <span class="device-item-status ${statusClass}"></span>
                </div>`;
        });

        DOM.deviceList.innerHTML = html;
    }


    /* ============================================
       STATS (Sidebar + Right Panel Cards)
       ============================================ */
    function updateStats() {
        const ids = Object.keys(devices);
        const total = ids.length;
        let onlineCount = 0;

        ids.forEach(function (id) {
            if (isDeviceOnline(devices[id])) onlineCount++;
        });

        const offlineCount = total - onlineCount;

        // Sidebar stats
        DOM.totalDevices.textContent = total;
        DOM.onlineDevices.textContent = onlineCount;
        DOM.offlineDevices.textContent = offlineCount;

        // Right panel status cards
        DOM.scardTotal.textContent = total;
        DOM.scardOnline.textContent = onlineCount;
        DOM.scardOffline.textContent = offlineCount;
        DOM.scardLastUpdate.textContent = formatTime(lastGlobalUpdate);

        // Header device count
        DOM.headerDeviceCount.textContent = total + ' Device' + (total !== 1 ? 's' : '');
    }


    /* ============================================
       DETAIL PANEL
       ============================================ */
    function selectDevice(id) {
        if (!devices[id]) return;

        selectedDeviceId = id;
        const device = devices[id];

        // Focus map on device marker (no animation, instant)
        map.setView(
            [device.data.latitude, device.data.longitude],
            CONFIG.MARKER_ZOOM,
            { animate: false }
        );

        // Open popup
        device.marker.openPopup();

        // Update detail panel & sidebar
        updateDetailPanel();
        updateSidebar();

        // Close mobile sidebar
        closeMobileSidebar();
    }

    // Expose to global for onclick handler in sidebar list items
    window.__selectDevice = selectDevice;

    function updateDetailPanel() {
        if (!selectedDeviceId || !devices[selectedDeviceId]) {
            DOM.detailPlaceholder.style.display = 'flex';
            DOM.detailContent.style.display = 'none';
            return;
        }

        DOM.detailPlaceholder.style.display = 'none';
        DOM.detailContent.style.display = 'flex';

        const device = devices[selectedDeviceId];
        const d = device.data;
        const online = isDeviceOnline(device);

        // Device header
        DOM.detailDeviceId.textContent = 'Device ' + selectedDeviceId;
        DOM.detailDeviceIcon.style.background = device.color + '18';
        DOM.detailDeviceIcon.style.color = device.color;

        // Status
        const statusClass = online ? 'online' : 'offline';
        const statusText = online ? 'Online' : 'Offline';
        DOM.detailStatus.className = 'detail-status ' + statusClass;
        DOM.detailStatus.querySelector('.status-label').textContent = statusText;

        // Values
        DOM.detailLat.textContent = Number(d.latitude).toFixed(6) + '°';
        DOM.detailLon.textContent = Number(d.longitude).toFixed(6) + '°';
        DOM.detailAlt.textContent = d.altitude + ' m';
        DOM.detailSpeed.textContent = d.speed + ' km/h';
        DOM.detailSat.textContent = d.satellites;
        DOM.detailLastUpdate.textContent = formatTime(device.lastUpdate);
    }


    /* ============================================
       MAP OVERLAY INFO
       ============================================ */
    function updateMapOverlay() {
        const total = Object.keys(devices).length;
        if (total === 0) {
            DOM.mapOverlayText.textContent = 'Waiting for GPS data...';
        } else {
            let onlineCount = 0;
            Object.keys(devices).forEach(function (id) {
                if (isDeviceOnline(devices[id])) onlineCount++;
            });
            DOM.mapOverlayText.textContent = `Tracking ${total} device${total > 1 ? 's' : ''} · ${onlineCount} online`;
        }
    }


    /* ============================================
       GPS DATA FETCHER
       ============================================ */
    async function fetchGPSData() {
        try {
            const response = await fetch(CONFIG.GPS_URL + '?t=' + Date.now());

            if (!response.ok) {
                throw new Error('HTTP ' + response.status);
            }

            let data = await response.json();

            // Support both single object and array format
            if (!Array.isArray(data)) {
                data = [data];
            }

            processGPSData(data);

            // Reset error state
            fetchErrorCount = 0;
            setConnectionStatus(true);

        } catch (err) {
            fetchErrorCount++;
            console.warn('[PointRescue] Fetch error:', err.message);

            if (fetchErrorCount >= 3) {
                setConnectionStatus(false);
            }

            // Still update UI for offline detection
            updateStats();
            updateSidebar();
            updateDetailPanel();
        }
    }


    /* ============================================
       CONNECTION STATUS
       ============================================ */
    function setConnectionStatus(connected) {
        const el = DOM.connectionStatus;
        const textEl = el.querySelector('.status-text');

        if (connected) {
            el.classList.remove('error');
            textEl.textContent = 'SYSTEM ONLINE';
        } else {
            el.classList.add('error');
            textEl.textContent = 'DISCONNECTED';
        }
    }


    /* ============================================
       CLOCK
       ============================================ */
    function updateClock() {
        const now = new Date();
        DOM.clock.textContent = now.toLocaleTimeString('id-ID', {
            hour: '2-digit',
            minute: '2-digit',
            second: '2-digit',
        });
    }


    /* ============================================
       MOBILE SIDEBAR
       ============================================ */
    let overlay = null;

    function setupMobileSidebar() {
        // Create overlay
        overlay = document.createElement('div');
        overlay.className = 'sidebar-overlay';
        document.body.appendChild(overlay);

        DOM.sidebarToggle.addEventListener('click', function () {
            const isOpen = DOM.sidebar.classList.contains('open');
            if (isOpen) {
                closeMobileSidebar();
            } else {
                openMobileSidebar();
            }
        });

        overlay.addEventListener('click', closeMobileSidebar);
    }

    function openMobileSidebar() {
        DOM.sidebar.classList.add('open');
        if (overlay) overlay.classList.add('active');
    }

    function closeMobileSidebar() {
        DOM.sidebar.classList.remove('open');
        if (overlay) overlay.classList.remove('active');
    }


    /* ============================================
       INITIALIZATION
       ============================================ */
    function init() {
        initMap();
        setupMobileSidebar();
        updateClock();

        // Start clock update
        setInterval(updateClock, 1000);

        // Start GPS data polling
        fetchGPSData();
        setInterval(fetchGPSData, CONFIG.FETCH_INTERVAL);
    }

    // Boot up when DOM is ready
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

})();