#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NotificationManager (Pflichtenheft Abschnitt 3.8) - Schwellwert-
// Ueberschreitung -> Benachrichtigung. Versandweg entschieden
// (docs/entscheidungen.md "Benachrichtigungswege: Syslog + SMTP ohne
// TLS"): zwei parallele, unabhaengig konfigurierbare Wege -
//  - Syslog (UDP): an einen zentralen Log-/SIEM-Server, unabhaengig von
//    Benutzerkonten.
//  - SMTP OHNE TLS: an jeden Benutzer mit aktivierter Benachrichtigung
//    (siehe user_manager.h) - bewusst ohne Verschluesselung, siehe
//    Entscheidungsprotokoll fuer die Kosten/Nutzen-Abwaegung
//    (TLS haette einen zweiten, vollstaendigen Krypto-Stack neben dem
//    schon vorhandenen wolfSSL fuer SSH gebraucht).
// Beide Wege sind optional (leerer Server = deaktiviert) und koennen
// unabhaengig voneinander konfiguriert sein.

void notification_manager_init(void);

// Wird bei jeder Schwellwert-Ueberschreitung aufgerufen (flankengetriggert
// vom Aufrufer - siehe sensor_manager.c). "quelle" bezeichnet die
// Messgroesse (z.B. "NTC-Temperatur"), value/threshold die konkreten
// Werte fuer die Meldung.
void notification_manager_trigger(const char* quelle, float value, float threshold);

// --- Syslog-Zielserver (UDP, Port meist 514) ---
bool notification_manager_set_syslog(const char* server, uint16_t port);
void notification_manager_get_syslog(char* out_server, size_t out_len, uint16_t* out_port);

// --- SMTP-Zielserver (Klartext, kein TLS) ---
//
// "password" wird nur uebernommen, wenn nicht leer - ein leeres Feld im
// Web-Formular bedeutet "unveraendert lassen" (das gespeicherte Passwort
// wird nie an die Einstellungen-Seite zurueckgegeben, siehe
// notification_manager_get_smtp()).
bool notification_manager_set_smtp(const char* server, uint16_t port, const char* sender, const char* username,
                                    const char* password);
void notification_manager_get_smtp(char* out_server, size_t out_server_len, uint16_t* out_port, char* out_sender,
                                    size_t out_sender_len, char* out_username, size_t out_username_len);

void notification_manager_reset_to_defaults(void);
