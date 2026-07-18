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

// true, wenn eine Host-Anwendung den CDC-Port aktiv geoeffnet hat (DTR
// gesetzt) - Entscheidungsgrundlage fuer CDC- vs. HID-Fallback.
bool usb_manager_cdc_host_ready(void);

// Sendet Daten auf dem CDC-Kanal (z.B. spaeter aus der WebSocket-Konsole,
// P5). Nicht blockierend - puffert intern, siehe TinyUSB-CDC-Queue.
void usb_manager_cdc_write(const uint8_t* data, size_t len);

// Ueber CDC empfangene Rohbytes landen hier (Pflichtenheft: "Bidirektionale
// Weiterleitung CDC <-> interne Konsolen-Queue") - P5 (WebServerManager)
// liest daraus, sobald die WebSocket-Konsole existiert. Elemente sind
// einzelne Bytes (uint8_t), bewusst simpel gehalten, bis P5 den tatsaechlichen
// Konsolen-Rahmen definiert.
QueueHandle_t usb_manager_get_cdc_rx_queue(void);

// HID-Tastatur-Fallback: sendet einen einzelnen Tastendruck (Press+Release)
// mit optionalem Modifier (siehe TinyUSB hid.h HID_KEYBOARD_MODIFIER_*).
void usb_manager_send_key(uint8_t modifier, uint8_t keycode);

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

void usb_manager_console_claim(console_owner_t owner);
// Setzt den Besitzer nur zurueck, wenn "owner" gerade tatsaechlich haelt -
// verhindert, dass eine beendete Sitzung versehentlich eine inzwischen
// neu uebernommene Sitzung eines anderen Kanals wieder freigibt.
void usb_manager_console_release(console_owner_t owner);
console_owner_t usb_manager_console_owner(void);
