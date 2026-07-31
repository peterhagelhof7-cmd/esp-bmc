#pragma once

#include <stdbool.h>
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
// Liefert die Anzahl gelesener Bytes. ACHTUNG: liest nur die ersten out_len-1
// Byte ab Dateianfang - fuer den vollstaendigen Download stattdessen
// audit_log_stream() nutzen (das gesamte Log kann groesser als jeder
// vertretbare Stack-Puffer sein).
size_t audit_log_read(char* out, size_t out_len);

// Gibt das VOLLSTAENDIGE Audit-Log chunk-weise an sink() aus: zuerst die
// rotierte .old-Datei (falls vorhanden), dann die aktuelle - so bleibt der
// Verlauf ueber eine Rotation hinweg chronologisch und vollstaendig, ohne dass
// der Aufrufer die ganze Datei in einen Puffer laden muss. sink() bekommt den
// uebergebenen ctx und einen Datenausschnitt; liefert sink() false, wird der
// Vorgang abgebrochen. Rueckgabe: false bei Abbruch durch sink(), sonst true.
typedef bool (*audit_log_sink_t)(void* ctx, const char* data, size_t len);
bool audit_log_stream(audit_log_sink_t sink, void* ctx);
