# ESP-BMC — Projektstand

Stand: 2026-07-17. Abgeleitet aus `docs/lastenheft.txt` und
`docs/pflichtenheft.txt`, abgeglichen mit dem tatsächlichen
Firmware-Code (nicht nur mit dem, was in `docs/entscheidungen.md`
irgendwann mal geplant war). Lastenheft/Pflichtenheft selbst wurden
**nicht** nachgezogen — mehrere dort als "offen" gelistete Punkte sind
inzwischen entschieden oder gebaut, das steht hier, nicht dort.

Wichtiger Vorbehalt: **nichts von alldem ist auf echter Hardware
verifiziert.** Es existiert noch kein Board. Alles unten ist
"kompiliert fehlerfrei", nicht "funktioniert nachweislich".

---

## 1. Umgesetzte Features

### Überwachung (Lastenheft 4)

| Anforderung | Status | Anmerkung |
|---|---|---|
| Power-LED des Mainboards erfassen | ✅ | `gpio_manager_read_power_led()`, im Web/USB/SNMP sichtbar |
| HDD-LED des Mainboards erfassen | ✅ | zusätzlich 10s-Aktivitätsfenster (`hdd_led_active_recently()`), da reines Momentan-Lesen kurze Blinkimpulse verpassen würde |
| Power-Taste-Status erfassen | ✅ | entprellt (30 ms), 5 ms-Poll |
| Reset-Taste-Status erfassen | ✅ | dito |
| NTC 10K B3590 (Temperatur) | ✅ | Beta-Formel statt volles Steinhart-Hart (siehe Entscheidungslog) |
| DHT11 (Temperatur + Feuchte) | ✅ | eigener Bit-Bang-Treiber |
| Schwellwert-Vergleich | ✅ | konfigurierbar über Web/USB/SNMP, RAM-only + teilweise persistiert (siehe Abschnitt 3) |
| Benachrichtigungsversand bei Überschreitung | 🟡 | **gebaut, noch nicht auf Hardware getestet.** Zwei Wege: Syslog (UDP) an einen zentralen Server, SMTP (bewusst ohne TLS) an alle Benutzer mit aktivierter Benachrichtigung in einer Sammel-Mail (Cc). Flankengetriggert (nicht bei jedem 60s-Zyklus erneut) UND zusätzlich mengenbegrenzt: max. 4 Mails/Stunde sofort, danach Sammel-Mail in Minute 59 (Timer-Task) — schützt vor serverseitigen Mail/Stunde-Limits |

### Steuerung (Lastenheft 5)

| Anforderung | Status | Anmerkung |
|---|---|---|
| Power-Taste auslösen | ✅ | kurz/lang, über Web (Einstellungen-Seite), USB-Kommando, **und SNMP SET** |
| Reset-Taste auslösen | ✅ | dito (nur kurz, kein "lang") |
| Gehäuse-Power-LED ansteuern | ✅ | über Web (Einstellungen-Seite, Checkbox) und USB (`led set power 0\|1`) — behoben 2026-07-17 |
| Gehäuse-HDD-LED ansteuern | ✅ | dito (`led set hdd 0\|1`) — behoben 2026-07-17 |
| Tastschutz (Sperre der Taster-Weiterleitung) | ✅ | Web-Schalter (Einstellungen-Seite) und USB (`tastschutz set 0\|1`) — Web-Schalter behoben 2026-07-17 |
| Watchdog-Anzeige (Onboard-RGB-LED, GPIO48) | ✅ | nicht im Lastenheft gefordert, auf Nutzerwunsch ergänzt — Farbverlauf zeigt an, dass die ESP-eigene FreeRTOS-Firmware läuft (nicht das Host-Betriebssystem), zusätzlich echte Task-Watchdog-Anbindung (Panic+Reboot bei Hänger) |

### Kommunikation (Lastenheft 6)

| Anforderung | Status | Anmerkung |
|---|---|---|
| WLAN (2,4 GHz) | ✅ | inkl. Scan, Beitritt, statische IP/DHCP, Auto-Reconnect bei Trennung |
| WireGuard-VPN | 🟡 | Kompiliert, Konfiguration ladbar (Upload/USB), Status abfragbar — **echter Tunnelaufbau nie gegen einen realen Peer getestet**; kein aktiver Health-Check/Reconnect auf ESP-BMC-Seite (nur was die Bibliothek selbst intern macht) |
| HTTP-Webserver | ✅ | `esp_http_server`, aktuell 20 Routen |
| WebSocket | ✅ | interaktive Konsole (`/ws/console`) |
| Webinterface mit serieller Konsole + WireGuard-Upload | ✅ | |
| SSH-Zugang zur seriellen Konsole | 🟡 | **gebaut (P7), noch nicht auf Hardware getestet.** Eigener SSH-Server (`wolfssl/wolfssh`), Passwort UND Public-Key-Auth gegen dieselbe `user_manager`-Kontodatenbank, gebrückt auf denselben CDC-Kanal wie die Web-Konsole (genau eine Sitzung gleichzeitig, Web oder SSH). Host-Key-Fingerprint + volle Public-Key-Zeile auf der Übersichtsseite sichtbar (nicht vertraulich, dient der Out-of-Band-Prüfung vor dem ersten Connect) |

### USB-Schnittstelle (Lastenheft 7 / Pflichtenheft 6)

| Anforderung | Status | Anmerkung |
|---|---|---|
| USB-CDC (virtuelle serielle Schnittstelle) | ✅ | dient zugleich als Konsolen-Bridge UND als Trägerkanal für das `##ESPR`-Kommandoprotokoll |
| USB-HID (Tastatur-Fallback) | ✅ | Composite-Deskriptor von Hand gebaut (Kconfig-Autogenerierung reicht mit aktivem HID nicht) |
| USB-Kommandoprotokoll (Diagnose/Export/Steuerung/Provisionierung) | ✅ | über Pflichtenheft 3.6/3.7 hinausgehend — 20 Kommandos: login/logout/status/log/config/reboot/reset/wg/wlan/taster/system/snmp/thresholds/tastschutz/storage |
| Serieller BIOS-Zugriff (Serial Console Redirection) | ⭕ | **nicht begonnen** — zusätzlicher UART zum Mainboard-Header nicht verdrahtet, Bauteilbedarf nicht in `docs/bom.md` nachgetragen |
| Grafische Bildschirmausgabe (HDMI/VGA) | ❌ | bewusst nicht umgesetzt (Lastenheft 7.3 / Pflichtenheft 6.1 explizit ausgeschlossen — technisch nicht machbar mit dieser Hardwarebasis) |

### Webinterface (Lastenheft 8)

| Anforderung | Status | Anmerkung |
|---|---|---|
| Serielle Konsole im Browser | ✅ | |
| WireGuard-Konfigurationsdatei hochladen | ✅ | mit automatischem Entfernen der Default-Route |
| Anzeige der Überwachungswerte | ✅ | inkl. 24h-Chart (Chart.js via CDN, einzige Stelle mit Client-JS) |
| Bedienung der Steuerungsfunktionen | ✅ | (mit der Tastschutz-Lücke von oben) |
| Tastschutz aktivieren/deaktivieren | ✅ | Web-Schalter behoben 2026-07-17 (siehe Steuerungs-Tabelle oben) |
| Rollenbasierte Benutzerverwaltung | ✅ | Leser/SSH User/Verwalter/Admin, über Lastenheft/Pflichtenheft hinausgehend aus `webconfig.txt` übernommen |
| Audit-Log | ✅ | über Lastenheft/Pflichtenheft hinausgehend aus `webconfig.txt`/`was loggen.txt` übernommen |
| SSH-Key hinterlegen | 🟡 | Selbstbedienungs-Formular auf der Übersichtsseite (nur eigener Account) — gebaut, nicht auf Hardware getestet, siehe SSH-Zugang oben |
| E-Mail-Benachrichtigung hinterlegen | 🟡 | Selbstbedienungs-Formular auf der Übersichtsseite (Adresse + Aktiv-Schalter, jede Rolle) — gebaut, nicht auf Hardware getestet, siehe Benachrichtigungsversand oben |
| SNMP-Monitoring | ✅ | nicht im Lastenheft gefordert, auf Nutzerwunsch ergänzt (eigene private MIB, GET+SET, 17 Objekte inkl. Firmwareversion) — Zabbix-Template `docs/zabbix-template-esp-bmc.yaml` (Vorbild sm-wlan) |
| OTA-Firmware-Update | 🟡 | nicht im Lastenheft gefordert, auf Nutzerwunsch ergänzt — lokaler .bin-Upload, Admin-only, Identitäts-/Downgrade-Prüfung + Bootloader-Rollback wie/über die Sensormeter-Familie hinaus (siehe `docs/entscheidungen.md`) — **gebaut, noch nicht auf Hardware getestet**. Erste Version: `0.9.0-rc4` |

### Sicherheit (Lastenheft 9 / Pflichtenheft 7)

| Anforderung | Status | Anmerkung |
|---|---|---|
| Webserver auch über WireGuard erreichbar, nicht exklusiv | ✅ | (architekturell so gebaut, aber siehe VPN-Vorbehalt oben) |
| Eigene Authentifizierung für Nicht-VPN-Zugriff | ✅ | Session-Cookie (Web), Login+Rolle (USB), getrennte Lese-/Schreib-Community (SNMP) — war in Pflichtenheft 12 als offene Entscheidung gelistet, ist es nicht mehr |
| Galvanische Entkopplung Taster-Weiterleitung | 🟡 | Software/Ablauf-Logik fertig, Verdrahtung final geplant (PC817, siehe `docs/verdrahtungsplan.html`) — elektrische Umsetzung mangels Hardware nicht verifiziert |

---

## 2. Noch offene / geplante Features

Aus Pflichtenheft Abschnitt 12 ("Offene technische Entscheidungen"),
mit aktuellem Stand:

| Punkt | Ursprünglicher Stand (Pflichtenheft) | Aktueller Stand |
|---|---|---|
| WireGuard-Bibliothek | offen | ✅ entschieden (droscy/esp_wireguard) — nur der Hardware-Test fehlt noch |
| Partitionstabelle | offen | ✅ entschieden und gebaut (siehe Abschnitt 3) |
| SSH-Zugang | echtes SSH vs. TCP-Tunnel | ✅ entschieden und gebaut (eigener SSH-Server auf dem ESP, `wolfssl/wolfssh`) — **Hardware-Test steht noch aus** |
| Authentifizierung Webserver | offen | ✅ entschieden und gebaut |
| Versandweg Benachrichtigungen | offen | ✅ entschieden und gebaut: Syslog (UDP) + SMTP ohne TLS (bewusst, kein zweiter Krypto-Stack neben wolfSSL) — **Hardware-Test steht noch aus** |
| Konfigurationsformat | XML (Kandidat) vs. JSON | ✅ entschieden: JSON (cJSON), nicht XML wie ursprünglich vermutet |
| Log-/Diagnoseformat | offen | ✅ entschieden und gebaut (persistentes Audit-Log mit Rotation, analog Sensormeter-Familie) |
| GPIO-Pinbelegung | offen, hängt vom Pinout ab | ✅ **entschieden und final festgelegt** (2026-07-18, Sensor-Pins am 2026-07-20 auf die linke Leiste verschoben) — alle 10 Kanäle liegen jetzt auf einer einzigen Pinleiste (Huckepack-Platine braucht nur eine Stiftleiste), gegen Vendor-Pinout-Bilder + ESP-IDF-SoC-Header abgeglichen, keine Kollision mit Strapping-Pins/JTAG/UART/USB. Vollständiger interaktiver Verdrahtungsplan: `docs/verdrahtungsplan.html`. **Noch nicht auf echter Hardware verifiziert** (kein Board vorhanden) |
| Bauteilbedarf serieller BIOS-Zugriff | offen | ⭕ weiterhin offen, `docs/bom.md` nicht nachgetragen |

Zusätzlich, unabhängig von der ursprünglichen offenen-Punkte-Liste:

- **P6** — WireGuard-Normalbetrieb auf echter Hardware (Tunnelaufbau gegen einen echten Peer) — nicht testbar ohne Board.
- **P7** — SSH-Server — gebaut (Passwort+Public-Key-Auth, eigener Server auf dem ESP), noch nicht auf echter Hardware/gegen einen echten SSH-Client getestet.
- **P8** — Verkabelung/Ende-zu-Ende-Test — nicht begonnen.
- Generisches `config upload` über USB (ganze Konfiguration in einem Rutsch, aus `inital setup.txt`) — bewusst zurückgestellt, da `config_manager` bis vor kurzem komplett RAM-only war; einzelne Felder (Systemname, SNMP, Schwellwerte, Tastschutz) sind seit heute per USB einzeln setzbar, ein zusammenfassendes Upload-Kommando fehlt aber weiterhin.
- Serieller BIOS-Zugriff (Serial Console Redirection, Lastenheft 7.3/Pflichtenheft 6.2).
- Aktive WireGuard-Tunnel-Überwachung/Reconnect auf ESP-BMC-Seite (Pflichtenheft 8.2) — bislang nur die passive Status-Abfrage.
- Host-seitiges Setup-Tooling (`tools/Setup.ps1`) existiert bereits, aber ohne echten Hardware-Test und ohne Anbindung an ein Git-Remote (existiert noch nicht).

---

## 3. Partitionstabelle

Eigene Tabelle (`firmware/partitions.csv`), ersetzt das
PlatformIO-Standardpreset `partitions_two_ota.csv` (1 MB je OTA-Slot —
reichte nicht mehr, sobald TinyUSB + Webserver dazukamen, siehe
Pflichtenheft Abschnitt 10).

| Name | Typ | Subtyp | Offset | Größe | Zweck |
|---|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 0x5000 (20 KB) | ESP-IDF-Systemkonfiguration (WLAN-Treiber etc.) |
| otadata | data | ota | 0xE000 | 0x2000 (8 KB) | OTA-Slot-Auswahl |
| ota_0 | app | ota_0 | 0x10000 | 0x200000 (2 MB) | Firmware-Slot A |
| ota_1 | app | ota_1 | 0x210000 | 0x200000 (2 MB) | Firmware-Slot B |
| storage | data | spiffs | 0x410000 | 0x100000 (1 MB) | LittleFS — Config-JSONs, Audit-Log, Benutzerkonten |

Summe: 0x510000 Byte ≈ 5,06 MB von 16 MB verfügbarem Flash — reichlich
Reserve. `nvs`+`otadata` sind bewusst exakt so bemessen (0x7000 Byte
zusammen), dass `ota_0` direkt auf der von App-Partitionen geforderten
64-KB-Grenze (0x10000) beginnt, ohne Padding zu verschenken.

## 4. Aktueller Flash-/RAM-Füllstand

Letzter erfolgreicher Build, Umgebung `esp32-s3-devkitc-1-n16r8`
(2026-07-18, nach P7/SSH-Server + Host-Key-Fingerprint-Anzeige +
Watchdog-LED + Benachrichtigungswege Syslog/SMTP + E-Mail-Entprellung +
OTA-Firmware-Update + SNMP-Firmwareversion):

| Ressource | Belegt | Kapazität | Auslastung |
|---|---|---|---|
| Flash (App-Partition `ota_0`) | 1.295.317 Byte | 2.097.152 Byte (2 MB) | **61,8 %** |
| RAM | 65.264 Byte | 327.680 Byte | **19,9 %** |

Zuwachs auf 61,6 % kam vom OTA-Update-Feature (`esp_ota_ops`, eigener
Multipart-Handler, Marker-Scan), der letzte kleine Zuwachs auf 61,8 %
von der neuen SNMP-Firmwareversion-OID - siehe `docs/entscheidungen.md`
"OTA-Update im Webinterface ..." und "SNMP-Firmwareversion +
Zabbix-Template".

Seit 2026-07-18 gibt es zusätzlich `esp32-s3-devkitc-1-n8r8` (8 statt
16 MB Flash, sonst identisches Board, kein eigenes Board vorhanden — nur
Fußabdruck-Tracking wie beim P1-WireGuard-Spike, siehe
`docs/entscheidungen.md` "Portierbarkeit auf ESP32-S3-DevKitC-1-N8R8").
Baut auf denselben Stand praktisch identisch — `partitions.csv`
(≈5,06 MB Summe) passt unverändert auch unter 8 MB Flash. Wird ab jetzt
bei jeder Build-Verifikation mitgebaut.

Sprung von 51,3 % auf 59,1 % kommt vom SSH-Server (P7, `wolfssl/wolfssh`
+ `wolfssl/wolfssl`, echte ECC-Kryptografie fuer Host-Key/Handshake/
Verschluesselung) - siehe `docs/entscheidungen.md` "SSH-Server (P7)". Der
kleine weitere Zuwachs auf 59,2 % kommt vom Host-Key-Fingerprint/
Public-Key-Export auf der Übersichtsseite (Base64-Encoder + SSH-
Wire-Format-Aufbau, siehe Nachtrag in `docs/entscheidungen.md`). Der
Sprung auf 60,4 % kommt vom `espressif/led_strip`-Treiber (RMT-Backend)
für die neue Watchdog-LED, siehe `docs/entscheidungen.md`
"Watchdog-LED (RGB, GPIO48)". Der letzte Zuwachs auf 60,8 % kommt vom
Syslog/SMTP-Benachrichtigungsweg (reiner Socket-Code, kein zweiter
Krypto-Stack, bewusst ohne TLS — siehe `docs/entscheidungen.md`
"Benachrichtigungswege: Syslog + SMTP ohne TLS").

Zum Vergleich der P1-Machbarkeits-Spike-Wert aus Pflichtenheft Abschnitt
10 (WLAN+WireGuard allein, vor TinyUSB/Webserver/USB-Protokoll/SNMP/SSH):
771.709 Byte (73,6 % der **damals noch 1 MB großen** Standard-Partition).
Trotz seitdem massiv gewachsenem Funktionsumfang (TinyUSB, kompletter
Webserver mit 21 Routen, 20-Kommando-USB-Protokoll, SNMP-Agent mit
selbstgeschriebenem BER-Encoder, eigener SSH-Server, RGB-Watchdog-LED,
Syslog/SMTP-Benachrichtigung) liegt die Auslastung dank der größeren
2-MB-Partition weiterhin niedriger als der ursprüngliche Spike-Wert.
Flash ist aktuell kein Risikofaktor (39,2 % Reserve) — RAM war es nie
(durchgehend unter 20 %).

Wokwi-Sim-Umgebung diese Runde nicht neu gebaut (auf Nutzerwunsch
pausiert, siehe Projekt-Memory) - letzter bekannter Stand (vor P7):
Flash 1.055.069 Byte / 50,3 %, RAM 53.216 Byte / 16,2 %.
