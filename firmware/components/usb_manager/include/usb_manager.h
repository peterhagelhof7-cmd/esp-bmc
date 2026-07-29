#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// UsbManager (docs/pflichtenheft.txt Abschnitt 3.7/6) - TinyUSB-Composite-
// Device (CDC + HID-Tastatur) ueber den nativen USB-Port des ESP32-S3. Der
// zweite USB-C-Port (UART-Bridge) bleibt fuer Flashen/Debuggen reserviert,
// unabhaengig von dieser Komponente.
//
// CDC ist der Hauptkanal fuer die bidirektionale Konsolen-Kommunikation mit
// dem gesteuerten PC; HID (Tastatur) ist der Fallback-Kanal fuer Eingaben,
// wenn auf dem PC (noch) keine Software auf dem CDC-Kanal wartet (BIOS-/
// Bootloader-Phase, siehe Abschnitt 6). Ob der PC-seitig eine Anwendung den
// CDC-Port geoeffnet hat, wird ueber die DTR-Leitung erkannt
// (usb_manager_cdc_host_ready()) - die eigentliche Entscheidung, wann auf
// HID umgeschaltet wird, trifft der Aufrufer (WebServerManager/P5), diese
// Komponente liefert nur das Signal.

void usb_manager_init(void);

// Historisch fuer die CDC-vs-HID-Entscheidung (DTR). Der USJ-Transport liefert
// keinen DTR-Zustand und HID entfiel - liefert jetzt konstant true.
bool usb_manager_cdc_host_ready(void);

// Sendet Daten auf dem Konsolenkanal zum gesteuerten PC (aus Web-/SSH-Konsole).
// Nicht blockierend - kurzes TX-Timeout, verwirft bei fehlendem Host.
void usb_manager_cdc_write(const uint8_t* data, size_t len);

// Ueber CDC empfangene Rohbytes landen hier (Pflichtenheft: "Bidirektionale
// Weiterleitung CDC <-> interne Konsolen-Queue") - P5 (WebServerManager)
// liest daraus, sobald die WebSocket-Konsole existiert. Elemente sind
// einzelne Bytes (uint8_t), bewusst simpel gehalten, bis P5 den tatsaechlichen
// Konsolen-Rahmen definiert.
QueueHandle_t usb_manager_get_cdc_rx_queue(void);

// --- Konsolen-Besitz (P7: SSH-Server bruecckt auf denselben CDC-Kanal wie
// die WebSocket-Konsole, siehe docs/entscheidungen.md "SSH-Server (P7)") ---
//
// Genau EIN Verbraucher darf die CDC-RX-Queue gleichzeitig leeren (Web
// ODER SSH, nicht beide) - sonst wuerde jedes eingehende Byte
// zufaellig an den einen oder anderen Konsumenten gehen (FreeRTOS-Queue
// liefert jedes Element nur an EINEN Empfaenger). "Claim" wirkt wie im
// bisherigen Web-Konsolen-Verhalten nach dem Prinzip "der Letzte
// gewinnt" - kein Verbindungsabbau-Tracking, bewusst so simpel wie das
// bereits bestehende s_ws_console_fd-Muster.
typedef enum { CONSOLE_OWNER_NONE, CONSOLE_OWNER_WEB, CONSOLE_OWNER_SSH } console_owner_t;

// Uebernimmt die (einzige) Konsole fuer "owner" und liefert eine eindeutige,
// monoton steigende Generation zurueck. Jeder claim VERDRAENGT den bisherigen
// Besitzer (Takeover): die vorige Sitzung erkennt den Verlust ueber
// usb_manager_console_is_current(gen) == false und beendet sich selbst.
uint32_t usb_manager_console_claim(console_owner_t owner);

// true, solange die zu "gen" gehoerende Sitzung noch die aktive Konsole ist
// (kein spaeterer claim hat uebernommen und der Slot ist nicht freigegeben).
bool usb_manager_console_is_current(uint32_t gen);

// Gibt die Konsole frei - aber nur, wenn "gen" noch die aktuelle Generation
// ist. Eine bereits verdraengte (veraltete) Sitzung raeumt so nicht dem
// inzwischen aktiven Nachfolger den Slot weg.
void usb_manager_console_release(uint32_t gen);

console_owner_t usb_manager_console_owner(void);

// Baut einen kurzen Begruessungs-/Status-Text (Uptime, Temperaturen,
// Tastschutz/Freigabe der Taster, ...) fuer die serielle Konsole zusammen.
// Zeilen mit CRLF terminiert (SSH-PTY). Schreibt hoechstens len-1 Bytes.
void usb_manager_build_status_banner(char* buf, size_t len);
