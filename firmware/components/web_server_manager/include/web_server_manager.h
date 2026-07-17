#pragma once

// WebServerManager (docs/pflichtenheft.txt Abschnitt 3.6, webconfig.txt) -
// HTTP-Server mit Login/Session-Verwaltung (siehe user_manager) und
// WebSocket-Konsole (gebrueckt auf usb_manager's CDC-Queue).
//
// Diese erste Ausbaustufe deckt bewusst nur einen Teil von webconfig.txt ab:
// Login, Uebersichtsseite mit aktuellen Werten (ohne 24h-Chart/HDD-Blink-
// Anzeige - dafuer fehlt noch ein Sensor-Historie-Ringpuffer, siehe
// docs/entscheidungen.md) und die Webconsole. Einstellungen-/Logs-Seiten,
// Benutzerverwaltungs-UI, WLAN-Scan-Seite, WireGuard-Config-Upload,
// Audit-Log sind als naechste Schritte offen (siehe Projekt-Memory).

void web_server_manager_init(void);
