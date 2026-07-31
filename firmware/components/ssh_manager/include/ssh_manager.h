#pragma once

#include <stdbool.h>
#include <stddef.h>

// SshManager (P7, Lastenheft Abschnitt 6 / webconfig.txt) - eigener
// SSH-Server auf dem ESP (nicht Pass-Through zu einer sshd auf dem
// gesteuerten PC). Nutzer meldet sich direkt am ESP an (dieselbe
// user_manager-Kontodatenbank wie Web/USB), die Session steuert danach
// dieselbe CDC/HID-Bruecke wie die Web-Konsole.

void ssh_manager_init(void);

// Oeffentlicher Host-Key (nicht vertraulich - wird bei jeder Verbindung
// ohnehin an den Client uebertragen, siehe docs/entscheidungen.md
// "SSH-Server (P7)"). Fuer die Uebersichtsseite, damit ein Nutzer die
// Authentizitaet VOR dem ersten Connect out-of-band pruefen kann
// (Trust-on-First-Use-Problem). Leerer String, falls noch kein Host-Key
// geladen/erzeugt wurde.
const char* ssh_manager_get_host_key_fingerprint(void);
const char* ssh_manager_get_host_public_key_line(void);

// Aktuell aktive SSH-Konsolen-Sitzung fuer die Uebersichtsseite: true, wenn
// gerade eine SSH-Sitzung die Konsole nutzt; fuellt dann user/ip mit
// Benutzername und Quell-IP. false, wenn keine SSH-Sitzung aktiv ist.
bool ssh_manager_get_active_console(char* user, size_t user_cap, char* ip, size_t ip_cap);
