#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// P1 Machbarkeits-Spike (docs/implementierungsplan.html, Stufe 2 - Kritisches
// Gate). Kapselt trombik/esp_wireguard mit der Kconfig-Platzhalterkonfiguration
// aus components/wireguard_manager/Kconfig. Erfordert ein bereits verbundenes
// WLAN (siehe network_manager.h) - "a working network interface is required"
// (esp_wireguard-README).

// Baut den internen wireguard_ctx_t aus der Kconfig-Konfiguration auf.
// Muss vor wireguard_manager_connect() aufgerufen werden.
esp_err_t wireguard_manager_init(void);

// Stoesst den Tunnelaufbau an (asynchron - esp_wireguard baut die Verbindung
// im Hintergrund auf, siehe wireguard_manager_is_up()).
esp_err_t wireguard_manager_connect(void);

// true, sobald der Peer als erreichbar gilt (esp_wireguardif_peer_is_up()).
bool wireguard_manager_is_up(void);

esp_err_t wireguard_manager_disconnect(void);

// --- Laufzeit-Konfiguration ueber Upload (webconfig.txt "Seite
// Einstellungen") - ersetzt/ergaenzt die Kconfig-Platzhalterkonfiguration
// des P1-Spikes zur Laufzeit. ---

// Parst eine hochgeladene wireguard.conf ([Interface]/[Peer]-INI-Format),
// entfernt eine etwaige Default-Route (AllowedIPs "0.0.0.0/0" bzw. "::/0")
// VOR dem Speichern, persistiert das Ergebnis auf der storage-Partition
// und baut den Tunnel mit der neuen Konfiguration neu auf (trennt zuerst
// eine ggf. aktive Verbindung). Erfordert ein bereits verbundenes WLAN.
esp_err_t wireguard_manager_apply_uploaded_config(const char* conf_text);

// Loescht die gespeicherte Konfiguration und trennt den Tunnel.
esp_err_t wireguard_manager_delete_config(void);

// true, wenn eine hochgeladene (nicht nur Kconfig-Platzhalter-)Konfiguration
// vorliegt.
bool wireguard_manager_has_uploaded_config(void);

// Prueft NUR, ob eine hochgeladene Konfigurationsdatei auf der
// storage-Partition existiert - im Unterschied zu
// wireguard_manager_has_uploaded_config() OHNE wireguard_manager_init()
// vorher aufzurufen (das legt s_has_uploaded_config erst fest). Fuer
// main.c's Gate um wireguard_manager_init() herum gedacht - siehe dortiger
// Kommentar zum reproduzierbaren Boot-Absturz (docs/entscheidungen.md,
// 2026-07-23).
bool wireguard_manager_config_available(void);

// Aktuelle Werte fuer Anzeige (Uebersichtsseite) - gueltig nach
// wireguard_manager_init(), unabhaengig davon ob Kconfig-Platzhalter oder
// hochgeladene Konfiguration.
void wireguard_manager_get_local_address(char* out, size_t out_len);
void wireguard_manager_get_endpoint(char* out, size_t out_len);
// Oeffentlicher Schluessel des Peers (kein Geheimnis). Der PrivateKey wird
// bewusst NICHT nach aussen gegeben.
void wireguard_manager_get_public_key(char* out, size_t out_len);
// Kommagetrennte Liste der AllowedIPs (ip/prefix), ohne die per Upload
// entfernte Default-Route. Leerer String, wenn keine gesetzt sind.
void wireguard_manager_get_allowed_ips(char* out, size_t out_len);

// Startet die periodische VPN-Zustandsueberwachung (loggt Auf-/Abbau des
// Tunnels ins Audit-Log). Idempotent - mehrfacher Aufruf startet nur einmal.
void wireguard_manager_start_monitor(void);
