#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// OTA-Update per lokalem .bin-Upload ueber die Einstellungen-Seite -
// Identitaets-/Downgrade-Pruefung nach demselben Muster wie die
// Sensormeter-Familie: waehrend des Uploads wird im Byte-Stream nach
// einem in diese Firmware einkompilierten Marker
// "ESPBMC-FW-ID:<FIRMWARE_PROJECT_ID>:<DEVICE_FIRMWARE_VERSION>:ESPBMC-FW-END"
// (siehe firmware_version.h) gesucht - verhindert versehentliches Flashen
// einer .bin eines anderen Projekts oder einer aelteren eigenen Version.
// Kein kryptografischer Schutz, nur eine Plausibilitaetspruefung gegen
// Verwechslungen (identisches Konzept wie bei Sensormeter, siehe deren
// docs/entscheidungen.md).
//
// Marker-Prefix/-Suffix bewusst NICHT deckungsgleich mit Sensormeters
// "SM-FW-ID:"/"​:SM-FW-END" gewaehlt (bzw. keine Teilstring-Beziehung in
// beide Richtungen) - sonst wuerde eine Sensormeter-.bin faelschlich auch
// hier einen (Teil-)Treffer liefern und umgekehrt.
//
// Von Anfang an byte-sicher (memcmp-basiert) implementiert statt mit
// C-String-Funktionen (strstr() o.ae.) - genau der Fehler, den die
// Sensormeter-Familie erst nachtraeglich gefunden und beheben musste
// (String::indexOf() ist strstr()-basiert und bricht am ersten
// eingebetteten Null-Byte ab, das in einer echten .bin schon ab Byte 9 im
// ESP32-Image-Header vorkommt - der Marker wurde dadurch nie gefunden,
// jeder echte Upload faelschlich abgelehnt). Siehe docs/entscheidungen.md
// "OTA-Update ...".

void ota_manager_init(void);

// allow_downgrade entspricht der "Downgrade erzwingen"-Option im
// Web-Formular - muss VOR dem Upload bekannt sein (wird beim Erkennen des
// Markers sofort ausgewertet, nicht erst in ota_manager_end()).
bool ota_manager_begin(bool allow_downgrade);
bool ota_manager_write_chunk(const uint8_t* data, size_t len);

// Committet nur, wenn Marker gefunden UND Projekt-Identitaet passt UND
// (Version nicht aelter ODER Downgrade erzwungen) - sonst wird die
// OTA-Partition verworfen (esp_ota_abort), kein Neustart in eine
// unvollstaendige/fremde Firmware moeglich.
bool ota_manager_end(void);

bool ota_manager_marker_found(void);
bool ota_manager_identity_matches(void);
bool ota_manager_version_allowed(void);

const char* ota_manager_get_version(void);
