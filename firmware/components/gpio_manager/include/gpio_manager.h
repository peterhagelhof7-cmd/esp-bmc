#pragma once

#include <stdbool.h>
#include <stdint.h>

// GpioManager - Taster-Erfassung, Taster-Weiterleitung (ueber Optokoppler),
// LED-Erfassung, LED-Ansteuerung. Setzt den Ablauf aus
// docs/lastenheft.txt Abschnitt 10.4 direkt um:
//
//   1. Taster wird gedrueckt -> zieht den zugehoerigen ESP-Eingangs-Pin auf LOW
//   2. Firmware prueft die Tastschutz-Bedingung aus dem Webinterface
//   3. Ist kein Tastschutz aktiv: ESP setzt den zugehoerigen Ausgangs-Pin
//      (ueber den Optokoppler an den Mainboard-Header) auf LOW und leitet
//      den Tastendruck damit weiter
//
// GPIO-Nummern final festgelegt (2026-07-18) gegen die Vendor-Pinout-
// Bilder ("board mit bezeichner.bmp", "board.jpg", "docs/board-foto.jpg")
// und die ESP-IDF-SoC-Header abgeglichen (keine Kollision mit
// Strapping-Pins GPIO0/3/45/46, JTAG GPIO39-42, UART GPIO43/44 oder
// nativem USB GPIO19/20) - siehe docs/verdrahtungsplan.html fuer die
// vollstaendige Pinbelegung inkl. Beschaltung (Optokoppler-Varianten,
// Spannungsteiler fuer die LED-Erfassung) und docs/entscheidungen.md.
// Noch nicht auf echter Hardware verifiziert (kein Board vorhanden), aber
// kein reiner Wokwi-Platzhalter mehr.
//
// Alle acht Pins liegen auf der linken Pinleiste (siehe
// docs/verdrahtungsplan.html) - seit 2026-07-20 gilt das auch fuer
// sensor_manager.h (NTC/DHT dorthin verschoben), damit eine
// Huckepack-Platine mit nur einer Stiftleiste auskommt.

#define GPIO_REMOTE_POWER_SENSE   4   // Eingang, Pull-up (Lastenheft 10.2)
#define GPIO_REMOTE_RESET_SENSE   5   // Eingang, Pull-up (Lastenheft 10.2)
#define GPIO_REMOTE_POWER_DRIVE   6   // Ausgang -> PC817 -> Mainboard (10.3)
#define GPIO_REMOTE_RESET_DRIVE   7   // Ausgang -> PC817 -> Mainboard (10.3)
#define GPIO_REMOTE_POWER_LED_IN  15  // Eingang, direkt (Lastenheft 10.1)
#define GPIO_REMOTE_HDD_LED_IN    16  // Eingang, direkt (Lastenheft 10.1)
#define GPIO_REMOTE_POWER_LED_OUT 17  // Ausgang, direkt (Lastenheft 10.1)
#define GPIO_REMOTE_HDD_LED_OUT   18  // Ausgang, direkt (Lastenheft 10.1)

// Konfiguriert alle acht Gehaeuse-Pins und startet die GpioTask
// (Entprellung + Ablauf aus 10.4). Einmalig aus app_main() aufrufen.
void gpio_manager_init(void);

// Aktueller (entprellter) Erfassungs-Zustand - true = Taster gerade gedrueckt.
bool gpio_manager_power_taste_gedrueckt(void);
bool gpio_manager_reset_taste_gedrueckt(void);

// Aktueller Weiterleitungs-Zustand - true = Ausgang aktiv (Optokoppler
// durchgeschaltet), unabhaengig davon ob durch Tastschutz unterdrueckt.
bool gpio_manager_power_taste_weitergeleitet(void);
bool gpio_manager_reset_taste_weitergeleitet(void);

// LED-Erfassung (Lastenheft 10.1) - roher Pin-Zustand, keine Entprellung
// noetig fuer eine Statusanzeige.
bool gpio_manager_read_power_led(void);
bool gpio_manager_read_hdd_led(void);

// true, wenn in den letzten 10 Sekunden mindestens einmal ein Signal am
// HDD-LED-Eingang anlag (webconfig.txt Uebersichtsseite: "eine Rote
// Flaeche die blinkt, wenn im Verlauf der letzten 10 sec ein Signal am
// Eingang anlag") - im Gegensatz zu gpio_manager_read_hdd_led() (nur der
// aktuelle Momentan-Pegel) faengt das auch kurze, zwischen zwei
// Seitenaufrufen liegende Blink-Impulse ein.
bool gpio_manager_hdd_led_active_recently(void);

// Host-Uptime, abgeleitet aus der Power-LED-Erfassung: laeuft, seit die
// Power-LED zuletzt von "aus" auf "an" gewechselt ist (nicht die
// ESP-eigene Boot-Uptime!). Liefert false, solange die LED aktuell nicht
// als aktiv erkannt wird (Web-UI zeigt dann "Host AUS oder nicht erkannt"
// statt einer Laufzeit) - true + Sekunden seit diesem Wechsel, sonst.
bool gpio_manager_host_uptime_seconds(int64_t* out_seconds);

// LED-Ansteuerung (Lastenheft Abschnitt 5 "Gehaeuse-Power-/HDD-LED
// ansteuern", elektrisch siehe 10.1).
void gpio_manager_set_power_led(bool on);
void gpio_manager_set_hdd_led(bool on);

// Zuletzt per gpio_manager_set_*_led() gesetzter Zustand (Software-
// Schattenkopie, siehe Kommentar bei TasterKanal.weitergeleitet in der
// .c-Datei - kein zuverlaessiges GPIO-Readback auf einem OUTPUT-Pin).
// Fuer die Web-/USB-Anzeige des aktuellen Soll-Zustands.
bool gpio_manager_power_led_out_state(void);
bool gpio_manager_hdd_led_out_state(void);

// --- Taster-Steuerung per Software (webconfig.txt "steuerung der taster
// (power und reset)", was-loggen.txt "user set power host (push or hold)" /
// "user set reset host") ---
//
// Wirkt genau wie ein physischer Tastendruck: leitet fuer die jeweilige
// Dauer ueber denselben Optokoppler-Ausgang weiter wie
// taster_kanal_poll() - und respektiert deshalb auch denselben
// Tastschutz. Liefert false, wenn Tastschutz aktiv ist (dann passiert
// nichts).
//
// hold=false -> kurzer Tastendruck (Soft-Power-On/-Off, ~300ms),
// hold=true -> langer Tastendruck (erzwungenes Abschalten, ~5s) -
// uebliche PC-Konvention. Reset kennt kein "hold", nur einen kurzen Puls.
bool gpio_manager_trigger_power(bool hold);
bool gpio_manager_trigger_reset(void);
