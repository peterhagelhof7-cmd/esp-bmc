#pragma once

#include <stdbool.h>

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
// GPIO-Nummern unten sind AUSDRUECKLICH PROVISORISCH, nur fuer die
// Wokwi-Simulation ohne echtes Board (siehe docs/pflichtenheft.txt
// Abschnitt 12, "Exakte GPIO-Pinbelegung ... noch offen"). Vor dem ersten
// echten Flash auf das reale diymore-Board pruefen/anpassen.

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

// LED-Ansteuerung (Lastenheft 10.1).
void gpio_manager_set_power_led(bool on);
void gpio_manager_set_hdd_led(bool on);

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
