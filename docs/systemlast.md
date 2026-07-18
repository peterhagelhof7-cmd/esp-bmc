# Systemlast (CPU, RAM, Flash)

Einordnung gegenüber den Zielwerten aus
[`docs/pflichtenheft.txt`](pflichtenheft.txt) Abschnitt 9 — die dort
formulierten Ziele sind bewusst **qualitativ**, nicht numerisch (anders
als z. B. `sensormeter-poe`s "CPU < 40 % / RAM < 60 %"):

> Webserver non-blocking · Taster-Erfassung mit geringer Latenz
> (Entprellung im ms-Bereich, kein spürbares Delay zwischen physischem
> Tastendruck und Weiterleitung)

**Architektur-Hinweis, wichtig für dieses Dokument:** ESP-BMC ist reines
ESP-IDF (kein Arduino), es gibt **keinen** gemeinsamen `loop()`-Tick wie
bei der Arduino-basierten Sensormeter-Familie. Jede aktiv pollende
Komponente läuft in ihrem **eigenen** FreeRTOS-Task, dazu kommen interne
Systemtasks (WLAN-Treiber, LWIP, Default-Event-Loop, TinyUSB,
HTTP-Server) und ein `app_main`-Haupttask. Eine "Last pro Loop-Tick"-
Rechnung wie bei `sensormeter-poe` ergibt hier keinen Sinn — stattdessen
wird unten pro Task ein Tastverhältnis (Duty-Cycle) abgeschätzt.

Zwei Datenquellen, wie bei den Sensormeter-Geschwisterprojekten
konsequent getrennt:

1. **Gemessen** — reale `pio run`-Build-Ausgabe (Flash/RAM), diese
   Sitzung (2026-07-18) frisch für **beide** PlatformIO-Umgebungen neu
   gebaut, nicht aus `entscheidungen.md`/`projektstand.md` übernommen.
2. **Abgeschätzt** — Tastverhältnis pro Task, hergeleitet aus dem
   tatsächlichen `vTaskDelay`-Takt und bekannten Operationskosten (ADC-
   Read, DHT-Protokoll-Timing, RMT-LED-Refresh). **Keine reale
   Hardware-Profilierung möglich** — ESP-BMC wurde in diesem gesamten
   Projekt noch nie auf echte Hardware geflasht (siehe
   `docs/entscheidungen.md`, Stand 2026-07-18).

## 1. Flash/RAM (gemessen, beide Umgebungen)

Frisch gebaut aus `firmware/` mit `pio run -e <env>`:

```
esp32-s3-devkitc-1-n16r8:
  RAM:   19,6 % (64.224 von 327.680 Byte)
  Flash: 61,8 % (1.295.353 von 2.097.152 Byte App-Partition "ota_0")

esp32-s3-devkitc-1-n8r8:
  RAM:   19,6 % (64.240 von 327.680 Byte)
  Flash: 61,6 % (1.292.085 von 2.097.152 Byte App-Partition "ota_0")
```

Praktisch identisch zwischen beiden Umgebungen (Differenz < 0,3 kB RAM,
< 3,3 kB Flash) — erwartbar, da `board_upload.flash_size` nur die
esptool-Zielgröße ändert, nicht den kompilierten Code. Beide Werte
decken sich mit dem zuletzt in `docs/projektstand.md` dokumentierten
Stand (61,8 % / 19,9 % für n16r8), kleine Abweichung durch
Zwischen-Commits ohne Auswirkung auf die Größenordnung.

**Reserve:** Flash 38,2 % frei (n16r8) bzw. 38,4 % frei (n8r8) auf der
2-MB-App-Partition; RAM 80,4 % frei für Heap/Stack zur Laufzeit — beide
unkritisch. Die tatsächlich verfügbare Flash-Kapazität (16 MB bzw. 8 MB)
spielt für die Partitions-Auslastung selbst keine Rolle, `partitions.csv`
summiert unverändert auf ≈ 5,06 MB, siehe `docs/projektstand.md`
Abschnitt 3.

Build-Hinweis: `pio run -e esp32-s3-devkitc-1-n8r8` meldet
`Warning! Flash memory size mismatch detected. Expected 8MB, found 16MB!`
— esptool vergleicht die konfigurierte Zielgröße mit einer
Toolchain-internen Annahme, keine echte Fehlermeldung, kein Einfluss auf
die oben gemessenen Flash-/RAM-Werte.

## 2. Nebenläufigkeitsmodell (Tasks, real aus dem Quellcode)

Alle `xTaskCreate`/`xTaskCreatePinnedToCore`-Aufrufe in
`firmware/components/` bzw. `firmware/main/main.c` direkt gegriffen:

| Task (Name im Code) | Komponente | Stack | Prio | Kern | Zweck / Takt |
|---|---|---|---|---|---|
| `gpio_manager` | GpioManager | 2048 B | 1 | 0 (pinned) | Taster-/LED-Erfassung, Poll alle 5 ms (`DEBOUNCE_POLL_MS`) |
| `sensor_manager` | SensorManager | 4096 B | 1 | 0 (pinned) | NTC/DHT11-Poll alle 60 s (`SENSOR_TASK_INTERVAL_MS`) |
| `watchdog_led` | WatchdogManager | 3072 B | 2 | beliebig | RGB-Farbverlauf alle 40 ms, füttert zugleich den TWDT |
| `console_pump` | WebServerManager | 3072 B | 1 | beliebig | Pumpt WebSocket-Konsolendaten, Poll alle 50 ms |
| `notify_digest` | NotificationManager | 3072 B | 1 | beliebig | Prüft Sammel-Mail-Fälligkeit alle 30 s (`DIGEST_CHECK_INTERVAL_MS`) |
| `time_manager` | TimeManager | 3072 B | 1 | beliebig | NTP-Resync, wartet auf Link-Up-Semaphore, Timeout 5 h |
| `snmp_manager` | SnmpManager | 4096 B | 4 | beliebig | SNMP-UDP-Server, blockierend auf `recvfrom` |
| `ssh_manager` | SshManager | 8192 B | 4 | beliebig | SSH-Server (wolfSSH), blockierend auf `accept`/`recv` |
| *(intern, `esp_http_server`)* | WebServerManager | 24576 B (explizit erhöht, siehe unten) | HTTPD-Default | beliebig | HTTP-Request-Handler, blockierend auf `select` |
| `TinyUSB` *(intern, `esp_tinyusb`)* | UsbManager | 4096 B (Kconfig-Default) | 5 | 1 (Kconfig-Default bei Dual-Core) | USB-CDC+HID-Polling, per `tinyusb_driver_install()` gestartet, kein eigener `xTaskCreate`-Aufruf in `usb_manager.c` |
| `main` | `app_main` | App-Standard-Stack (Kconfig) | 1 | — | Init-Sequenz, danach 1-s-Statuszeile + `esp_task_wdt_reset()` |
| `wifi`/`tiT`/`sys_evt` *(ESP-IDF-Systemtasks)* | NetworkManager (indirekt) | ESP-IDF-Standardwerte, projektunabhängig | System | System | WLAN-Treiber, LWIP, Default-Event-Loop (`esp_event_loop_create_default()` in `network_manager.c`) |

**Korrektur der Annahme "ein Task pro Manager":** Trifft nur auf die
aktiv pollenden Komponenten zu. Folgende Komponenten haben **keinen**
eigenen Task und laufen synchron im Kontext des jeweiligen Aufrufers
(meist der `httpd`- oder `ssh_manager`/`usb_manager`-Task, aus dem heraus
ihre Funktionen aufgerufen werden):

- `OtaManager` — Upload-Handling läuft im `httpd`-Request-Handler
- `UserManager`, `ConfigManager`/`StorageManager`, `AuditLog`,
  `SensorHistory`, `FirmwareVersion` — reine Funktionsbibliotheken,
  aufgerufen aus `app_main`-Init bzw. aus Web-/USB-/SSH-Handlern
- `WireguardManager` — **kein eigener Task und kein `esp_timer`** in der
  eingebundenen `droscy/esp_wireguard`-Bibliothek gefunden
  (`.pio/libdeps/*/esp_wireguard/` durchsucht, keine Treffer für
  `xTaskCreate`/`esp_timer_create`); `esp_wireguard_connect()` wird
  einmalig aus `app_main()` aufgerufen. Deckt sich mit dem in
  `docs/projektstand.md` offen gelisteten Punkt "Aktive
  WireGuard-Tunnel-Überwachung/Reconnect ... bislang nur die passive
  Status-Abfrage" — es gibt aktuell keine periodische
  Keepalive-/Reconnect-Task auf ESP-BMC-Seite.

## 3. CPU-Tastverhältnis pro Task (abgeschätzt)

Keine reale Profilierung (kein Board vorhanden) — Schätzung aus
Poll-Intervall × bekannter Operationskosten:

| Task | Kosten pro Durchlauf | Takt | Ø-Tastverhältnis |
|---|---|---|---|
| `gpio_manager` | < 0,1 ms (reine GPIO-Register-Reads) | alle 5 ms | < 2 % |
| `sensor_manager` | ADC-Read < 1 ms + DHT11-Protokoll ~20–25 ms (blockierend, Bit-Timing) | alle 60 s | ~0,04 % |
| `watchdog_led` | < 0,5 ms (HSV-Berechnung + RMT-Refresh für 1 LED, DMA-/Peripherie-getrieben) | alle 40 ms | < 1,3 % |
| `console_pump` | Queue-Poll, ~0 ms wenn keine WebSocket-Konsolensitzung offen | alle 50 ms | vernachlässigbar im Leerlauf |
| `notify_digest` | Reiner Zeitvergleich, kein I/O im Regelfall | alle 30 s | vernachlässigbar |
| `time_manager` | Blockiert auf Semaphore, NTP-Sync nur bei Link-Up/alle 5 h | selten | vernachlässigbar |
| `snmp_manager`/`ssh_manager` | Blockieren auf Socket-I/O, Arbeit nur pro eingehender Anfrage | ereignisgesteuert | vernachlässigbar im Leerlauf |
| `httpd` (intern) | Seitenaufbau bis ~12 KB Puffer (`settings_get_handler`, siehe `entscheidungen.md`-Nachtrag zum Stack-Fix) | ereignisgesteuert, nutzergetrieben | vernachlässigbar im Leerlauf, spürbar nur bei aktivem Seitenaufruf |
| `TinyUSB` (intern) | USB-SOF-getriebenes Polling, laut Treiber-Design für Leerlauf optimiert | kontinuierlich, aber interrupt-/queue-gestützt | gering, nicht quantifiziert (Closed-Source-Treiberverhalten nicht im Detail geprüft) |
| `main` | 1× `ESP_LOGI` + zwei Funktionsaufrufe | alle 1 s | vernachlässigbar |

**Höchste Grundlast im Dauerbetrieb:** `gpio_manager` mit 5-ms-Poll
(< 2 % geschätzt) — bewusst so eng getaktet, um die geforderte
"geringe Latenz" der Taster-Erfassung zu erfüllen (Pflichtenheft
Abschnitt 9). Alle anderen Tasks liegen weit darunter oder sind
ereignisgesteuert. Ein numerischer Gesamt-CPU-Wert (wie
`sensormeter-poe`s "~4–5 % Worst Case") lässt sich für dieses
Multi-Task-Modell nicht seriös als eine einzelne Zahl angeben, ohne
eine echte Trace-Messung (z. B. FreeRTOS-Runtime-Stats) auf realer
Hardware — das ist bewusst offen gelassen, statt eine erfundene
Gesamtsumme zu präsentieren.

## Fazit

| Zielwert (Pflichtenheft 9) | Rechnerischer/gemessener Stand |
|---|---|
| Webserver non-blocking | Teilweise eingeordnet: der `httpd`-Task selbst läuft unabhängig von allen anderen Tasks (blockiert niemanden sonst), **aber** einzelne Handler (`settings_get_handler`, `root_get_handler`) bauen ihre Antwort synchron in großen Stack-Puffern auf (bis ~12 KB) — das hatte bereits einen realen Stack-Overflow-Bug zur Folge (behoben durch `stack_size = 24576`, siehe `entscheidungen.md`). "Non-blocking" bezieht sich hier auf *andere Tasks*, nicht auf eine asynchrone Handler-Implementierung. |
| Taster-Erfassung geringe Latenz (ms-Bereich) | Erfüllt (rechnerisch): 5-ms-Poll-Intervall in `gpio_manager`, deutlich innerhalb "kein spürbares Delay" |
| RAM/Flash-Reserve | Deutlich unkritisch: 19,6 % RAM, ~62 % Flash auf der 2-MB-App-Partition (beide Umgebungen, frisch gemessen) |

Kein Hardware-Bringup dieses Projekts bisher erfolgt — alle Angaben in
diesem Dokument sind entweder Build-Artefakt-Messungen (Flash/RAM, real)
oder Quellcode-basierte Schätzungen (CPU-Tastverhältnis, **nicht**
gemessen). Eine echte Laufzeit-Lastmessung (z. B. `vTaskGetRunTimeStats()`
über die USB- oder SSH-Konsole) ist als sinnvoller erster Schritt nach
dem ersten Flash vorgemerkt, aber nicht Teil dieses Dokuments.
