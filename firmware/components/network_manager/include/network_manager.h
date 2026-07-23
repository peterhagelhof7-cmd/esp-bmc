#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Minimaler WLAN-STA-Aufbau (Pflichtenheft 3.4). Persistenz/Reconnect-Feintuning
// noch offen (siehe Pflichtenheft 8.1) - dies ist der P0-Mindestumfang, der
// WireGuardManager (P1) ein Netzwerkinterface bereitstellt.

// Initialisiert NVS/Netif/Event-Loop/WiFi-Treiber und stoesst den
// Verbindungsaufbau an. Blockiert nicht.
void network_manager_init(void);

// true, sobald eine IP-Adresse zugewiesen wurde (IP_EVENT_STA_GOT_IP).
bool network_manager_is_connected(void);

// true, waehrend der Fallback-Access-Point "installer" aktiv ist (kein
// konfiguriertes WLAN 5 Minuten lang erreichbar, siehe network_manager.c
// start_fallback_ap()). Erreichbar unter 192.168.4.1, DHCP-Server aktiv -
// neue Zugangsdaten ueber network_manager_join() eintragen, um zurueck auf
// STA zu wechseln.
bool network_manager_is_fallback_ap_active(void);

// Schreibt die aktuelle WLAN-IP als String (z.B. "192.168.1.42") nach
// out_buf (mind. 16 Byte). Liefert false, falls (noch) keine IP vorliegt -
// out_buf enthaelt dann einen leeren String.
bool network_manager_get_ip_string(char* out_buf, size_t buf_len);

// --- IP-Konfiguration (webconfig.txt "Seite Einstellungen") ---

// true, wenn aktuell eine statische IP aktiv ist (statt DHCP).
bool network_manager_is_static_ip(void);

// Versucht eine statische IP-Konfiguration zu uebernehmen: stoppt DHCP,
// setzt die neue Konfiguration, prueft per Ping gegen das Gateway ("vor
// Uebernehmen Ping-Check wie bei SM"). Bei Fehlschlag wird die vorherige
// Konfiguration wiederhergestellt (DHCP bzw. die vorherige statische
// Konfiguration) und false zurueckgegeben - die neue Konfiguration bleibt
// dann NICHT aktiv.
bool network_manager_apply_static_ip(const char* ip, const char* netmask, const char* gateway);

// Schaltet zurueck auf DHCP.
void network_manager_use_dhcp(void);

// Schreibt die aktuell konfigurierten statischen Werte (nur sinnvoll,
// wenn network_manager_is_static_ip() true liefert). Jeder Puffer
// mindestens 16 Byte.
void network_manager_get_static_config(char* out_ip, char* out_netmask, char* out_gateway);

// --- WLAN-Zugangsdaten setzen (inital-setup.txt: Erstinbetriebnahme ueber
// USB, ohne dass vorher schon ein Netzwerk erreichbar sein muss) ---

// Setzt neue STA-Zugangsdaten, persistiert sie auf der storage-Partition
// (ueberlebt Reboots, ueberschreibt danach dauerhaft die
// Kconfig-Platzhalterwerte aus dem P0-Mindestumfang) und stoesst einen
// Reconnect an. password="" fuer offene Netze. Nicht blockierend -
// Ergebnis ueber network_manager_is_connected() abfragbar.
void network_manager_join(const char* ssid, const char* password);

// Aktuell konfigurierte SSID (Kconfig-Platzhalter oder zuletzt per
// network_manager_join() gesetzter Wert).
void network_manager_get_ssid(char* out, size_t out_len);

// Loescht gespeicherte WLAN-Zugangsdaten von der storage-Partition
// (webconfig.txt "Seite Einstellungen": "reset (nur einstellungen)") -
// wirkt erst nach einem Neustart vollstaendig (dann greifen wieder die
// Kconfig-Platzhalter aus network_manager_init()).
void network_manager_reset(void);

// --- WLAN-Scan (webconfig.txt "Wlan scan (wie bei SM-WLAN)") ---

typedef struct {
  char ssid[33];
  bool open;    // true = kein Passwort noetig (kein Schloss-Symbol im UI)
  int8_t rssi;
} network_wifi_scan_result_t;

// Blockierender Scan (dauert typischerweise 1-3s). Schreibt bis zu
// max_results Eintraege nach out, liefert die tatsaechliche Anzahl.
int network_manager_scan_wifi(network_wifi_scan_result_t* out, int max_results);
