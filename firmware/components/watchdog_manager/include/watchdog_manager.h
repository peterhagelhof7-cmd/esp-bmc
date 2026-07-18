#pragma once

// WatchdogManager - Onboard-RGB-LED (WS2812, GPIO48, siehe "board mit
// bezeichner.bmp") als Lebenszeichen-Anzeige fuer die ESP-eigene
// Firmware/FreeRTOS (NICHT fuer das Host-Betriebssystem - das war ein
// frueherer, verworfener Ansatz). Details: docs/entscheidungen.md
// "Watchdog-LED (RGB, GPIO48)".

void watchdog_manager_init(void);
