# ESP-BMC

ESP32-S3-basiertes Modul zur Fernsteuerung, Überwachung und Diagnose eines
Standard-PCs über WLAN und WireGuard-VPN — ein kompaktes "BMC-lite":
erfasst Power-/HDD-LED und Power-/Reset-Taster des Mainboards, kann beide
Taster ferngesteuert auslösen, liest zwei Temperatursensoren (NTC 10K
B3590, DHT11) aus und meldet Schwellwertüberschreitungen per E-Mail/
Syslog. Bedienung über Weboberfläche (inkl. serieller Konsole per
WebSocket zum angeschlossenen PC), SSH, SNMP und eine native USB-CDC-
Schnittstelle, die sich zugleich als USB-HID-Tastatur ausgibt.

[**One-Pager (PDF)**](docs/esp-bmc-onepager.pdf) — kompakte Projektübersicht auf einer Seite.

## Dokumentation

| Datei | Inhalt |
|---|---|
| [docs/esp-bmc-onepager.pdf](docs/esp-bmc-onepager.pdf) | One-Pager: Projektübersicht, Architektur, Kennzahlen auf einer Seite |
| [docs/lastenheft.txt](docs/lastenheft.txt) | Fachliche Anforderungen |
| [docs/pflichtenheft.txt](docs/pflichtenheft.txt) | Technische Umsetzung: Module, Pinbelegung, Speicherlayout |
| [docs/implementierungsplan.html](docs/implementierungsplan.html) | Visueller Implementierungsplan (lokal im Browser öffnen) |
| [docs/entscheidungen.md](docs/entscheidungen.md) | Entscheidungsprotokoll: Boardwahl, Pinbelegung, OTA, SNMP, bekannte Abweichungen |
| [docs/board-recherche.md](docs/board-recherche.md) | Board-Recherche zur Hardwareentscheidung (diymore ESP32-S3 DevKitC-1 N16R8), inkl. [Foto](docs/board-foto.jpg) |
| [docs/bom.md](docs/bom.md) | Bauteile pro Gerät (inkl. Optokoppler-/Transistor-Variante, Spannungsteiler) |
| [docs/verdrahtungsplan.html](docs/verdrahtungsplan.html) | Interaktiver Verdrahtungsplan: alle Pins, USB-Portzuordnung, ATX-+5VSB-Option, Dual-Powering-Warnung |
| [docs/admin-guide.pdf](docs/admin-guide.pdf) ([HTML](docs/admin-guide.html)) | Admin-Guide: Inbetriebnahme, Weboberfläche, SSH, OTA, SNMP/Zabbix, USB-Kommandozeile |
| [docs/zabbix-template-esp-bmc.yaml](docs/zabbix-template-esp-bmc.yaml) | Fertiges Zabbix-Template (17 SNMP-Objekte) |
| [docs/projektstand.md](docs/projektstand.md) | Fortschrittstabelle: was ist fertig, was noch offen |
| [tools/Setup.ps1](tools/Setup.ps1) | PowerShell-Skript: Abhängigkeiten installieren, bauen, Flash-Größe erkennen, flashen, Ersteinrichtung per USB |
| [tools/EspBmcLink.psm1](tools/EspBmcLink.psm1) | PowerShell-Modul für die USB-Kommandoverbindung (auch von `Setup.ps1` genutzt) |

## Hardware

- diymore ESP32-S3-DevKitC-1 **N16R8** (16 MB Flash, 8 MB PSRAM) — primäre
  Zielhardware; eine **N8R8**-Variante (8 MB Flash) wird als zweite
  PlatformIO-Umgebung dauerhaft mitgebaut, siehe
  [docs/entscheidungen.md](docs/entscheidungen.md)
- Nativer USB-Controller (USB-CDC + USB-HID-Tastatur) getrennt vom
  USB-UART-Bridge-Port zum Flashen — siehe
  [docs/verdrahtungsplan.html](docs/verdrahtungsplan.html) für die genaue
  Portzuordnung
- Power-/Reset-Weiterleitung zum Mainboard-Header galvanisch entkoppelt
  über PC817-Optokoppler (empfohlene Variante); eine Transistor-Variante
  ohne Optokoppler ist als Alternative dokumentiert
- Alternative Stromversorgung über ATX **+5VSB** möglich — **niemals
  gleichzeitig mit USB-Stromversorgung** (Dual-Powering-Gefahr, beide
  Quellen teilen sich denselben 5V-Netz), siehe Warnhinweis im
  Verdrahtungsplan

## Firmware

`firmware/` ist ein PlatformIO-Projekt (Board `esp32-s3-devkitc-1`,
Framework **ESP-IDF**, kein Arduino).

**Version:** `0.9.0-rc4` (Beta) — gleiche Version wie die
Sensormeter-Projektfamilie zum Zeitpunkt der ersten Beta.

Einrichten per PowerShell-Skript (baut, erkennt die Flash-Größe des
angeschlossenen Boards automatisch, flasht, führt durch die
Ersteinrichtung per USB):

```powershell
tools\Setup.ps1
```

Manuelle Alternative ohne Skript:

```
cd firmware
pio run -e esp32-s3-devkitc-1-n16r8 -t upload
```

### Module

- `GpioManager`: Power-/HDD-LED-Erfassung, Power-/Reset-Taster-Erfassung
  und -Weiterleitung (inkl. Tastschutz), finale Pinbelegung gegen
  Vendor-Bilder und ESP-IDF-SoC-Header abgeglichen
- `SensorManager`: NTC 10K B3590 + DHT11, kantengetriggerte
  Schwellwert-Auslösung (löst nur beim Übergang in den Alarmzustand aus,
  nicht bei jedem Messzyklus)
- `NetworkManager`/`WireguardManager`: WLAN-Anbindung + WireGuard-VPN,
  Weboberfläche auch über VPN erreichbar
- `WebServerManager`: Hauptseite, passwortgeschützte Einstellungsseite,
  WebSocket-Konsole zum angeschlossenen PC, WireGuard-Config-Upload,
  admin-only Firmware-Update per Multipart-Upload
- `UsbManager`: natives USB-CDC + USB-HID-Tastatur-Fallback,
  Konsolen-Arbitrierung zwischen Web- und SSH-Konsole
- `SshManager`: SSH-Zugang zur seriellen Konsole (nur ECDSA/Ed25519-
  Client-Keys, RSA projektweit deaktiviert)
- `SnmpManager`: SNMP v1/v2c read-only unter `.1.3.6.1.4.1.99999.x`,
  17 Objekte inkl. Firmware-Version
- `OtaManager`: OTA-Update mit Identitäts-/Downgrade-Prüfung (byte-sicherer
  Marker-Scan, `esp_ota_ops`-basiert), Bootloader-Rollback bei
  fehlgeschlagenem Neustart aktiviert
- `NotificationManager`: Syslog (UDP) + SMTP-Benachrichtigung bei
  Schwellwertüberschreitung, mit Entprellung (erste 4 Alarme sofort, ab
  dem 5. Sammel-Mail zur vollen Stunde, alle Empfänger per CC)
- `WatchdogManager`: onboard RGB-LED (WS2812, GPIO48) als sichtbares
  Lebenszeichen, Task-Watchdog-Anmeldung mit aktiviertem Panic-Reboot bei
  hängenden Tasks
- `UserManager`: rollenbasierte Benutzerkonten, SSH-Public-Key- und
  E-Mail-Benachrichtigungs-Selbstbedienung pro Konto
- `ConfigManager`/`StorageManager`: Persistenz auf LittleFS
- USB-Kommandozeile für den Fall, dass das Gerät nur per USB, aber nicht
  per Netzwerk erreichbar ist — siehe
  [docs/admin-guide.pdf](docs/admin-guide.pdf) Abschnitt 9

Aktueller Stand siehe [docs/projektstand.md](docs/projektstand.md).

## Über dieses Projekt

Repo-Struktur und Dokumentation entstehen in Zusammenarbeit mit
[Claude](https://claude.com/claude-code) (Anthropic) als KI-Coding-Assistent.

## Lizenz

[MIT](LICENSE)
