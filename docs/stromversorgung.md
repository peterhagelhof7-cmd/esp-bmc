# Strombedarf & Stromversorgung

Strombudget für ein ESP-BMC-Gerät, damit ein passendes 5-V-Netzteil
gewählt werden kann bzw. damit klar ist, dass die ATX-+5VSB-Schiene des
verwalteten PCs ausreicht. Unterschieden wird zwischen **Durchschnitt**
(Dauerlast/Wärmeentwicklung) und **Spitze** (wie kräftig die Quelle
kurzzeitig sein muss). Alle Angaben sind **Schätzungen aus
Datenblattwerten und Code-Analyse — nicht real nachgemessen**, da
ESP-BMC noch nie an echter Hardware geflasht wurde (siehe
`docs/entscheidungen.md`, Stand 2026-07-18).

## Zwei Versorgungswege — niemals gleichzeitig

Anders als bei der Sensormeter-Familie (dort USB-C oder PoE, beides über
unabhängige galvanisch getrennte Netzteil-/PoE-Wandlerpfade) teilen sich
bei ESP-BMC **beide** Versorgungswege dasselbe 5-V-Netz auf dem Board.
Wortlaut der Warnung aus `docs/verdrahtungsplan.html` Abschnitt 2 (hier
bewusst nicht abgeschwächt übernommen):

> **⚠ Gefahr: Dual Powering — nie zwei aktive 5V-Quellen gleichzeitig
> anschließen.** 5Vin-Pin und beide USB-VBUS-Anschlüsse liegen intern auf
> demselben 5V-Netz des Boards. Werden zwei dieser Quellen gleichzeitig
> aktiv angeschlossen (z. B. ATX +5VSB an `5Vin` UND gleichzeitig ein
> USB-Kabel zu einem eingeschalteten Host-Rechner an Port A oder B),
> speisen beide unkoordiniert in dasselbe Netz ein — ohne
> Power-ORing-Schaltung (Dioden, idealer Dioden-Controller) kann Strom in
> die jeweils andere Quelle zurückfließen. Mögliche Folgen: beschädigter
> USB-Host-Controller des Rechners, beschädigte Standby-Regelung im
> ATX-Netzteil, oder eine undefinierte Spannung auf dem 5V-Netz des
> Boards selbst.

Regel unverändert: entweder USB (Port A **oder** Port B) **oder**
`5Vin`/ATX +5VSB — nie beides gleichzeitig, außer mit einer eigens
ergänzten Schottky-Dioden-ODER-Schaltung (nicht mitgeliefert).

## Strombudget pro Komponente (bei 3,3–5 V)

| Komponente | Ø-Strom | Spitzenstrom | Quelle |
|---|---|---|---|
| ESP32-S3 (WLAN + WireGuard-Krypto aktiv) | ~80–120 mA | ≥ 500 mA (WLAN-TX-Burst) | Espressif ESP32-S3-Datenblatt: TX-Spitzen bis ~700 mA bei voller Sendeleistung. WireGuard (Curve25519/ChaCha20-Poly1305) ist CPU-lastig, aber kein zusätzlicher HF-Verbraucher — kein relevanter Strom-Zuschlag über die ohnehin aktive CPU-Grundlast hinaus. |
| PC817-Optokoppler ×2 (LED-Seite, Power-/Reset-Weiterleitung) | ~0 mA (Ruhe) | ~7,8 mA je Kanal bei aktivem Tastendruck (`I = (3,3 V − V_f≈1,2 V) / 270 Ω`, `docs/verdrahtungsplan.html` Vorwiderstand) | Standard-PC817-Datenblatt (LED-Vorwärtsspannung ~1,2 V), eigene Berechnung mit dem im Verdrahtungsplan festgelegten 270-Ω-Vorwiderstand. Nur aktiv, solange ein Tastendruck simuliert wird (typ. < 1 s) — kein Dauerverbraucher. |
| *(Alternative zu PC817: NPN-Transistoren, siehe `bom.md` #5b)* | ~0 mA (Ruhe) | ~1 mA Basisstrom je Kanal bei aktivem Tastendruck (1-kΩ-Basiswiderstand) | Vernachlässigbar gegenüber der Optokoppler-Variante, hier nicht als eigene Zeile in der Summenrechnung geführt (nur eine der beiden Varianten wird bestückt). |
| NTC 10K B3590 (Temperatur) | ~0,1–0,3 mA | < 1 mA | Spannungsteiler gegen 3,3 V mit 10-kΩ-Festwiderstand (`bom.md` #3) — Dauerstrom durch den Teiler selbst, nicht das Element allein; 60-s-Poll-Takt (`sensor_manager.c`) ändert am *stationären* Teilerstrom nichts, nur an der ADC-Lesefrequenz. |
| DHT11 (Temperatur + Feuchte) | ~0,3–0,5 mA | ~2,5 mA | Standard-DHT11-Datenblattwerte; 60-s-Abfragetakt (`SENSOR_TASK_INTERVAL_MS`), Sensor liegt die meiste Zeit im Standby. |
| WS2812-RGB-LED (Watchdog-LED, GPIO48, onboard fest verdrahtet) | ~3–6 mA | ~9,4 mA (rechnerisches Maximum bei der aktuell im Code gesetzten Helligkeit) | Siehe eigene Herleitung unten. |
| PC817-Fototransistor-Seite (Ausgang zum Mainboard-Header) | 0 mA vom ESP-BMC-Netz — galvanisch getrennt, wird vom Mainboard selbst versorgt | 0 mA vom ESP-BMC-Netz | `docs/verdrahtungsplan.html` Abschnitt 4: Fototransistor-Kollektor/Emitter hängt ausschließlich am Mainboard-Header, keine elektrische Verbindung zur ESP-Seite — daher **nicht** Teil dieses Budgets. |

### WS2812-Stromabschätzung im Detail

Datenblatt-Referenz: ein WS2812(B) zieht bei **voller Helligkeit** (RGB
je 255/255) bis zu ~20 mA pro Farbkanal, also bis ~60 mA bei reinem
Weiß — das ist der oft zitierte Werkswert für Budget-Zwecke mit
Sicherheitsaufschlag.

`watchdog_manager.c` setzt die LED aber **nicht** auf volle Helligkeit:

```c
// value=40 statt 255: als Dauer-Statusanzeige gedacht, nicht als
// helle Beleuchtung
led_strip_set_pixel_hsv(s_strip, 0, hue, 255, 40);
```

`value = 40` von 255 (~16 %) bei voller Sättigung (`saturation = 255`)
bedeutet: pro Hue-Winkel sind höchstens **zwei** der drei Farbkanäle
gleichzeitig ungleich null, keiner davon über dem Value-Limit von
40/255. Rechnerisch: `40/255 × 20 mA ≈ 3,14 mA` je aktivem Kanal, bei
den seltenen Übergangswinkeln mit zwei aktiven Kanälen in Summe bis
`≈ 6,3 mA`. Der oben tabellierte Spitzenwert (9,4 mA) ist der
theoretische Vollweiß-Wert bei `value = 40` auf allen drei Kanälen
gleichzeitig (`3× 3,14 mA`) — bei `saturation = 255` in der aktuellen
Firmware nie erreicht, hier trotzdem als konservative obere Grenze
geführt, falls die Sättigung künftig geändert wird. Der volle
60-mA-Datenblattwert (Value 255, Weiß) ist **nicht** repräsentativ für
den aktuellen Code, wird hier nur als Referenz-Obergrenze für künftige
Firmware-Änderungen genannt.

## Gesamtbedarf pro Gerät

| Konfiguration | Ø-Strom (Dauerlast, 5 V) | Spitzenstrom |
|---|---|---|
| **Normalbetrieb** (ESP32-S3 + beide Sensoren + Watchdog-LED, keine Taste gerade gedrückt) | ~85–130 mA | ~510–730 mA (WLAN-TX-Burst dominiert) |
| **Worst Case** (zusätzlich: Power- oder Reset-Taste wird gerade weitergeleitet) | ~85–130 mA | ~525–745 mA (+ ~15,6 mA für beide Optokoppler-Kanäle im theoretischen Gleichzeitig-Fall) |

Der Spitzenwert wird praktisch vollständig vom ESP32-S3 selbst bestimmt
(WLAN-Sendebursts, Flash-Schreibzugriffe bei Config/Audit-Log/OTA) —
die Optokoppler-/LED-Zuschläge sind dagegen im Vergleich klein
(< 3 % des Gesamtspitzenwerts).

## Versorgungsweg 1: USB-C, 5 V

**5-V-USB-C-Netzteil, mindestens 1 A (1000 mA).**

Analog zur Sensormeter-Familie: 1 A statt der bloßen ~130-mA-
Dauerlast-Schätzung gibt großzügig Reserve gegen Spannungsabfall durch
dünne/billige Kabel und deckt den ermittelten Spitzenwert (~745 mA)
sicher ab. Nicht verwenden: der Ausgang des USB-UART-Flash-Adapters
(Port A) als Dauerversorgung — dieser ist für Flash-/Debug-Sitzungen
gedacht, nicht als belastbare Dauerstromquelle spezifiziert. Für den
Dauerbetrieb Port B (natives USB) oder — sofern gewünscht — `5Vin`
verwenden (siehe Weg 2), nie beide gleichzeitig (siehe Warnung oben).

## Versorgungsweg 2: ATX +5VSB

Einspeisung über `5Vin` + `GND` (siehe `docs/verdrahtungsplan.html`
Abschnitt 2 und `docs/bom.md` #10 für das Anzapfkabel), **nicht** über
einen USB-Port. Die ATX-+5VSB-Standby-Schiene liegt an, sobald das
Netzteil am Stromnetz hängt — unabhängig vom Einschaltzustand des PCs,
genau die Voraussetzung dafür, dass sich der PC per Fernzugriff über
ESP-BMC überhaupt wieder einschalten lässt (Kernfunktion des Geräts).

**Belastbarkeit:** Standard-ATX-Netzteile spezifizieren +5VSB
üblicherweise mit 1,5–3 A (herstellerabhängig, siehe Typenschild/
Datenblatt des jeweiligen Netzteils) — das ermittelte ESP-BMC-Budget
(~745 mA Spitze, ~130 mA Dauerlast) liegt damit komfortabel innerhalb
der Reserve praktisch jedes modernen ATX-Netzteils, ohne dass ESP-BMC
allein die +5VSB-Schiene nennenswert auslastet. Trotzdem gilt: die
+5VSB-Schiene versorgt je nach Mainboard u. U. weitere Verbraucher
(Wake-on-LAN-Logik, USB-Aufladung im ausgeschalteten Zustand,
LED-Beleuchtung) — im Zweifel das tatsächliche Netzteil-Typenschild
gegen `~745 mA + bereits bestehende +5VSB-Last` prüfen, bevor
ESP-BMC dauerhaft angeschlossen wird.

> **Nicht nachgemessen:** Ob der tatsächliche 5-V-Pfad auf einem realen
> ATX-Netzteil unter Volllast (WLAN-Burst + gleichzeitige
> Tastenweiterleitung) sauber bleibt, wurde mangels vorhandener Hardware
> noch nicht real getestet. Vor dem produktiven Dauerbetrieb über
> ATX +5VSB empfiehlt sich eine kurze Kontrollmessung mit Multimeter/
> Oszilloskop am `5Vin`-Pin.

## USB-C und ATX +5VSB gleichzeitig

**Nicht zulässig ohne zusätzliche Power-ORing-Schaltung** — siehe die
Dual-Powering-Warnung oben, wortgleich mit `docs/verdrahtungsplan.html`
Abschnitt 2 und `README.md`s Hardware-Abschnitt. Diese Einschränkung ist
für ESP-BMC **strenger** als bei der Sensormeter-Familie (dort sind
USB-C und PoE galvanisch über unabhängige DC/DC-Pfade getrennt und daher
unproblematisch gleichzeitig nutzbar) — hier teilen sich beide Wege
dasselbe interne 5-V-Netz, ein Sicherheitsrisiko bei gleichzeitigem
Anschluss, kein bloßer Komfort-Hinweis.
