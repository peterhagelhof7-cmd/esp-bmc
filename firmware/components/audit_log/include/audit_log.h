#pragma once

#include <stddef.h>

// AuditLog (webconfig.txt "Seite Logs") - protokolliert sicherheitsrelevante
// Ereignisse (Verbindungsaufbau ueber Web/SSH, Taster-Steuerung ueber die
// Weboberflaeche, Erkennung physischer Tastendruecke) persistent auf der
// storage-Partition (/storage/audit.log), mit Groessenrotation analog dem
// etablierten Log-Muster der Sensormeter-Familie. Zeitstempel: echte
// Wanduhrzeit (ueber time_manager/NTP), sobald mindestens einmal
// synchronisiert wurde - davor Uptime-Sekunden als Platzhalter (z.B. ganz
// kurz nach dem Boot, bevor der erste NTP-Sync durch ist).

void audit_log_init(void);

void audit_log_add(const char* event);

// Liest den aktuellen Log-Inhalt (nicht die rotierte .old-Datei) nach out.
// Liefert die Anzahl gelesener Bytes.
size_t audit_log_read(char* out, size_t out_len);
