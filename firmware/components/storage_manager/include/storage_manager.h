#pragma once

#include <stdbool.h>
#include <stddef.h>

// StorageManager - mountet die "storage"-Partition (LittleFS, siehe
// firmware/partitions.csv und docs/entscheidungen.md) unter /storage.
//
// Bewusst nur Lese-/Schreib-Grundfunktionen ohne Vorgabe fuer
// Dateiformat/-namen - welche Dateien genau abgelegt werden (Config-Format,
// Log-Rotation, wireguard.conf, SSH-Host-Key) ist noch offen (Pflichtenheft
// Abschnitt 12) und folgt als eigener Schritt. Dieses Modul ist die
// gemeinsame Grundlage, die spaeter sowohl UsbManager (P4, CDC-Kommando)
// als auch WebServerManager (P5, REST/WebSocket-Endpunkt) zum Auslesen des
// Inhalts nutzen sollen, ohne dass beide getrennt eigenen
// Dateisystem-Zugriffscode brauchen.

// Mountet die Partition (formatiert automatisch neu, falls das Mounten
// fehlschlaegt - z.B. beim allerersten Boot mit leerer Partition). Einmalig
// aus app_main() aufrufen.
void storage_manager_init(void);

bool storage_manager_is_mounted(void);

// Basisverzeichnis, unter dem die Partition gemountet ist ("/storage") -
// Aufrufer haengen ihre Dateinamen selbst an (z.B. "/storage/config.json").
const char* storage_manager_base_path(void);

// Belegter/Gesamtspeicher in Byte, fuer Diagnose-/Status-Ausgabe. false,
// wenn die Partition nicht gemountet ist.
bool storage_manager_get_usage(size_t* out_used_bytes, size_t* out_total_bytes);
