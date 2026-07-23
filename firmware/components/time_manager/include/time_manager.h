#pragma once

#include <stdbool.h>
#include <time.h>

// TimeManager - NTP-Zeitsynchronisation, analog dem bewaehrten Muster der
// Sensormeter-Familie: sofortiger Resync bei jedem Netzwerk-Link-Up
// (auch nach einem Reconnect), sonst alle 5 Stunden. Zeitzone Europe/Berlin
// (CET/CEST, automatische Sommerzeitumschaltung).

void time_manager_init(void);

// Von network_manager bei jedem WLAN-Verbindungsaufbau (IP erhalten)
// aufzurufen - stoesst einen sofortigen Resync an, unabhaengig vom
// 5h-Rhythmus.
void time_manager_notify_link_up(void);

// true, sobald die Zeit mindestens einmal erfolgreich synchronisiert wurde.
bool time_manager_is_synced(void);

// Unix-Zeitstempel des letzten ERFOLGREICHEN NTP-Syncs (on_time_sync()) - 0,
// wenn seit dem Boot noch nie erfolgreich synchronisiert wurde. Fuer die
// Anzeige "letzter NTP-Sync" auf der Uebersichtsseite gedacht.
time_t time_manager_get_last_sync(void);

// Wird nach jedem einzelnen Sync-Versuch (Link-Up-Resync oder 5h-Rhythmus)
// mit dem Erfolg dieses Versuchs aufgerufen - fuer das Audit-Log
// ("ntp sync OK/nicht OK", siehe was-loggen.txt). Bewusst als
// Callback statt direktem audit_log-Aufruf aus time_manager.c, um keine
// Kreisabhaengigkeit zu audit_log (das seinerseits time_manager fuer
// Zeitstempel braucht) einzugehen - main.c verdrahtet beide.
typedef void (*time_manager_sync_result_cb_t)(bool success);
void time_manager_set_sync_result_cb(time_manager_sync_result_cb_t cb);
