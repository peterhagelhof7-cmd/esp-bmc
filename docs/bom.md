# ESP-BMC — Stückliste (BOM)

Stand 2026-07-18 (Pinbelegung final, siehe `docs/verdrahtungsplan.html`),
basierend auf `Projektbeschreibung.txt` und `docs/board-recherche.md`.
Reine Komponentenliste für einen ersten Prototyp (Breadboard/Lochraster)
— noch kein PCB-Layout.

| # | Bauteil | Menge benötigt | Verpackungseinheit | Preis (ca.) | Zweck |
|---|---|---|---|---|---|
| 1 | diymore ESP32-S3 DevKitC-1 N16R8 | 1 | 2er-Pack | 16,98 € | Hauptboard (siehe `docs/board-recherche.md`) |
| 2 | NTC-Thermistor 10K B3590, 1% | 1 | 10er-Pack | 7,39 € | Temperaturerfassung |
| 3 | Festwiderstand 10 kΩ (für NTC-Spannungsteiler) | 1 | Sortiment/vorhanden | < 1 € | Spannungsteiler NTC ↔ ADC-Pin |
| 4 | DHT11-Sensor, 3-Pin-Modul (mit eingebautem Pull-up) | 1 | Einzeln oder 3er-Pack | ~5 € | zweiter Temperatur-/Luftfeuchtewert |
| 5 | PC817-Optokoppler | 2 | 10er-Pack | 6–8,50 € | **empfohlene** Ausbaustufe: galvanische Entkopplung auf der Ausgangsseite ESP→Mainboard-Header (Power/Reset auslösen) — siehe #5b für die Alternative |
| 5b | *Alternative zu #5:* NPN-Kleinsignaltransistor (BC547/2N2222) + Basiswiderstand 1 kΩ | je 2 | Sortiment | < 2 € | nur falls bewusst auf Optokoppler verzichtet wird — **ohne galvanische Trennung**, siehe `docs/verdrahtungsplan.html` Abschnitt 4 |
| 6 | Vorwiderstand ~220–330 Ω (Optokoppler-LED-Seite) | 2 | Sortiment/vorhanden | < 1 € | Strombegrenzung für PC817-Eingangs-LED (nur bei #5) |
| 6b | **Neu**: Spannungsteiler LED-Erfassung — 10 kΩ + 20 kΩ | je 2 | Sortiment/vorhanden | < 1 € | schützt die 3,3-V-only-GPIOs vor 5-V-referenzierten Power-/HDD-LED-Headern (Lastenheft 10.1 ging von „kein Bauteil" aus — Sicherheitsergänzung, siehe `docs/verdrahtungsplan.html` Abschnitt 3) |
| 7 | Dupont-Jumper-Kabel (m/w, m/m je nach Bedarf) | ~15–20 | Sortiment | 3–5 € | Prototyp-Verkabelung zu Mainboard-Headern |
| 8 | Lochrasterplatine oder Breadboard | 1 | Einzeln | 2–5 € | Trägerplatine für Optokoppler-/Transistor-Schaltung |
| 9 | Gehäuse-Taster-Erfassung (Power, Reset) | 2 Eingangs-Pins | — | 0 € | direkt auf ESP-Pins, interner Pull-up reicht — kein Zusatzbauteil |
| 10 | **Neu**: ATX-24-Pin-Anzapfkabel oder einzelne Buchsenkontakte | 2 Adern | Sortiment | < 2 € | alternative Stromversorgung über ATX +5VSB (Pin 9, violett) + GND (schwarz) — siehe `docs/verdrahtungsplan.html` Abschnitt 2, **nie gleichzeitig mit USB anschließen** |

## Geschätzte Gesamtkosten

**~35–48 €** für den kompletten ersten Einkauf (mit Optokoppler-Variante
+ LED-Spannungsteiler + ATX-Anzapfkabel) — die meisten Positionen sind
Packungen/Sortimente, davon wird nur ein Bruchteil für dieses Projekt
gebraucht, der Rest ist Reserve für spätere Projekte (analog zur
Sensormeter-Familie).

## Noch offen / zu klären

- **DHT11-Preis nur geschätzt** (~5 €, AZDelivery-Niveau) - noch nicht
  wie beim Hauptboard einzeln verifiziert.
- **Spannungsteiler-Dimensionierung NTC**: 10 kΩ Festwiderstand als
  Standardwert angenommen (symmetrischer Teiler bei 10 kΩ NTC-Nennwert) -
  exakte Dimensionierung hängt vom gewünschten Messbereich/der
  ADC-Auflösung ab, noch nicht durchgerechnet.
- ~~LED-Eingänge (Power-LED, HDD-LED) brauchen laut aktueller
  Projektbeschreibung keine zusätzlichen Bauteile~~ **korrigiert
  2026-07-18**: Spannungsteiler ergänzt (#6b) - die ursprüngliche Annahme
  „kein Bauteil" war nur für einen sicher 3,3-V-referenzierten Header
  gültig, viele Mainboards ziehen ihn aber auf 5 V hoch.
- ~~Power-/Reset-Taster-Status auslesen~~ **geklärt**: eigene, von der
  Optokoppler-Weiterleitung komplett getrennte Eingangs-Pins (Taster
  direkt auf ESP, gegen GND schaltend). Kein gemeinsamer Knoten mit der
  Mainboard-Ansteuerung, daher auch kein Entkopplungsbedarf auf dieser
  Seite - siehe `Projektbeschreibung.txt` „Ablauf Tastendruck".
- Kein PCB in dieser Liste - reine Prototyp-Stückliste.
- **USB-Port-Zuordnung (welcher der zwei USB-C-Anschlüsse ist Flash/UART
  vs. natives USB) nur mit mittlerem Vertrauen aus Fotos bestimmt** -
  siehe `docs/verdrahtungsplan.html` Abschnitt 1, vor dem ersten Einsatz
  am echten Board verifizieren.
