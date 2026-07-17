# ESP-BMC — Hardwarebasis (festgelegt)

Board-Entscheidung: **diymore 2× ESP32-S3 DevKitC-1 N16R8**
Festgelegt 2026-07-16.

Amazon.de: https://www.amazon.de/diymore-ESP32-S3-DevKitC-1-Entwicklungsboard-Anschlie%C3%9Fbare-ESP32-S3-TYP-C/dp/B0F3XMYYQY
(ASIN `B0F3XMYYQY`, auch hinterlegt in `amazon link.url`)

## Produkt

- **Titel**: diymore 2PCS für ESP32-S3 DevKitC-1 N16R8 Modul, für ESP32 S3
  1-N16R8 Entwicklungsboard mit WiFi, Bluetooth 5.0, USB C Anschließbare
  Antenne
- **Marke / Verkäufer**: diymore® (Direktverkauf, nicht nur Reseller)
- **Preis**: 16,98 € für 2 Stück (≈ 8,49 €/Stück)
- **Bewertung**: 4,2★ bei 65 Bewertungen, „Amazons Tipp", 100+ mal im
  letzten Monat verkauft
- **Versand**: Amazon, auf Lager

## Technische Daten

| Merkmal | Wert |
|---|---|
| Chipsatz | ESP32-S3 |
| Prozessor | Xtensa 32-Bit LX7, Dual-Core |
| WiFi | 802.11 b/g/n, 20/40 MHz Bandbreite, 1T1R-Modus, bis 150 Mbps |
| WiFi-Zusatzfunktionen | WMM, A-MPDU, A-MSDU, Instant Block Confirmation |
| Bluetooth | Bluetooth 5.0 (LE) + Bluetooth Mesh |
| Sendeleistung | bis 20 dBm (High-Power-Modus), zusätzlicher Low-Power-Coprozessor |
| Flash | **16 MB** |
| PSRAM | **8 MB** (auf der Amazon-Spec-Tabelle irreführend als „RAM" gelistet) |
| USB | **2× USB-C** — nativer USB-Port + separater USB-UART-Bridge-Port |
| Antenne | extern anschließbar (U.FL-Buchse) |
| Pinleisten | vollständige 44-Pin-Header beidseitig, Standard-DevKitC-1-Footprint |
| Formfaktor | „normale" DevKitC-1-Bauform (kein SuperMini-Miniaturmodul) |
| Kompatibilität | Arduino IDE, ESP-IDF (laut Produktbeschreibung) |

## Warum dieses Board (Kurzfassung der Entscheidung)

- 16 MB Flash statt der ursprünglich angenommenen 8 MB - mehr Reserve für
  WireGuard + Webserver + WebSocket + TinyUSB + Sensor-Logik zusammen auf
  einem Chip.
- 8 MB PSRAM vorhanden (im Projektentwurf als „optional" gewünscht).
- Zwei USB-C-Ports trennen sauber die Rollen: einer für die USB-CDC/HID-
  Verbindung zum gesteuerten PC (siehe Projektbeschreibung.txt), der
  andere für Flashen/Debuggen über die USB-UART-Bridge - kein
  Umstecken/Konflikt zwischen beiden Funktionen nötig.
- Deutlich etablierteres Angebot als die zuvor geprüften Alternativen
  (65 Bewertungen + „Amazons Tipp" + Direktverkauf durch die Marke,
  gegenüber Angeboten mit 2 Bewertungen oder sogar fehlerhaft
  vermischten Chip-Angaben bei anderen Anbietern).
- Standard-DevKitC-1-Footprint - Pinout und Beispielcode für dieses
  Referenzdesign sind breit dokumentiert (Espressif-Referenzdesign,
  nicht ein proprietärer No-Name-Formfaktor).

## Offene Punkte für die Firmware-/Hardware-Planung

- Exakte GPIO-Pinbelegung noch nicht aus der Amazon-Seite entnommen
  (keine Pin-Zahl/Diagramm im Angebotstext) - vor dem Schaltungsentwurf
  das Referenz-Pinout für ESP32-S3-DevKitC-1 (Espressif-Doku) heranziehen,
  das für dieses Board gelten sollte, da Standard-Footprint.
- Physische Größe (Standard-DevKitC-1, nicht SuperMini) bei der
  „kompaktes Modul"-Zielsetzung aus der Projektbeschreibung berücksichtigen
  - für den ersten Prototyp unkritisch, für die Endintegration ggf.
  relevant.
- Flash-Budget-Check trotz 16 MB weiterhin sinnvoll, sobald WireGuard-
  Bibliothek + TinyUSB + Webserver zusammen gebaut werden (siehe
  ursprüngliche Projekt-Risikoeinschätzung zu WireGuard auf ESP32).
