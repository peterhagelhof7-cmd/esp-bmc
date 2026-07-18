#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// UserManager (webconfig.txt "Geschuetzte Seite" / Benutzerverwaltung) -
// rollenbasierte Benutzerkonten + Web-Session-Verwaltung. Persistiert auf
// der "storage"-Partition (siehe storage_manager) als JSON
// ("/storage/users.json"). Wird spaeter auch von P7 (SSH-Zugang) fuer die
// Public-Key-Zuordnung dieselbe Benutzerbasis nutzen.
//
// Rollen (webconfig.txt):
//   Leser       - nur Log-/Sensorwerte-Download
//   SSH User    - Webconsole + SSH-Key-Hinterlegung
//   Verwalter   - alle Einstellungen, Taster-Steuerung nur mit eigenem
//                 Passwort erneut bestaetigt (Bestaetigung liegt beim
//                 Aufrufer/WebServerManager, nicht hier)
//   Admin       - wie Verwalter, zusaetzlich Taster-Steuerung ohne erneute
//                 Bestaetigung + Userverwaltung
typedef enum {
  USER_ROLE_LESER = 0,
  USER_ROLE_SSH_USER,
  USER_ROLE_VERWALTER,
  USER_ROLE_ADMIN,
} user_role_t;

// Laedt die Benutzerliste von der storage-Partition. Existiert noch keine
// (erster Start), wird ein Default-Konto "admin"/"admin" (Rolle Admin)
// angelegt und eine Warnung geloggt, das Passwort zu aendern - analog dem
// "installer"/"installer"-Fallback-Muster aus Sensormeter-WLAN.
void user_manager_init(void);

// Prueft Zugangsdaten. Bei Erfolg wird *out_role gesetzt und true
// zurueckgegeben.
bool user_manager_authenticate(const char* username, const char* password, user_role_t* out_role);

// Benutzername: 1-31 Zeichen, nur [a-zA-Z0-9_-] (keine Sonderzeichen).
bool user_manager_validate_username(const char* username);

// Passwort: mindestens 8 Zeichen UND mindestens 2 der 3 Klassen
// (Grossbuchstaben/Kleinbuchstaben/Ziffern) erfuellt.
bool user_manager_validate_password(const char* password);

// Legt ein neues Konto an (schlaegt fehl, wenn Benutzername ungueltig,
// Passwort die Policy nicht erfuellt, oder der Name schon existiert).
bool user_manager_create(const char* username, const char* password, user_role_t role);
bool user_manager_delete(const char* username);
bool user_manager_exists(const char* username);
size_t user_manager_count(void);

// Iteration ueber alle Konten (fuer Anzeige/Config-Export) - index laeuft
// von 0 bis user_manager_count()-1. Liefert false, wenn der Index
// ausserhalb des gueltigen Bereichs liegt.
bool user_manager_get_at(size_t index, char out_username[32], user_role_t* out_role);

// --- Web-Session-Verwaltung (Cookie-Token) ---

// Erzeugt ein neues Session-Token (32 Hex-Zeichen + Nullterminierung) fuer
// einen bereits erfolgreich authentifizierten Benutzer.
void user_manager_session_create(const char* username, user_role_t role, char out_token[33]);

// Prueft ein Session-Token. Bei Erfolg werden Benutzername/Rolle gesetzt.
bool user_manager_session_validate(const char* token, char out_username[32], user_role_t* out_role);

void user_manager_session_invalidate(const char* token);

// Loescht alle Benutzerkonten und Sessions, legt danach wieder das
// Default-Konto "admin"/"admin" an (webconfig.txt "Seite Einstellungen":
// "reset (nur einstellungen)") - identisch zum Erststart-Verhalten in
// user_manager_init().
void user_manager_reset_to_default(void);

// --- SSH-Public-Key (P7, webconfig.txt: "Admin"/"SSH User" = "... +
// hinterlegen eines SSH key") ---
//
// Ein Schluessel pro Konto, im ueblichen OpenSSH-"authorized_keys"-
// Zeilenformat gespeichert ("ecdsa-sha2-nistp256 AAAA...[Kommentar]") -
// nur der Base64-Block wird fuer den eigentlichen Vergleich gebraucht
// (der Typ-String steckt ohnehin redundant im Wire-Format-Blob selbst
// drin). Nur ECDSA/Ed25519-Client-Schluessel funktionieren - RSA ist
// projektweit deaktiviert (firmware/CMakeLists.txt NO_RSA, siehe
// docs/entscheidungen.md "SSH-Server (P7)"), ein RSA-Schluessel wuerde
// nie passen.
bool user_manager_set_ssh_public_key(const char* username, const char* openssh_line);
bool user_manager_get_ssh_public_key(const char* username, char* out, size_t out_len);

// Prueft einen SSH-Wire-Format-Public-Key-Blob (roh, wie ihn der
// SSH-Client waehrend der Anmeldung praesentiert - NICHT Base64) gegen
// den fuer diesen Benutzer hinterlegten Schluessel. Bei Erfolg wird
// *out_role gesetzt.
bool user_manager_verify_ssh_public_key(const char* username, const uint8_t* key_blob, size_t key_blob_len,
                                         user_role_t* out_role);

// --- E-Mail-Benachrichtigung (Pflichtenheft Abschnitt 3.8, Versandweg) ---
//
// Jeder Nutzer verwaltet seine eigene Adresse + Aktiv-Schalter selbst
// (Selbstbedienung wie beim SSH-Key oben, aber ohne Rollenbeschraenkung -
// Benachrichtigungen sind fuer jede Rolle sinnvoll). Ist "enabled" true,
// muss "email" eine minimal plausible Adresse sein (enthaelt "@" und
// danach noch mind. ein Zeichen), sonst schlaegt der Aufruf fehl - eine
// aktivierte Benachrichtigung ohne Adresse ergibt keinen Sinn. Ist
// "enabled" false, wird die Adresse trotzdem gespeichert (Nutzer kann sie
// vorbereiten, ohne gleich Mails zu bekommen).
bool user_manager_set_notification_email(const char* username, const char* email, bool enabled);
bool user_manager_get_notification_email(const char* username, char* out_email, size_t out_len, bool* out_enabled);

// Iteration NUR ueber Konten mit aktivierter Benachrichtigung UND
// hinterlegter Adresse - fuer den Versand-Fan-Out in notification_manager.
// Anders als user_manager_get_at() (das ueber ALLE Konten iteriert) laeuft
// der Index hier nur ueber die passenden Konten.
size_t user_manager_count_notify_recipients(void);
bool user_manager_get_notify_recipient_at(size_t index, char* out_email, size_t out_len);
