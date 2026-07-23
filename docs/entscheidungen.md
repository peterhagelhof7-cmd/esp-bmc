# ESP-BMC (vormals ESP-Remote) — Entscheidungsprotokoll

Append-only: neue Erkenntnisse werden als neuer Eintrag ergänzt, bestehende
Einträge werden nicht rückwirkend verändert oder gelöscht (analog zur
Sensormeter-Familie).

## 2026-07-17 — P3 Sensorik: SensorManager (NTC + DHT11) gebaut, in Wokwi verifiziert

Umsetzung von Pflichtenheft Abschnitt 3.2/2.1 (SensorManager/SensorTask,
60s-Intervall). Neue Komponenten `sensor_manager` und `notification_manager`,
Schwellwerte (NTC-Temperatur, DHT11-Temperatur, DHT11-Luftfeuchte) als
RAM-only Getter/Setter in `config_manager` ergänzt (gleiches
Platzstand-Muster wie Tastschutz - Persistenz folgt erst mit der
LittleFS-Config-Phase).

### NTC-Auswertung: Beta-Formel statt vollem Steinhart-Hart

Spannungsteiler 3V3 -> Festwiderstand (10 kΩ, `docs/bom.md`) -> ADC-Pin ->
NTC 10K B3590 -> GND. Umrechnung ueber die vereinfachte Beta-Formel
(`1/T = 1/T0 + (1/B)*ln(Rntc/R0)`, B=3590 laut Typenbezeichnung) statt der
vollen Steinhart-Hart-Gleichung mit drei Koeffizienten - fuer den in
Abschnitt 8.3 geforderten reinen Schwellwert-Vergleich reicht die
Beta-Naeherung, die zusaetzliche Genauigkeit der Drei-Koeffizienten-Form
waere hier unbegruendeter Mehraufwand. ADC-Kalibrierung ueber
`adc_cali_create_scheme_curve_fitting` (ESP32-S3 unterstuetzt dieses Schema
durchgehend, kein Fallback-Zweig noetig) - ohne Kalibrierung waeren die
mV-Werte fuer die Spannungsteiler-Rechnung zu ungenau.

### DHT11: eigener Bit-Bang-Treiber statt Bibliothek

ESP-IDF bringt keinen DHT-Treiber mit; das 1-Wire-artige Protokoll (Host-
Startsignal, 40 Bit ueber Puls-Timing, additive Pruefsumme) ist einfach
genug fuer eine eigene ~50-Zeilen-Implementierung ohne externe Abhaengigkeit
(anders als bei WireGuard, siehe P1-Eintrag oben, wo eine Fremdbibliothek
klar guenstiger war). Zeitkritischer Abschnitt (Bit-Fenster ~70-120us) laeuft
in einer FreeRTOS-Critical-Section (`taskENTER_CRITICAL`/`taskEXIT_CRITICAL`),
damit der Tick-Interrupt das Timing nicht verfaelscht.

### Wokwi-Simulationsluecke: kein DHT11-Part, nur DHT22 - Datenformat-Switch noetig

Wokwi stellt keinen eigenen `wokwi-dht11`-Part bereit, nur `wokwi-dht22`.
Beide nutzen dasselbe Bit-/Pruefsummen-Protokoll, aber unterschiedliche
Dateninterpretation: DHT11 = Ganzzahl-Byte, DHT22 = 0.1-Aufloesung mit
Vorzeichenbit. Neue Kconfig-Option
`ESP_REMOTE_SENSOR_DHT_IS_DHT22_FORMAT` (Default `n`) schaltet die Dekodierung
um - **nur** in `sdkconfig.wokwi-sim` auf `y` gesetzt (gleiches Muster wie
`CONFIG_SPIRAM` fuer die PSRAM-Deaktivierung, siehe
docs/entscheidungen.md-Eintrag zu den Wokwi-Simulationsluecken bzw.
Projekt-Memory `esp-remote-wokwi-simulation-gotchas`). Ohne diesen Switch
haette die Simulation mit dem DHT11-Ganzzahl-Dekoder aus einem simulierten
40.0 %/24.0 °C-Wert (DHT22-Rohbytes `0x01 0x90 0x00 0xF0`) einen falschen
Wert (1 % / 0 °C) gelesen - waere aber NICHT als Fehler aufgefallen (nur
plausibel-aber-falsch), daher explizit hier festgehalten.

NTC-Simulation: Wokwi hat keinen Thermistor-Part, ein `wokwi-potentiometer`
dient als Spannungsteiler-Platzhalter (liefert eine variable Analogspannung
am ADC-Pin, keine echte NTC-Kennlinie) - reicht aus, um den ADC-Lese- und
Umrechnungspfad zu verifizieren, nicht um konkrete Temperaturwerte gegen
echtes NTC-Verhalten zu pruefen.

### Verifikation (Wokwi CLI, `wokwi-sim`-Env, 85s Simulationszeit)

Beide PlatformIO-Envs bauen fehlerfrei (echtes Board: 788297 B Flash =
75,2 % der 1-MB-App-Partition, +16588 B ggue. dem P1-Spike-Stand;
wokwi-sim: 766109 B = 73,1 %). Simulationslauf zeigt zwei SensorTask-Zyklen
im erwarteten 60s-Abstand (t=127ms und t=60147ms), beide mit identischen,
plausiblen Werten (NTC 17.7 °C vom Potentiometer-Platzhalter; DHT11 24.0 °C/
40.0 %, exakt deckungsgleich mit den in `diagram.json` gesetzten
Wokwi-Part-Attributen - bestaetigt Pruefsummen-, Timing- und
Dekodierlogik). Echte Plausibilitaets-/Schwellwert-Grenzfaelle (Abschnitt
8.3, Sensorausfall) sind mangels Hardware/gezielter Fehlersimulation noch
nicht getestet.

**Naechster offener Punkt aus P1** (eigene `partitions.csv`, siehe
Abschnitt 10/12) bleibt unveraendert bestehen und wird mit dem P3-Zuwachs
(+1,6 Prozentpunkte Flash) etwas dringlicher.

## 2026-07-17 — Eigene Partitionstabelle angelegt, StorageManager (LittleFS) gebaut

Loest den in P1/P3 aufgeschobenen Punkt: eigene `firmware/partitions.csv`
ersetzt das PlatformIO-Standardpreset `partitions_two_ota.csv` (1 MB je
OTA-Slot, dort zuletzt schon bei 75,2% Auslastung).

### Partitionslayout

| Name     | Type | SubType | Offset    | Groesse | Zweck |
|----------|------|---------|-----------|---------|-------|
| nvs      | data | nvs     | 0x9000    | 20 K    | IDF-interne WLAN-Kalibrierungsdaten |
| otadata  | data | ota     | 0xE000    | 8 K     | OTA-Slot-Auswahl (Groesse von ESP-IDF fest vorgegeben) |
| ota_0    | app  | ota_0   | 0x10000   | 2 M     | Firmware-Slot A |
| ota_1    | app  | ota_1   | 0x210000  | 2 M     | Firmware-Slot B |
| storage  | data | spiffs  | 0x410000  | 1 M     | LittleFS: Einstellungen, Logs, `wireguard.conf`, SSH-Host-Key |

`nvs` (20 K) + `otadata` (8 K, fix) ergeben zusammen exakt 0x7000 - ab
Start 0x9000 landet `ota_0` dadurch exakt auf der von App-Partitionen
geforderten 64-KB-Grenze (0x10000), ohne die sonst noetige Aufrundung auf
0x20000 (~60 KB verschenktes Padding). Gleiches Prinzip wie im
ESP-IDF-Stock-Preset, nur ohne `phy_init`-Partition (nicht noetig,
`CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` ist aus). `storage` nutzt
`type=data,subtype=spiffs`, obwohl LittleFS gemountet wird - kein
natives `littlefs`-Subtype in ESP-IDFs Partitionswerkzeug, `spiffs` ist
dafuer die uebliche Konvention (auch in der `esp_littlefs`-Doku so
empfohlen); das Mounten laeuft ueber den Partitionsnamen, nicht den
Subtype.

Bewusst **eine** Partition fuer Config+Logs+WireGuard-Config+SSH-Key
(User-Entscheidung) statt getrennter Partitionen - Log-Rotation/-Limit
muss trotzdem in Software erzwungen werden, die Partitionsgroesse allein
schuetzt nicht vor einer vollgeschriebenen Partition.

### Build-Einbindung: PlatformIO ignoriert dafuer sdkconfig, braucht `board_build.partitions`

Reines Setzen von `CONFIG_PARTITION_TABLE_CUSTOM`/`_FILENAME` in der
sdkconfig (das fuer andere Einstellungen in diesem Projekt uebliche
Vorgehen, siehe WLAN-DHT-Format-Switch) **wirkte hier still nicht** -
PlatformIOs espidf-Builder setzt bei `framework = espidf` intern einen
eigenen Default-Partitionspfad, der die sdkconfig-Werte ueberschreibt,
ohne Fehlermeldung (die eingebettete Tabelle blieb beim PlatformIO-Default
haengen, per `gen_esp32part.py` auf der gebauten `partitions.bin`
verifiziert). Fix: zusaetzlich `board_build.partitions = partitions.csv`
in `platformio.ini` setzen - das ist der von PlatformIO dokumentierte,
eigentlich vorgesehene Weg fuer eine eigene Partitionstabelle unter
espidf. Nach dem Fix per `gen_esp32part.py` gegen die gebaute
`partitions.bin` verifiziert: Tabelle stimmt exakt. Flash-Anzeige beim
Bauen jetzt korrekt gegen 2 MB (37,6%/36,5%) statt vorher irrefuehrend
gegen 1 MB.

### esp_littlefs (joltwallet/littlefs) ueber ESP-IDF Component Manager eingebunden

Anders als bei WireGuard (siehe P1-Eintrag, `lib_deps`-Workaround noetig,
da keine `idf_component.yml`) bringt `joltwallet/littlefs` ein echtes
`idf_component.yml` mit - normale ESP-IDF-Managed-Component-Einbindung
ueber ein eigenes `components/storage_manager/idf_component.yml`
(`joltwallet/littlefs: "^1.6.0"`), aufgeloest auf Version 1.22.2 (hat
bereits dedizierten ESP-IDF-6-Support, inkl. der neueren
Blockdevice-Abstraktion `esp_blockdev`). Eine Falle dabei: der
CMake-Komponentenname fuer `REQUIRES` ist NICHT "esp_littlefs" (der
GitHub-Repo-/Ordnername), sondern `joltwallet__littlefs` (Namespace-Praefix
aus dem `managed_components/`-Verzeichnisnamen) - "Failed to resolve
component 'esp_littlefs': unknown name" beim ersten Versuch, korrigiert
nach Pruefung von `managed_components/joltwallet__littlefs/CMakeLists.txt`.
Baut fehlerfrei auf beiden Envs (echtes Board: 827937 B = 39,5% von 2 MB,
+39640 B ggue. dem P3-Stand ohne LittleFS).

### Wokwi-Grenze gefunden: `board-esp32-s3-devkitc-1`-Part ignoriert die Partitionstabelle komplett zur Laufzeit

Beim Verifikationslauf schlug das Mounten in Wokwi fehl
(`esp_littlefs: partition "storage" could not be found`). Diagnose per
`esp_partition_find(TYPE_ANY, SUBTYPE_ANY, NULL)`-Iteration direkt in
`storage_manager_init()` zeigte: Wokwi liefert zur Laufzeit ein komplett
**eigenes, fest einprogrammiertes Standard-Partitionslayout**
(`nvs`@0x9000/24K, `phy_init`@0xf000/4K, `factory`@0x10000/1.5625M,
`vfs`@0x200000/1.9375M, `coredump`@0x3f0000/64K) - unsere komplette
`partitions.csv` (inkl. `otadata`, `ota_0`, `ota_1`, `storage`) taucht
darin **gar nicht auf**, unabhaengig davon wie sie geschrieben ist. Dass
`nvs`/WLAN in fruaeheren P0-P3-Laeufen anstandslos funktionierte, war
Zufall - Wokwis eigene Default-Tabelle hat selbst eine `nvs`-Partition
(nur mit anderer Groesse, 24K statt unserer 20K), nicht weil unsere
Tabelle gelesen wurde. Das erklaert auch, warum die Wokwi-Doku ("einfach
eine partitions.csv hinzufuegen") hier nicht griff: die
"undocumented"/"unsupported" `board-esp32-s3-devkitc-1`-Part-Variante
(die CLI markiert das selbst als Warnung, siehe
[[esp-remote-wokwi-simulation-gotchas]]) unterstuetzt offenbar keine
eigene Partitionstabelle zur Laufzeit, unabhaengig von
`board_build.partitions`/eingebetteter `partitions.bin`.

**Konsequenz**: StorageManager (LittleFS-Mount) ist in Wokwi grundsaetzlich
nicht verifizierbar, unabhaengig vom Code - der Mount-Fehlschlag dort ist
erwartet und kein Bug. Der Code selbst folgt der Standard-`esp_littlefs`-API
korrekt (durch die `esp_partition_find`-Diagnose bestaetigt: die Anfrage
war korrekt, die Partition existiert in Wokwis Sicht schlicht nicht) und
degradiert sauber (Mount-Fehlschlag wird geloggt, Rueckgabewert
`storage_manager_is_mounted() == false`, restliches System laeuft normal
weiter). Echte Verifikation folgt erst mit realer Hardware.

### Naechster Schritt (bewusst noch nicht umgesetzt)

Format/Inhalt der Dateien auf `storage` (Config-Format, Log-Rotation,
`wireguard.conf`-Ablage, SSH-Host-Key) ist weiterhin offen (Pflichtenheft
Abschnitt 12) und wird spaeter przisiert. StorageManager stellt bewusst
nur die Mount-/Lese-/Schreib-Grundlage bereit, auf die UsbManager (P4)
und WebServerManager (P5) beide zugreifen sollen, um den Inhalt der
Storage-Partition auslesbar zu machen (CDC-Kommando bzw.
REST/WebSocket-Endpunkt) - siehe Pflichtenheft Abschnitt 3.6/3.7/3.9.

## 2026-07-17 — P4 USB Composite-Device (TinyUSB CDC+HID) gebaut

Umsetzung von Pflichtenheft Abschnitt 3.7/6 (UsbManager, CDC-Hauptkanal +
HID-Tastatur-Fallback). Neue Komponente `usb_manager`.

### Bibliothek: `espressif/esp_tinyusb` ueber den ESP-IDF Component Manager

Wie schon bei `esp_littlefs` (siehe P3-Storage-Eintrag oben) ein echtes
`idf_component.yml`, normale Component-Manager-Einbindung ueber
`components/usb_manager/idf_component.yml`. Aufgeloest auf 1.7.6~2 -
Changelog nennt explizit "Added support for IDF 6.0 after removal of the
USB component", also aktiv auf unsere IDF-Version hin gepflegt (anders als
die WireGuard-Bibliothek anfangs, siehe P1-Eintrag). Component-Name fuer
`REQUIRES` ist `espressif__esp_tinyusb` (Namespace-Praefix aus dem
`managed_components/`-Verzeichnis, gleiches Muster wie
`joltwallet__littlefs`).

### Descriptor-Ansatz: Kconfig-generiert statt handgeschrieben

`tinyusb_config_t` komplett auf `{0}` (alle Deskriptor-Zeiger NULL) belassen
- `esp_tinyusb` generiert Geraete-/Konfigurations-/String-Deskriptoren dann
automatisch aus Kconfig (`CONFIG_TINYUSB_CDC_ENABLED`+`_COUNT=1`,
`CONFIG_TINYUSB_HID_COUNT=1`). Erspart einen von Hand zusammengesetzten
Composite-Descriptor (Interface-Nummern/Endpoint-Zuteilung/IAD) - beide
Klassen gleichzeitig werden von `esp_tinyusb` selbst korrekt in einem
Composite-Deskriptor zusammengesetzt, sobald beide Kconfig-Werte gesetzt
sind. Einzige Handarbeit bleibt der HID-Report-Deskriptor selbst (Inhalt
ist immer anwendungsspezifisch, kein generischer Default moeglich) -
`TUD_HID_REPORT_DESC_KEYBOARD()` aus dem TinyUSB-Core-Header
(`class/hid/hid_device.h`) fuer eine Standard-Tastatur, ein Report ohne
Report-ID (nur eine HID-Instanz).

### Drei TinyUSB-HID-Callbacks sind Pflicht, kein Weak-Default

Anders als z.B. `tud_hid_set_protocol_cb`/`tud_hid_report_complete_cb`
(haben `TU_ATTR_WEAK`-Default-Implementierungen in
`managed_components/espressif__tinyusb/src/class/hid/hid_device.c`) haben
`tud_hid_descriptor_report_cb`, `tud_hid_get_report_cb` und
`tud_hid_set_report_cb` **keinen** Weak-Default - ohne eigene
Implementierung schlaegt der Link fehl. Alle drei in `usb_manager.c`
implementiert (Report-Deskriptor liefern; GET_REPORT/SET_REPORT werden
inhaltlich nicht unterstuetzt, nur leer beantwortet/ignoriert).

### CDC-vs-HID-Umschaltung: DTR-Leitung als Signal, Entscheidung bleibt bei P5

Pflichtenheft Abschnitt 6: HID ist Fallback, "wenn der PC (noch) keine
Software auf dem CDC-Kanal bereithaelt". `esp_tinyusb`s
`callback_line_state_changed` liefert genau das (DTR-Zustand) - ob eine
Terminal-/Konsolenanwendung den virtuellen COM-Port geoeffnet hat.
`usb_manager` liefert dieses Signal nur (`usb_manager_cdc_host_ready()`),
trifft die eigentliche Umschalt-Entscheidung aber bewusst nicht selbst -
das ist Aufgabe von P5 (WebServerManager/WebSocket-Konsole), das noch
nicht existiert.

### CDC-Kommando fuer StorageManager-Auslese (minimal, Format bewusst simpel)

Ein Wort ("storage" + Enter) auf dem CDC-Kanal liefert Mount-Status +
Speichernutzung zurueck - analog dem bestehenden Seriell-Kommandomuster
im Sensormeter-Projekt (`dhcp`/`ip`/`wifi`/`status`/...). Erfuellt die
Pflichtenheft-3.7-Anforderung minimal; das eigentliche Dateiformat auf
`storage` ist weiterhin offen (siehe P3-Eintrag), daher bewusst kein
Datei-Listing/-Lesen ueber CDC, nur der Status.

### Bidirektionale CDC <-> interne Konsolen-Queue (Vorarbeit fuer P5)

Empfangene CDC-Rohbytes landen in einer FreeRTOS-Queue
(`usb_manager_get_cdc_rx_queue()`), noch ohne Verbraucher - P5 wird diese
Queue lesen, sobald die WebSocket-Konsole existiert (`UsbTask` aus dem
Pflichtenheft: "Bidirektionale Weiterleitung CDC <-> interne
Konsolen-Queue"). Queue nicht-blockierend befuellt (verwirft das aelteste
Element bei vollem Puffer, kein Block des TinyUSB-eigenen USB-Tasks).

### Wokwi: natives USB (TinyUSB/USB-OTG) - Korrektur, mehr simulierbar als angenommen

Vor der Implementierung stand hier die Annahme, Wokwi simuliere beim
ESP32-S3 nur die einfache USB-Serial-JTAG-Konsolenumleitung, nicht den
vollen TinyUSB-/USB-OTG-Stack, und P4 sei deshalb wie StorageManager (P3)
nur kompilier-, nicht simulierbar. **Das stimmt nur teilweise** - beim
tatsaechlichen ersten Boot-Test in Wokwi (siehe naechster Abschnitt) lief
`tinyusb_driver_install()` durch, das Geraet meldete sich sogar mit einem
DTR-Zustand ("CDC-Host bereit"). Wokwi fuehrt also zumindest die
TinyUSB-Deskriptor-/Treiberlogik software-seitig aus - genug, um einen
echten Deskriptor-Bug zu finden (siehe unten). Ob eine vollstaendige,
physikalisch korrekte USB-Host-Enumeration (echter PC am echten Port)
ebenso funktioniert, bleibt weiterhin nur auf echter Hardware pruefbar -
die Korrektur betrifft nur die Behauptung "gar nicht simulierbar", nicht
"vollstaendig verifiziert".

### Echter Bug gefunden: Kconfig-generierter Konfigurations-Deskriptor funktioniert NICHT mit HID

Der urspruengliche Ansatz (`tinyusb_config_t` komplett auf `{0}`, siehe
oben) schlug beim ersten tatsaechlichen Boot in Wokwi fehl:
`tinyusb_driver_install()` brach mit `ESP_ERR_INVALID_ARG` ab
("Configuration descriptor must be provided for this device"), das
Geraet crashte in einer Boot-Schleife. Ursache, direkt im Quellcode
gefunden (`managed_components/espressif__esp_tinyusb/descriptors_control.c`,
`tinyusb_set_descriptors()`): der automatisch generierte
Konfigurations-Deskriptor funktioniert nur, wenn **keine** der Klassen
HID/MIDI/ECM_RNDIS/DFU/DFU_RUNTIME/BTH aktiv ist - mit `TINYUSB_HID_COUNT=1`
verlangt `esp_tinyusb` zwingend einen selbst zusammengesetzten
Konfigurations-Deskriptor. Das war nicht durch Doku-Lektuere vorherzusehen
(die Kconfig-Beschreibung selbst erwaehnt diese Einschraenkung nicht) -
erst der Blick in `descriptors_control.c` und der tatsaechliche Boot-Test
deckten es auf.

**Fix**: Composite-Konfigurations-Deskriptor von Hand zusammengesetzt
(`TUD_CONFIG_DESCRIPTOR` + `TUD_CDC_DESCRIPTOR` + `TUD_HID_DESCRIPTOR`,
Interfaces 0/1=CDC, 2=HID, Endpoints 0x81/0x02/0x82/0x83), Geraete-Deskriptor
bleibt weiterhin NULL/Kconfig-generiert (davon ist nur der
Konfigurations-Deskriptor betroffen). Nach dem Fix startet Wokwi ohne
Absturz durch (`UsbManager gestartet`, `CDC-Host bereit (DTR=1)`,
`WebServerManager gestartet`). Beide Envs bauen fehlerfrei (echtes Board:
934393 B = 44,6% von 2 MB, wokwi-sim: 912257 B = 43,5%).

**Lehre fuers naechste Mal**: bei TinyUSB-Composite-Geraeten mit HID/MIDI/
DFU/BTH IMMER einen Boot-Test machen (und sei es nur in Wokwi, das reicht
fuer diese Klasse Bug bereits aus) statt sich auf "Kconfig generiert das
schon automatisch" zu verlassen, sobald eine dieser Klassen beteiligt ist.

## 2026-07-17 — P5 Webserver, erste Ausbaustufe (Login+Rollen+Uebersicht+Webconsole)

Umsetzung eines ersten Teils von `webconfig.txt` (vom Nutzer bereitgestellte
Detailspezifikation der Webserver-Seiten) und Pflichtenheft Abschnitt 3.6.
Bewusst NICHT in dieser Ausbaustufe: Einstellungen-Seite (IP-Config,
WireGuard-Upload, WLAN-Scan, Userverwaltungs-UI), Logs-Seite, Audit-Log,
24h-Sensor-Chart, HDD-LED-Blink-Anzeige (10s-Fenster) - siehe
Projekt-Memory fuer den vollstaendigen Rueckstand.

### UserManager: rollenbasierte Konten, RAM+storage-Persistenz

Neue Komponente `user_manager`. Vier Rollen aus `webconfig.txt`
(Leser/SSH User/Verwalter/Admin), Passwort-Policy (>=8 Zeichen, >=2 von
3 Zeichenklassen), Benutzername ohne Sonderzeichen. Passwoerter gesalzen
per `psa_hash_compute(PSA_ALG_SHA_256, ...)` gehasht (nie im Klartext
gespeichert) - modernes PSA-Crypto-API statt der klassischen
`mbedtls_sha256()`-Funktion, die unter mbedtls 4.x/TF-PSA-Crypto nur noch
ueber einen privaten Header erreichbar ist (gleiche Ursache wie beim
WireGuard-Fund in P1, hier aber von vornherein die richtige API gewaehlt
statt erst zu scheitern). Persistenz als JSON auf der `storage`-Partition
(`/storage/users.json`) ueber `espressif/cjson` (Component Manager, wie
schon `esp_littlefs`/`esp_tinyusb` zuvor - kein IDF6-Kompatibilitaetsproblem).
Erster Start ohne Benutzer: Default-Konto `admin`/`admin` (Rolle Admin)
wird angelegt und eine Warnung geloggt - analog dem
`installer`/`installer`-Fallback-Muster aus Sensormeter-WLAN.

Web-Sessions (Cookie-Token) ebenfalls in `user_manager` verwaltet (RAM-only,
30 Minuten Gueltigkeit, max. 8 gleichzeitige Sessions mit
Least-Recently-Issued-Verdraengung) - dieselbe Benutzerbasis wird spaeter
auch von P7 (SSH-Zugang) fuer die Public-Key-Zuordnung mitgenutzt (siehe
Chat-Diskussion zur SSH-Architektur: der ESP betreibt einen eigenen
SSH-Server, der Nutzer meldet sich direkt am ESP an, danach Zugriff auf
den gebrueckten CDC-/HID-Kanal - kein reiner Durchreiche-Proxy zum
Ziel-PC).

### WebServerManager: esp_http_server (eingebaut, keine neue Abhaengigkeit)

`esp_http_server` ist Teil des ESP-IDF-Kernframeworks (keine
Component-Manager-Abhaengigkeit noetig) und bringt WebSocket-Unterstuetzung
mit - muss aber explizit per `CONFIG_HTTPD_WS_SUPPORT=y` aktiviert werden
(Default ist aus). Umgesetzt: Login-Formular + Session-Cookie, Uebersichts-
seite (WLAN-IP, VPN-Status via `esp_wireguard_peer_is_up()`, NTC-/DHT-Werte,
Uptime, Power-LED-Anzeige), Logout, WebSocket-Endpunkt `/ws/console`
gebrueckt auf `usb_manager`s CDC-Queue (ein Hintergrund-Task pumpt
eingehende CDC-Rohbytes an den zuletzt verbundenen WebSocket-Client -
bewusst nur ein gleichzeitiger Konsolen-Client in dieser Ausbaustufe).

### PlatformIO-Bug: EMBED_TXTFILES/target_add_binary_data erzeugt kaputten doppelten Pfad

Beim Versuch, die statische Login-Seite als Datei einzubetten (wie bei
echten ESP-IDF-Projekten ueblich), schlugen **drei verschiedene,
eigentlich korrekte Ansaetze** nacheinander fehl, alle mit derselben
Fehlermeldung: `Source '.../login.html.S' not found, needed by target
'.../.pio/build/<env>/.pio/build/<env>/login.html.S.o'` (ein erkennbar
verdoppelter Pfad):

1. `idf_component_register(... EMBED_TXTFILES "data/login.html" ...)` in
   der eigenen Komponente
2. `target_add_binary_data(${COMPONENT_LIB} "data/login.html" TEXT)` in
   derselben Komponente (der niedrigere, von mbedtls fuer sein eigenes
   `x509_crt_bundle`-Embed genutzte Mechanismus - dort funktioniert es,
   hier nicht)
3. Derselbe `target_add_binary_data()`-Aufruf stattdessen in
   `main/CMakeLists.txt` (passend zu allen online auffindbaren
   PlatformIO-Beispielen, die das Embed ausschliesslich aus der
   main-Komponente heraus zeigen)

Alle drei scheiterten identisch, unabhaengig vom aufrufenden Ort - deutet
auf einen generellen Bug in PlatformIOs espidf-Ninja-Integration hin, nicht
auf einen Fehler in der CMake-Formulierung. **Pragmatischer Ausweg**: bei
dieser Dateigroesse (~1,4 KB) lohnt sich der Kampf gegen das kaputte
Embed-Tooling nicht - die Login-Seite ist stattdessen ein einfacher
C-String (`components/web_server_manager/include/login_html.h`). Fuer
groessere zukuenftige Web-Assets (falls das noetig wird) muesste dieser Bug
tiefer untersucht oder ein alternativer Mechanismus (z.B. Ablage auf der
`storage`-Partition statt Firmware-Embed) gewaehlt werden.

### Ergebnis

Beide Envs bauen fehlerfrei (echtes Board: 934393 B = 44,6% von 2 MB,
wokwi-sim: 912257 B = 43,5%). Wokwi-Boot-Test bestaetigt sauberen Start
aller Manager inkl. WebServerManager, keine echte HTTP-Anfrage gegen die
Simulation getestet (dafuer waere Wokwis Netzwerk-Gateway-Feature noetig,
nicht Teil dieser Session).

## 2026-07-17 — P5 Einstellungen-Seite (Nur-Bauen-Runde, kein Wokwi-Lauf)

Umsetzung des groessten Teils von `webconfig.txt`s "Seite Einstellungen".
Bewusst NICHT umgesetzt: Reset (nur Einstellungen / Einstellungen+Werte) -
"Werte" (Sensor-Historie) ist als Feature noch gar nicht gebaut, ein Reset
dafuer ist vor der Historie sinnlos zu definieren.

### NetworkManager: statische IP + Ping-Check, WLAN-Scan

Neu: `network_manager_apply_static_ip()`/`_use_dhcp()` (stoppt/startet den
DHCP-Client ueber `esp_netif_dhcpc_stop/start`, setzt `esp_netif_set_ip_info`)
mit Ping-Check vor der endgueltigen Uebernahme (`lwip/ping/ping_sock.h`,
`esp_ping_new_session`+Callback-basiert, in einen blockierenden Helfer mit
Semaphore gewrappt) - genau wie bei Sensormeter gefordert
("vor uebernehmen Ping-Check wie bei SM"). Schlaegt der Ping fehl, wird die
vorherige Konfiguration (DHCP oder vorherige statische Werte) wiederhergestellt.
WLAN-Scan (`network_manager_scan_wifi()`) liefert SSID+Offen/Verschluesselt+RSSI,
blockierend ueber `esp_wifi_scan_start(..., true)`.

### WireGuardManager: Laufzeit-Konfiguration per Upload ersetzt/ergaenzt Kconfig-Platzhalter

Neue Funktionen `wireguard_manager_apply_uploaded_config()` (parst
[Interface]/[Peer]-INI-Text von Hand, siehe `parse_conf()`) und
`_delete_config()`. **Default-Route-Entfernung** (webconfig.txt: "vor dem
speichern der conf default route aus dieser entfernen"): beim Parsen der
`AllowedIPs`-Zeile werden Eintraege `0.0.0.0/0`/`::/0` erkannt und
uebersprungen, bevor irgendetwas gespeichert wird - alle anderen
AllowedIPs-Eintraege werden nach dem Verbindungsaufbau einzeln per
`esp_wireguard_add_allowed_ip()` ergaenzt (das ist eine vom Grundtunnel
getrennte API in `esp_wireguard`, keine Erweiterung von
`wireguard_config_t` selbst). Persistenz als JSON auf der `storage`-Partition
(`/storage/wireguard.json`, ueber `espressif/cjson` wie schon bei
`user_manager`) - bewusst NICHT das rohe hochgeladene `.conf`-Textformat
gespeichert, sondern die bereits geparsten/bereinigten Felder in einem
eigenen, einfacheren Format (kein Grund, beim naechsten Boot dieselbe
INI-Parse-Arbeit nochmal zu machen).

**Architekturaenderung in `main.c`**: `wireguard_manager_init()` wird jetzt
IMMER aufgerufen (laedt bei Vorhandensein eine gespeicherte Konfiguration,
sonst weiterhin die Kconfig-Platzhalterwerte aus dem P1-Spike) - das war
zuvor komplett hinter `#if CONFIG_ESP_REMOTE_WG_ENABLE` versteckt. Der
tatsaechliche Verbindungsaufbau (`wireguard_manager_connect()`) erfolgt nur,
wenn entweder eine echte hochgeladene Konfiguration vorliegt ODER der
Kconfig-Entwicklertest explizit aktiv ist - sonst waere die ganze
Upload-Funktion wirkungslos geblieben (waere zwar gespeichert und beim
naechsten Boot geladen worden, aber nie tatsaechlich verbunden, ohne dass
der Entwickler-Kconfig-Schalter UND ein Neu-Flash noetig gewesen waeren).
Kleine Falle dabei: `CONFIG_ESP_REMOTE_WG_ENABLE` als Kconfig-Bool
existiert als C-Bezeichner nur, wenn er auf "y" steht (unset bools werden
NICHT als `0` definiert) - eine direkte Laufzeit-Verwendung
(`... || CONFIG_ESP_REMOTE_WG_ENABLE`) scheitert beim Kompilieren mit
"undeclared" sobald der Schalter aus ist. Fix: weiterhin per `#if`-Block
in eine lokale `bool` uebersetzen, dann diese `bool` normal zur Laufzeit
verwenden - Kconfig-Bools gehoeren an dieser Stelle grundsaetzlich in einen
Praeprozessor-Kontext, nicht direkt in C-Ausdruecke.

### UserManager: Konten-Auflistung ergaenzt

`user_manager_get_at()` fuer Anzeige/Config-Export - kleine Ergaenzung,
kein neues Muster.

### WebServerManager: Einstellungen-Seite + sechs neue POST-Endpunkte

`/settings` (Verwalter+Admin, via neuem `require_role()`-Helfer),
`/settings/network`, `/settings/wireguard/upload` (Konfiguration als
Textarea-Feld statt echtem Datei-Upload - `esp_http_server` hat keinen
eingebauten Multipart-Parser, ein Textfeld ist fuer diese Groessenordnung
[~500 Byte] die pragmatischere Wahl), `/settings/wireguard/delete`,
`/settings/users` (Kontoerstellung, nutzt bestehende
`user_manager_validate_*`-Funktionen), `/settings/config-download`
(JSON-Export: WLAN-/VPN-Status + Benutzerliste, bewusst OHNE Passwoerter/
Hashes/private Schluessel), `/settings/reboot` (loggt den ausloesenden
Benutzernamen vor `esp_restart()`). `httpd_config_t.max_uri_handlers` von
Default 8 auf 16 erhoeht (12 Handler registriert, Default reichte nicht
mehr). WLAN-Scan ist bewusst ohne JavaScript/AJAX geloest (Server-Redirect
`/settings?scan=1`, kompletter Seiten-Reload) - passt zum Rest der Seite,
die durchgehend ohne Client-JS auskommt.

### Ergebnis (nur gebaut, nicht in Wokwi getestet - explizit so gewuenscht)

Format-Truncation-Warnung (als Error behandelt) durch Vergroessern des
Seiten-Puffers auf 8192 Byte behoben (der generierte HTML-Text selbst ist
klein, GCCs statische Worst-Case-Abschaetzung der `%s`-Platzhalter war nur
zu konservativ fuer den vorherigen 4096-Byte-Puffer). Beide Envs bauen
fehlerfrei: echtes Board 1.042.597 B = 49,7% von 2 MB (+108.204 B ggue.
P5-Slice-1), wokwi-sim 1.021.065 B = 48,7%.

## 2026-07-17 — P5 Logs-Seite (Audit-Log + Sensor-Historie)

Umsetzung von `webconfig.txt`s "Seite Logs" (Audit-Log-Download inkl.
Verbindungsaufbau + Taster-Ereignisse; Sensorwerte-Download 24h). Zwei neue
Komponenten, jeweils bewusst minimal gehalten.

### Neu: `sensor_history` (24h-Ringpuffer, RAM-only)

`sensor_history_maybe_record()` wird einmal je 60s-SensorTask-Zyklus
aufgerufen und entscheidet selbst anhand von `esp_timer_get_time()`, ob
seit dem letzten Eintrag eine volle Stunde vergangen ist - kein eigener
Task/Timer noetig. Bewusst NICHT auf der `storage`-Partition persistiert
(anders als User-/WireGuard-Konfiguration): 24 kleine Eintraege sind nach
einem Neustart schnell wieder gefuellt, taeglicher Datenverlust bei einem
Reboot ist fuer eine reine Anzeige-/Download-Historie hinnehmbar und spart
staendigen (stuendlichen) Flash-Verschleiss. CSV-Export ueber
`sensor_history_get_csv()`.

### Neu: `audit_log` (persistent auf `storage`, Groessenrotation)

`/storage/audit.log`, rotiert nach `audit.log.old` bei >16 KB - gleiches
Rotationsmuster wie die bestehende Sensormeter-Familie. Zeitstempel sind
Uptime-Sekunden (`esp_timer_get_time()`), da ESP-Remote (anders als
Sensormeter) noch keinen NTP-Client/TimeManager hat - keine Wanduhrzeit
verfuegbar. Drei Ereignisse werden aktuell geloggt:

- Erfolgreicher Web-Login (`web_server_manager`, `login_post_handler`)
- Physischer Taster-Druck (`gpio_manager`, direkt am Punkt der
  entprellten Zustandsaenderung - `TasterKanal` bekam dafuer ein `name`-Feld,
  "Power"/"Reset")
- Ausgeloester Neustart inkl. Benutzername (`settings_reboot_post_handler`)

**Bewusst NICHT umgesetzt**: "Ansteuerung Taster" (Power/Reset per
Web-Button ausloesen) als eigenes Audit-Ereignis - dieses Feature (Taster
per Weboberflaeche steuern) existiert selbst noch nicht. Es wuerde eine
Erweiterung von `gpio_manager`s Entprellungs-/Weiterleitungslogik um einen
Software-Ausloeser noetig machen, der mit der bereits vollstaendig
verifizierten P2-Taster-/Tastschutz-Logik (siehe Wokwi-CLI-Szenarien)
sauber zusammenspielen muss - bewusst nicht "nebenbei" in dieser Runde
angefasst, sondern als eigener, spaeterer Schritt vorgesehen. `audit_log`s
generische API (`audit_log_add(const char* event)`) ist bereits bereit,
sobald diese Steuerung gebaut wird.

### `/logs`-Seite

Sensorwerte-CSV-Download fuer alle eingeloggten Rollen (webconfig.txt:
"Leser = Log Download nur Sensorwerte"), Audit-Log-Download nur ab
Verwalter (dieselbe Berechtigungsschwelle wie die Einstellungen-Seite).
Navigationslinks zu/von Uebersicht und Einstellungen ergaenzt.

### Zwei weitere Format-Truncation-Fixes (gleiche Ursachenklasse wie zuvor)

`settings_reboot_post_handler`s Event-Puffer war mit 48 Byte fuer
"Neustart ausgeloest von " (25 Zeichen) + einen bis zu 31 Zeichen langen
Benutzernamen zu knapp bemessen (GCCs statische Analyse rechnet mit dem
theoretischen Maximum, nicht mit typischen Werten) - auf 64 Byte
vergroessert. Kein neues Muster, dieselbe Lehre wie beim
Einstellungen-Seiten-Puffer oben: bei `%s`-Platzhaltern mit variabler,
aber bekannter Maximallaenge (hier `USERNAME_CAP`) den Zielpuffer lieber
grosszuegig bemessen als das GCC-Minimum auszureizen.

### Ergebnis

Wokwi-Boot-Test diesmal durchgefuehrt (anders als bei der
Einstellungen-Seite, wo der Nutzer das explizit uebersprungen hatte) -
sauberer Start, `audit_log`/`web_server_manager` melden sich korrekt,
keine neuen Abstuerze. `storage_manager`-Mount-Fehler weiterhin die
bekannte Wokwi-Grenze (siehe P3-Eintrag), kein neues Problem. Beide Envs
bauen fehlerfrei: echtes Board 1.046.145 B = 49,9% von 2 MB, wokwi-sim
1.024.421 B = 48,8%.

## 2026-07-16 — Bildschirmzugriff "in jedem Betriebszustand": Video-Wege verworfen, Serial Console Redirection als einzig gangbarer (aber board-abhängiger) Weg

Ursprünglicher Wunsch: den Bildschirminhalt des gesteuerten PCs in jedem
Betriebszustand abgreifen können, inklusive BIOS/UEFI/Bootloader-Phase vor
jedem Betriebssystem - und dabei möglichst kompatibel zu vielen
Mainboards bleiben, nicht auf bestimmte Modelle beschränkt sein.

### Geprüft und verworfen: HDMI-Videoerfassung über USB-Host

Der ESP32-S3 unterstützt USB-Host-Modus, worüber sich theoretisch ein
günstiger USB-HDMI-Capture-Dongle (typischer Chipsatz: MS2109)
anschließen ließe. Dokumentierter Fehlschlag eines identischen
Community-Projekts geprüft
([alu.dog "Failed project: An ESP32-S3 based KVM solution"](https://alu.dog/posts/failed-project-an-esp32-s3-based-KVM-solution/)):
MS2109-Dongles funktionieren nur korrekt im USB-2.0-High-Speed-Modus
(480 Mbit/s) - der ESP32-S3 kann als USB-Host aber nur Full-Speed
(USB 1.1, 12 Mbit/s). Die Descriptor-Antwort des Dongles ist im
Full-Speed-Modus fehlerhaft (Länge 0 an falscher Stelle), der ESP32-S3
hängt sich beim Parsen auf. Das ist eine Hardware-Grenze des
ESP32-S3-USB-Peripherals, kein lösbares Software-Problem. Ein einfacher
UVC-Webcam-Stream (kein HDMI-Capture) funktionierte im selben Projekt
dagegen bei 640x480/10fps - zeigt, dass die Rechenleistung fürs reine
Streaming ausgereicht hätte, das Problem liegt ausschließlich an der
USB-Geschwindigkeit beim Verbindungsaufbau zum Capture-Chip.

**Für dieses Projekt verworfen** - würde ohnehin nur mit Video ab
OS-Start funktionieren (BIOS/POST braucht sowieso eine andere Lösung,
siehe unten), UND selbst das nur mit unzuverlässiger/experimenteller
Hardware.

### Geprüft und verworfen: ESP32-S3 als USB-Grafikkarte emulieren

Idee: der ESP32-S3 gibt sich selbst als einfaches USB-Anzeigegerät aus
(Device-Modus, kein Host-Modus-Problem wie oben), analog kommerziellen
USB-Grafikadaptern (z. B. DisplayLink-Sticks).

**Grundsätzlich nicht möglich, unabhängig von Auflösung/Einfachheit**:
BIOS/UEFI bezieht die POST-/Setup-Bildschirmausgabe ausschließlich aus
der Firmware, die in der Grafikkarte selbst liegt (VGA-BIOS bei
Legacy-Boot, GOP-Treiber bei UEFI - beide Bestandteil der GPU-eigenen
Firmware/Option-ROM, vom Mainboard direkt als Teil des eigenen
Boot-Vorgangs ausgeführt). USB-Grafikadapter wie DisplayLink
funktionieren komplett anders: rein über Betriebssystem-Treiber (Windows/
Linux/Mac). Für die Firmware selbst ist ein USB-Gerät während
POST/Setup unsichtbar - es gibt keinen Standard-Mechanismus, über den
BIOS/UEFI ein beliebiges USB-Gerät als Bildschirm erkennen würde. Das
ist keine Frage der UEFI-Generation oder Implementierungsreife, sondern
grundsätzlich so, wie PC-Firmware Grafikausgabe handhabt (siehe
[OSDev-Wiki „GOP"](https://wiki.osdev.org/GOP): GOP-Support „ist in der
Firmware der Grafikkarte, nicht im Betriebssystemtreiber").

### Angenommen (mit Einschränkung): Serial Port Console Redirection

BIOS/UEFI-Firmware kann POST-Meldungen, Setup-Menü und teils frühen
Boot-Text zusätzlich über einen echten UART/seriellen Port ausgeben -
eine Firmware-Funktion, kein Software-Hack. Text statt Bild, aber bereits
ab dem allerersten Boot-Moment verfügbar, nicht erst sobald ein
Betriebssystem läuft. Elektrisch einfach: ein ESP32-S3-eigener UART kann
direkt an den seriellen Mainboard-Header (TTL) bzw. über einen
Pegelwandler (bei echtem RS-232-Pegel) angeschlossen werden - kein
USB-Host-Modus, keine Video-Hardware nötig.

**Einschränkung, bewusst akzeptiert**: das ist eine BIOS-Einstellung,
keine Universalfunktion. Server-/Workstation-Mainboards haben das meist
im Setup unter „Console Redirection" o. ä. Consumer-/Gaming-Mainboards
haben diese Option in den allermeisten Fällen gar nicht. Damit bleibt
ESP-Remote in diesem Punkt nicht vollständig board-unabhängig - das war
zwar der ursprüngliche Wunsch, ist aber technisch nicht anders lösbar,
ohne die Hardware-Basis komplett zu wechseln (siehe unten).

### Konsequenz für den Projektumfang

„Bildschirmausgabe in jedem Betriebszustand, board-unabhängig" ist mit
dem ESP32-S3 als Hardwarebasis **nicht erreichbar** - unabhängig davon,
wie viel Engineering-Aufwand investiert würde. Eine echte, board-
unabhängige Lösung bräuchte physisches Video-Tapping (HDMI/VGA-Signal
abgreifen) mit einer deutlich leistungsfähigeren Plattform als dem
ESP32-S3 (Vorbild: PiKVM/NanoKVM nutzen Raspberry-Pi-Klasse-SoCs, nicht
Mikrocontroller) - das wäre ein eigenständiges Hardware-/Projektthema,
nicht mehr "ESP-Remote auf Basis eines ESP32-S3".

**Entscheidung**: Serial Console Redirection wird als optionales Feature
in Lastenheft/Pflichtenheft aufgenommen, ausdrücklich mit dem Hinweis
"funktioniert nur auf Mainboards, deren Firmware das unterstützt". Reine
USB-Grafikkarten-Emulation und HDMI-Capture-über-USB-Host werden nicht
weiterverfolgt - dieser Eintrag dient als Referenz, falls die Idee in
einer späteren Session wieder aufkommt.

## 2026-07-17 — P1 Machbarkeits-Spike WireGuard: trombik/esp_wireguard baut nicht gegen ESP-IDF 6.0.1, droscy/esp_wireguard-Fork behebt das

Ziel (Pflichtenheft Abschnitt 12 / Implementierungsplan P1): pruefen, ob
WireGuard auf diesem Chip/dieser ESP-IDF-Version ueberhaupt baut und wie
gross der Flash-/RAM-Fussabdruck ist - noch ohne echtes Board, da keins
verfuegbar ist. NetworkManager (minimaler WLAN-STA-Aufbau) und
WireguardManager wurden dafuer als P0/P1-Grundgeruest neu angelegt
(`components/network_manager`, `components/wireguard_manager`).

### Geprueft und verworfen (fuer den aktuellen Stand): trombik/esp_wireguard

Der im Pflichtenheft als Kandidat genannte trombik/esp_wireguard (offiziell
nur bis ESP-IDF v4.4.x getestet, ESP32-S3 nicht in der Liste unterstuetzter
Targets) baute nicht gegen unser ESP-IDF 6.0.1:

- `mbedtls/entropy.h`/`mbedtls/ctr_drbg.h` existieren dort nicht mehr an der
  oeffentlichen Stelle - mbedtls 4.x hat auf die TF-PSA-Crypto-Architektur
  umgestellt und die klassische Entropy-API bewusst privat gemacht
  (`.../tf-psa-crypto/drivers/builtin/include/mbedtls/private/entropy.h`).
- `wireguard.c` scheiterte zusaetzlich an einem neuen GCC-Fehler
  (`-Werror=unterminated-string-initialization`) bei den
  `uint8_t[N] = "N-Zeichen-String"`-Initialisierern (CONSTRUCTION,
  IDENTIFIER, LABEL_MAC1, LABEL_COOKIE) - die Arrays sind exakt N statt
  N+1 Bytes gross angelegt, neuere GCC-Versionen werten das als Fehler.
- Letzter Push des Repos: 2024-10-10 (Projekt wirkt de facto unbetreut).

Testweise auf eine aeltere Plattform (`espressif32@6.13.0`, ESP-IDF ~5.5.3)
zurueckgestuft baute es sauber durch - bestaetigt, dass es sich um eine
reine Versions-Inkompatibilitaet handelt, kein grundsaetzliches
ESP32-S3-Problem. Ein projektweites ESP-IDF-Downgrade wurde als Option
verworfen, siehe Entscheidung unten.

### Angenommen: droscy/esp_wireguard (aktiv gepflegte ESPHome-Fork)

[droscy/esp_wireguard](https://github.com/droscy/esp_wireguard) ist ein
Fork derselben Codebasis (trombik/wireguard-lwip-Abstammung), gepflegt fuer
ESPHome, zuletzt aktualisiert 2026-04-17. Behebt beide oben genannten
Punkte explizit:

- `wireguard-platform.c` prueft `MBEDTLS_VERSION_NUMBER` und nutzt ab
  mbedtls 4.0 die PSA-Crypto-API (`psa_crypto_init()`), darunter weiter die
  klassische Entropy-API - funktioniert damit versionsuebergreifend.
- Die problematischen Array-Initialisierer sind ueber ein
  `U8_ARRAY_FROM_STR`-Makro ersetzt (korrekte Groessenberechnung).
- Nutzt `esphome/libsodium` fuer die X25519-Skalarmultiplikation statt
  vendorten NaCl-Codes.
- API-kompatibel bis auf zwei umbenannte Felder in `wireguard_config_t`:
  `allowed_ip`/`allowed_ip_mask` (trombik) -> `address`/`netmask`
  (droscy) - in `wireguard_manager.c` angepasst.

**Wichtig fuer die Einbindung**: droscy/esp_wireguard hat kein
`idf_component.yml` (kein ESP-IDF-Managed-Component), sondern ein
`library.json` - klassisches PlatformIO-Library-Format. Bei
`framework = espidf` verlinkt PlatformIOs Library Dependency Finder (LDF)
`lib_deps`-Bibliotheken erst am finalen ELF-Link-Schritt, ausserhalb des
ESP-IDF-CMake-Component-Graphen - ein `idf_component_register(REQUIRES
esp_wireguard ...)` in einer eigenen `components/`-Komponente findet sie
NICHT automatisch (kein Fehler, aber auch keine sichtbaren Header). Fix:
`wireguard_manager/CMakeLists.txt` verweist ueber `PRIV_INCLUDE_DIRS` mit
einem relativen Pfad direkt auf
`.pio/libdeps/<env>/esp_wireguard/{include,src}` (src/ zusaetzlich noetig,
da `esp_wireguard_err.h` dort statt in include/ liegt). Funktioniert, ist
aber ein Umgehungspfad, kein sauber dokumentiertes PlatformIO-Feature -
sollte bei einem PlatformIO-Update erneut geprueft werden.

### Ergebnis: Flash-/RAM-Fussabdruck (Pflichtenheft Abschnitt 10)

Build fuer `esp32-s3-devkitc-1-n16r8` (ESP-IDF 6.0.1, mit NetworkManager +
WireguardManager, ohne TinyUSB/Webserver):

- Flash: 771709 / 1048576 Byte = **73,6 %** der aktuell konfigurierten
  1-MB-OTA-App-Partition (`CONFIG_PARTITION_TABLE_TWO_OTA`).
- RAM: 37968 / 327680 Byte = 11,6 % (unkritisch).

Bestaetigt die im Pflichtenheft vermutete Risikoeinschaetzung. Bei 16 MB
Gesamtflash ist das kein Platzproblem, aber die Standard-"two_ota"-
Partitionstabelle (1 MB je Slot) reicht nicht mehr fuer TinyUSB (P4) +
HTTP/WebSocket-Server (P5) zusaetzlich - **neue offene Aufgabe**: vor P4/P5
eine eigene `partitions.csv` mit groesseren App-Partitionen anlegen
(reichlich Platz vorhanden, nur der Default-Preset ist zu klein).

### Stand / was noch fehlt

Kompilierbarkeit und Fussabdruck sind jetzt bekannt - ein echter
Tunnelaufbau (Handshake mit einem realen Peer) ist ohne Hardware nicht
pruefbar (Wokwi hat keinen Bezug zu einem echten WireGuard-Server). Der
Tunnelaufbau ist ueber `CONFIG_ESP_REMOTE_WG_ENABLE` (Kconfig, Default
aus) bewusst deaktiviert, bis eine echte Konfiguration eintragen wird -
die Platzhalter-Schluessel in `components/wireguard_manager/Kconfig` sind
32 Null-Bytes (syntaktisch gueltiges Base64, aber keine echten
Schluessel). P1 gilt als "gebaut, noch nicht auf Hardware verifiziert" -
Verifikation folgt gemaess Implementierungsplan, sobald ein Board da ist.

## 2026-07-17 — P5 Logs-Nachtrag: NTP-Zeitsynchronisation (TimeManager)

Bislang existierte keine NTP-Abfrage im Projekt - Zeitstempel in
`audit_log` waren reine Uptime-Sekunden. Neue Komponente `time_manager`,
1:1 nach dem bewaehrten Muster der Sensormeter-Familie: `esp_netif_sntp`
(moderner IDF-SNTP-Wrapper) mit `.start = false`; ein eigener Task
wartet per `xSemaphoreTake` mit Timeout entweder auf ein Link-Up-Signal
(`time_manager_notify_link_up()`, von `network_manager` bei
`IP_EVENT_STA_GOT_IP` ausgeloest) oder auf Ablauf von 5 Stunden
(`RESYNC_INTERVAL_MS`) - danach jeweils ein `esp_netif_sntp_start()`.
Zeitzone per POSIX-TZ-String `CET-1CEST,M3.5.0,M10.5.0/3` (Europe/Berlin
mit Sommerzeit).

`audit_log_add()` nutzt jetzt `time_manager_is_synced()`: nach dem
ersten erfolgreichen Sync echte Wanduhrzeit (`ctime_r`), davor weiterhin
der Uptime-Platzhalter (z.B. in den ersten Sekunden nach einem Kaltstart,
bevor der erste Sync durch ist).

Init-Reihenfolge in `main.c` wichtig: `time_manager_init()` muss vor
`network_manager_init()` laufen, da dessen Event-Handler die Semaphore
schon beim ersten Link-Up ansprechen koennen muss.

Beide Umgebungen (`esp32-s3-devkitc-1-n16r8`, `wokwi-sim`) bauen sauber
mit der neuen Komponente in der Abhaengigkeitskette von `network_manager`,
`audit_log` und `main`. Flash-Fussabdruck stieg minimal (Realboard: 50,3 %
der 2-MB-OTA-Partition, vorher ~50,1 %).

## 2026-07-17 — USB-Kommandoprotokoll (Host -> ESP-Remote)

Bislang gab es nur ein einzelnes Ad-hoc-Kommando ("storage", bare Wort +
Enter) fuer CDC-Kommandos - das war als Platzhalter markiert
("genaues Kommandoformat noch offen", Pflichtenheft 3.6/3.7). Anlass fuer
den Ausbau: `inital setup.txt` beschreibt ein Neugeraete-Rollout-Skript,
das VOR jeder Netzwerkverbindung per USB eine Default-Config bearbeiten
laesst, eine wireguard.conf hochlaedt und einen WLAN-Scan+Beitritt
durchfuehrt - das geht nur ueber genau diesen Kanal.

### Format

Kommandozeilen werden am Praefix `##ESPR ` erkannt, im selben Bytestrom,
der auch die Konsole des gesteuerten PCs durchreicht (Pflichtenheft
Abschnitt 6: CDC = Hauptkanal fuer die Konsolen-Kommunikation). Echter
Konsolentext beginnt praktisch nie zufaellig mit dieser Sequenz. Antworten:
erste Zeile immer `##ESPR OK [zusatz]` oder `##ESPR ERR <grund>`, danach
ggf. Nutzdaten, abgeschlossen durch `##ESPR END` - ein Host-Werkzeug liest
so immer bis zur END-Zeile.

Bewusst KEINE Sonderbehandlung, die Kommandozeilen aus der
Konsolen-Weiterleitungs-Queue heraushaelt: das haette Line-Buffering vor
jedem Zeichen-Forward erzwungen und die Interaktivitaet der echten Konsole
spuerbar verschlechtert. Ein paar Steuerzeichen, die dabei in der Live-
Konsole auftauchen, sind der akzeptierte Kompromiss - galt schon fuer das
alte "storage"-Kommando. Einzige Ausnahme: der laengere Rohdatenblock von
`wg upload <laenge>` (bis 2048 Byte) wird NICHT in die Konsolen-Queue
gespiegelt, da das ohnehin keine Konsolen-Interaktion waere, sondern ein
in sich geschlossener Steuerkanal-Datentransfer.

### Auth

Login ueber dieselben `user_manager`-Konten/Rollen wie im Web
(`login <benutzer> <passwort>`, Passwort = Rest der Zeile wegen moeglicher
Leerzeichen). Genau ein globaler USB-Sessionszustand (kein Cookie/Token
noetig - es gibt nur eine physische USB-Verbindung gleichzeitig).
Rollenschwellen 1:1 von den bestehenden Web-Handlern uebernommen (siehe
web_server_manager.c require_role()-Aufrufe): `status`/`log sensors` ab
Leser, `log audit`/`config download`/`reboot`/`wg upload`/`wg delete`/
`wlan scan`/`wlan join` ab Verwalter. Fuer die Erstinbetriebnahme meldet
sich das Setup-Skript einfach mit dem automatisch angelegten
Default-Konto `admin`/`admin` an.

### Neu: persistente WLAN-Zugangsdaten (`network_manager`)

Bisher kam die STA-SSID/PSK ausschliesslich aus Kconfig-Platzhaltern -
keine Moeglichkeit, sie zur Laufzeit zu setzen (auch nicht ueber das Web -
die Einstellungen-Seite kann WLANs nur scannen/anzeigen, nicht beitreten).
Neue Funktion `network_manager_join(ssid, password)`: persistiert auf
`/storage/wlan.json` (gleiches save/load-Muster wie
`wireguard_manager`s JSON-Persistenz), setzt `esp_wifi_set_config` und
stoesst einen Reconnect an. `network_manager_init()` laedt die
gespeicherte Konfiguration, falls vorhanden, sonst weiterhin die
Kconfig-Platzhalter.

### Kommandouebersicht

- `login`/`logout` - Session-Auth
- `status` - Spiegel der Uebersichtsseite (WLAN/VPN/Sensoren/Uptime/LEDs), ohne Netzwerk erreichbar
- `log audit` / `log sensors` - Exporte, identisch zu den Web-Downloads
- `config download` - JSON-Snapshot, wie der Web-Settings-Download
- `reboot` - mit Audit-Log-Eintrag wie im Web
- `wg upload <laenge>` (Rohdatenblock) / `wg delete` - WireGuard-Provisionierung ohne Netzwerk
- `wlan scan` / `wlan join <index> [psk]` - Scan+Beitritt per Index (kein SSID-Freitext, vermeidet Tokenizing-Probleme mit Leerzeichen in SSIDs)
- `storage` - unveraendert, jetzt unter dem einheitlichen Praefix statt als bares Wort

### Bewusst NICHT umgesetzt in dieser Runde

`config upload <laenge>` (generische Default-Config aus inital-setup.txt
Zeile 4-6) fehlt noch - dafuer muesste `config_manager` zuerst ein
persistiertes Config-Dateiformat bekommen (aktuell rein RAM-only fuer
Schwellwerte/Tastschutz, siehe Pflichtenheft 4.1 "Detailformat noch
offen"). Firmware-Upload (inital-setup.txt Zeile 9) ist bewusst kein
CDC-Kommando - laeuft ueber den zweiten, separaten USB-Port des Boards
(nativer USB-Serial/JTAG-Bootloader, Pflichtenheft Abschnitt 6) mit dem
bereits vorhandenen esptool/PlatformIO-Weg.

### Ergebnis

Beide Umgebungen (`esp32-s3-devkitc-1-n16r8`, `wokwi-sim`) bauen sauber.
Wokwi-Boot-Check diese Runde nicht moeglich (CLI-Token abgelaufen/
ungueltig, "API Error: Unauthorized" - kein Code-Problem). Noch nicht auf
echter Hardware getestet.

### Nebenbei: NTP-Sync jetzt im Audit-Log

`was loggen.txt` fordert "ntp sync (OK nicht OK)" - `time_manager` bekam
dafuer einen Callback (`time_manager_set_sync_result_cb()`), den `main.c`
mit `audit_log_add()` verdrahtet (bewusst kein direkter Aufruf aus
`time_manager.c`, um keine Kreisabhaengigkeit zu `audit_log` einzugehen,
das seinerseits `time_manager` fuer Zeitstempel braucht). Erkennung von
"nicht OK" ueber `esp_netif_sntp_sync_wait()` mit 15s-Timeout nach jedem
`esp_netif_sntp_start()`.

## 2026-07-17 — Restliches webconfig.txt: Taster-Steuerung, HDD-LED-Blink, 24h-Chart, Reset

Vier verbleibende Punkte aus `webconfig.txt` umgesetzt, jeweils auf beiden
Kanaelen (Web + USB-Kommandoprotokoll), wo die Spezifikation das verlangt.

### Taster-Steuerung (Power/Reset)

Neue `gpio_manager`-Primitive `gpio_manager_trigger_power(hold)` /
`gpio_manager_trigger_reset()`: wirkt ueber denselben Optokoppler-Ausgang
wie ein physischer Tastendruck (`taster_kanal_poll()` erweitert um einen
per Software gesetzten Zeitstempel `remote_release_at_us`) und
respektiert deshalb denselben Tastschutz - ist dieser aktiv, passiert
bewusst nichts (liefert false). Power kennt "kurz" (300ms, Soft-Power)
und "lang" (5000ms, erzwungenes Abschalten, uebliche PC-Konvention),
Reset nur einen kurzen Puls. Rollenlogik exakt nach webconfig.txt:
Admin ohne Ruecksicherung, Verwalter muss die Ausfuehrung mit dem
eigenen Passwort erneut bestaetigen (`settings_taster_post_handler` im
Web, `cmd_taster()` im USB-Protokoll - beide rufen dieselbe
`gpio_manager`-Funktion auf, keine Doppelimplementierung der eigentlichen
Steuerungslogik).

### HDD-LED-Aktivitaetsanzeige (10s-Fenster)

`gpio_manager_task()` merkt sich bei jedem 5ms-Poll-Zyklus zusaetzlich
den Zeitpunkt der letzten aktiven Flanke am HDD-LED-Eingang
(`s_hdd_led_last_active_us`). Neue Funktion
`gpio_manager_hdd_led_active_recently()` vergleicht das gegen ein
10s-Fenster - faengt damit auch kurze Blink-Impulse zwischen zwei
Seitenaufrufen ein (im Gegensatz zum reinen Momentan-Pegel von
`gpio_manager_read_hdd_led()`). Auf der Uebersichtsseite als rote,
per CSS-Keyframe blinkende Flaeche dargestellt (`webconfig.txt`: "eine
Rote Flaeche die blinkt"); im USB-`status`-Kommando als
`hdd_led_active_10s` mitgeliefert.

### 24h-Sensor-Chart

`sensor_history` bekam einen rohen Accessor
(`sensor_history_get_entries()`, gemeinsamer Typ `sensor_history_entry_t`
jetzt auch im Header) zusaetzlich zur bestehenden CSV-Ausgabe. Neuer
JSON-Endpunkt `/api/graph` (Session-geschuetzt, jede eingeloggte Rolle)
liefert Labels+drei Datenreihen (NTC-Temp, DHT-Temp, DHT-Feuchte).
Darstellung via Chart.js aus dem CDN, exakt dasselbe Muster wie das
etablierte `/api/graph` der Sensormeter-Familie (`fetch()` + zwei
Y-Achsen fuer Temperatur/Feuchte) - einzige Stelle auf der gesamten
ESP-Remote-Web-UI mit Client-JS, weil ein Chart ohne JS praktisch nicht
sinnvoll darstellbar ist (der Rest der UI bleibt bewusst reines
Server-HTML ohne AJAX).

### Reset (Einstellungen / Einstellungen+Werte)

Jeder betroffene Manager bekam eine eigene Reset-Funktion (kein
zentraler "God-Reset", jede Komponente kennt ihre eigenen Default-Werte
am besten): `config_manager_reset_to_defaults()` (Schwellwerte+
Tastschutz, rein RAM), `network_manager_reset()` (loescht
`/storage/wlan.json`), `wireguard_manager_delete_config()`
(wiederverwendet, existierte schon), `user_manager_reset_to_default()`
(loescht `/storage/users.json`, sett wieder nur admin/admin),
`sensor_history_reset()` (nur beim "+Werte"-Umfang). Das Audit-Log selbst
bleibt IMMER erhalten - ein Reset, der seine eigene Spur verwischt, waere
gegen den Zweck eines Audit-Logs. Beide Kanaele (Settings-Seite mit zwei
Buttons "nur Einstellungen"/"Einstellungen+Werte", USB `reset
settings|settings_values`) starten das Geraet danach neu, damit alle
betroffenen Komponenten (v.a. WLAN/WireGuard) sauber mit den
zurueckgesetzten Werten neu initialisieren.

### Ergebnis

Beide Umgebungen (`esp32-s3-devkitc-1-n16r8`, `wokwi-sim`) bauen sauber
(Flash real ~50.8%, RAM ~17.3%). Wokwi-Boot-Check bewusst nicht
durchgefuehrt (siehe Projekt-Memory: pausiert bis zum ersten echten
Hardware-Flash). Damit ist der komplette `webconfig.txt`-Funktionsumfang
umgesetzt - offen bleiben nur noch P6 (WireGuard-Normalbetrieb auf
echter Hardware), P7 (SSH-Server) und P8 (Verkabelung/E2E-Test), sowie
das bereits vorher als bewusst deferred markierte generische
`config upload` per USB.

## 2026-07-17 — Projekt umbenannt: ESP-Remote -> ESP-BMC

Grund: `esp-remote` kollidiert auf GitHub bereits mit einem existierenden,
aktiven Fremdprojekt gleichen Namens (BLE-Universalfernbedienung fuer
IR/RF-Geraete - inhaltlich komplett anderes Projekt). Neuer Name
**ESP-BMC** gewaehlt: kurz, sofort verstaendlich fuer die Zielgruppe
(Baseboard-Management-Controller-Analogie, IPMI/iLO/iDRAC-aehnlich, aber
bewusst ohne Videoerfassung - deckt sich mit der in Abschnitt 6.1
getroffenen Entscheidung gegen ein drittes USB-Interface). "BMC" ist ein
rein generischer Branchenbegriff, keine Markenrechte betroffen.

Umbenennung umfasst: Projektordner (`ESP-Remote/` -> `ESP-BMC/`),
`firmware/CMakeLists.txt` `project()`-Name, alle Kconfig-Optionen
(`CONFIG_ESP_REMOTE_*` -> `CONFIG_ESP_BMC_*`, betrifft
`network_manager`/`sensor_manager`/`wireguard_manager`/`sdkconfig.*`),
Log-TAGs, HTML-Seitentitel, Doku-Titel (`docs/*.txt`, `docs/*.md`,
`docs/*.html`), Wokwi-Diagramm/Szenario-Metadaten, `.vscode`-Pfade.
**Bewusst NICHT angefasst**: bestehende Eintraege in diesem Protokoll
(append-only-Konvention dieser Datei, siehe Kopf) - historische
Erwaehnungen von "ESP-Remote" oder `CONFIG_ESP_REMOTE_*` oben bleiben
unveraendert, weil sie den Stand zum jeweiligen Zeitpunkt korrekt
wiedergeben. Nur der Dokumenttitel (Zeile 1) wurde aktualisiert.

Nach der Umbenennung beide Umgebungen sauber neu gebaut (Kconfig-Rename
ist der riskanteste Teil - Build-Verifikation war Pflicht, nicht
optional). Danach `git init` im Projektordner, erster Commit - noch kein
Remote/Push (Repo bleibt vorerst lokal).

## 2026-07-17 — SNMP-Agent

Neue Komponente `snmp_manager`: schreibgeschuetzter SNMPv1-Agent (UDP
Port 161), analog dem bereits produktiven Muster der Sensormeter-Familie
(dort per Zabbix-Template bereits im Einsatz).

### Warum kein lwIP-SNMP-APPS-Modul

ESP-IDFs lwIP bringt ein eigenes SNMP-Modul mit (`CONFIG_LWIP_SNMP`),
dessen private-MIB-API aber recht umstaendlich ist (statischer Baum aus
`mib_node_t`/`mib_scalar_node_t`-Strukturen mit eigenen GET/GETNEXT/SET-
Callback-Konventionen, siehe lwIP-eigene `snmp_mib.h`/Beispiele) und in
ESP-IDF kaum dokumentiert/benutzt ist. Fuer eine kleine, feste
Skalar-OID-Tabelle (13 Werte, kein SET, kein GetBulk) ist das
unverhaeltnismaessig. Stattdessen: direkt auf einem rohen UDP-Socket
(`lwip/sockets.h`, ESP-IDF-Standardweg) ein minimaler, auf genau diese
Nachrichtenform zugeschnittener BER/ASN.1-Encoder/Decoder von Hand
geschrieben - inhaltlich derselbe Ansatz wie bei der Sensormeter-Familie
(dort per dritter Bibliothek in C++/Arduino), nur ohne Bibliotheks-
Abhaengigkeit.

### Umfang (bewusst eingeschraenkt)

- Nur GET und GETNEXT, kein SET (strukturell nicht implementiert - jede
  andere PDU wird mit `genErr` beantwortet), kein GetBulk. Zabbix-Hosts
  muessen deshalb "Use bulk requests" am SNMP-Interface deaktivieren.
- SNMPv1-Semantik fuer Fehlercodes, akzeptiert aber sowohl Version 0
  (v1) als auch 1 (v2c) im Request (wie Sensormeters Agent) - kein
  SNMPv3.
- Community-basierte Zugriffskontrolle (Default "public", auf der
  Einstellungen-Seite änderbar, persistiert unter `/storage/snmp.json`
  wie WLAN/WireGuard). Falsche Community: keine Antwort (kein
  Informationsleck ueber Existenz von OIDs).
- GETNEXT ist ein linearer Scan ueber die (aufsteigend sortierte)
  OID-Tabelle mit einem allgemeinen lexikografischen OID-Vergleich -
  funktioniert korrekt sowohl fuer gezielte Zabbix-Item-Abfragen als
  auch fuer einen manuellen `snmpwalk` zum Testen.

### OID-Schema

Private Enterprise-MIB unter `1.3.6.1.4.1.99999.10` (dieselbe frei
erfundene, unregistrierte "Haushalts"-Enterprise-Nummer 99999 wie bei
der Sensormeter-Familie, die dort direkt `.1` bis `.5` belegt - Zweig
`.10` ist neu und ESP-BMC vorbehalten). 13 Skalare (`.1.0` bis `.13.0`):
sysName, uptimeTicks, wlanIp, wlanSsid, vpnUp, vpnLocalIp,
ntcTempC×10, dhtTempC×10, dhtHumidityPct×10, powerLedOn,
hddLedActive10s, freeHeapBytes, wlanStatic. Temperaturen/Feuchte als
×10-Festkomma-INTEGER (gleiche Konvention wie Sensormeter), fehlende
Sensorwerte als Sentinel -32768 statt eines fehlenden OIDs (einfacher
fuer Zabbix-Trigger als "kein Wert" zu behandeln).

### Ergebnis

Beide Umgebungen bauen sauber (Flash real ~51.0%, RAM ~17.5%). Alle
Puffergroessen im Encoder wurden von Hand gegen den theoretischen
Worst-Case (10 Varbinds je Anfrage, laengster String-Wert) nachgerechnet -
kein Ueberlauf. Noch nicht gegen einen echten SNMP-Client (net-snmp
`snmpget`/`snmpwalk`, Zabbix) getestet - das geht erst mit echter
Hardware/Netzwerk. Ein passendes Zabbix-Template (analog
`docs/zabbix-template-sensormeter.yaml` der Sensormeter-Familie) ist
noch nicht angelegt.

## 2026-07-17 — SNMP-Agent: Systemname/-typ getrennt, SET fuer Power/Reset-Taste

Drei Ergaenzungen zum SNMP-Agenten auf Nutzerwunsch.

### Systemname (frei) vs. Systemtyp (fest)

`sysName` (.1) war bisher fest "ESP-BMC" - jetzt liest es einen frei
vergebbaren Namen (z.B. "Buero-PC"), neues Objekt `sysType` (.14, neu
angehaengt statt umnummeriert) liefert weiterhin fest "ESP-BMC". Der
Name lebt neu in `config_manager` (`config_manager_get/set_device_name()`),
dem bisher einzigen RAM-only-Modul dieses Projekts - dafuer bekam es
seine erste echte Persistenz (`/storage/device.json`, gleiches
JSON-Muster wie WLAN/WireGuard/SNMP), waehrend Tastschutz/Schwellwerte
bewusst weiterhin RAM-only bleiben (kein Anlass, den Rahmen des Moduls
jetzt komplett zu erweitern). Einstellungen-Seite: neues Feld im
System-Card. `config_manager_reset_to_defaults()` setzt den Namen mit
zurueck.

### SNMP SET fuer powerKey/resetKey

Neue Objekte `powerKey` (.15) und `resetKey` (.16), beide GET+SET:
- GET liefert den aktuellen Weiterleitungs-Zustand (1 = Tastendruck
  gerade aktiv, unabhaengig davon ob physisch oder per Web/USB/SNMP
  ausgeloest) - `gpio_manager_*_taste_weitergeleitet()`.
- SET loest einen Tastendruck aus, ueber dieselbe
  `gpio_manager_trigger_power()/_reset()`-Logik wie die Web/USB-
  Taster-Steuerung (respektiert also denselben Tastschutz). powerKey:
  1=kurz, 2=lang (erzwungenes Abschalten). resetKey: nur 1=kurz.

**Sicherheitsaspekt, bewusst gegenueber dem Nutzer dokumentiert:** SNMP
kennt anders als Web/USB keine Benutzeranmeldung - eine
Passwort-Rueckbestaetigung wie bei der Verwalter-Rolle ist hier
grundsaetzlich nicht moeglich. Deshalb: getrennte Lese-/Schreib-
Community (`snmp_manager_get/set_rw_community()`, Standardwert
"private" - uebliche SNMP-Konvention, analog ro/rwcommunity bei
net-snmp), NICHT dieselbe wie die Lese-Community. Wer nur die
Lese-Community kennt, bekommt bei einem SET-Versuch eine explizite
Fehlerantwort (kein stilles Verwerfen - die Community ist ja bereits als
bekannt bestaetigt, kein zusaetzliches Informationsleck). Jede
erfolgreiche wie abgelehnte SNMP-Ausloesung landet im Audit-Log
(Quell-IP statt Benutzername, da SNMP keine Identitaet hat) - Eintrag
"Taster ... ausgeloest von <ip> (SNMP)".

Bei mehreren Varbinds in einer SET-PDU: kein echtes Rollback bei
Teilfehlern (ein bereits ausgeloester Tastendruck laesst sich nicht
zuruecknehmen) - bei der ueblichen Nutzung (ein Varbind pro Request) ist
das nicht beobachtbar, volle Transaktions-Semantik waere fuer diesen
Anwendungsfall unverhaeltnismaessiger Aufwand.

### Bug beim Erstentwurf gefunden und behoben

Beim ersten Entwurf wurde die Quell-IP fuer den Audit-Log-Eintrag ERST
NACH der Varbind-Verarbeitungsschleife gesetzt - die Setter-Funktionen
lasen also noch die IP des vorherigen Pakets (oder NULL beim allerersten
SET). Vor dem ersten Build gefunden und korrigiert (IP-Ermittlung jetzt
vor die Schleife gezogen).

### Ergebnis

Beide Umgebungen bauen sauber (Flash real ~51.2%, RAM ~17.5%). Wie beim
Rest des SNMP-Agenten: noch nicht gegen einen echten SNMP-Client
getestet.

## 2026-07-17 — Setup-Tooling (inital-setup.txt umgesetzt, PowerShell statt Python)

`tools/` neu angelegt: `EspBmcLink.psm1` (PowerShell-Modul, Client fuer
das USB-Kommandoprotokoll ueber `System.IO.Ports.SerialPort`),
`Setup.ps1` (interaktives Erstinbetriebnahme-Skript, genau der Ablauf
aus `inital setup.txt`: Firmware flashen -> Konfigurationsvorlage im
Editor bearbeiten und per USB anwenden -> optional WireGuard-.conf aus
dem aktuellen Ordner importieren -> optional WLAN-Scan+Beitritt),
`config_template.txt` (editierbare Vorlage: Systemname, SNMP-
Communities, Schwellwerte, Tastschutz).

Erster Entwurf war in Python (pyserial) - auf Wunsch auf reines
PowerShell umgestellt (kein zusaetzliches Laufzeit-/Paket-
Requirement, passt zur ueberwiegend PowerShell-basierten
Arbeitsumgebung). Response-Parsing-Logik gegen synthetische, dem
Firmware-Code exakt nachgebildete Antworten getestet (kein Zugriff auf
echte Hardware fuer einen echten End-to-End-Test).

Dafuer noetige neue USB-Kommandos in `usb_manager.c` ergaenzt:
`system set <name>`, `snmp set <ro> <rw>`,
`thresholds set <ntc> <dht> <hum>`, `tastschutz set 0|1` - vorher gab es
keinen USB-Weg, diese vier Werte zu setzen (nur ueber die Web-UI).
`config download` liefert jetzt zusaetzlich Systemname/-typ,
Tastschutz, Schwellwerte und SNMP-Communities (vorher nur
Netzwerk/VPN/Benutzer) - dient auch als Verifikation nach dem Anwenden
der Konfigurationsvorlage.

**Kein Git-Remote-Download** (fuer Firmware oder Vorlage) - es existiert
noch kein Remote, das Skript arbeitet mit dem lokalen Checkout
(`-EnvName`/`-Template` zeigen bei Bedarf auf etwas anderes). Nachtrag
faellig, sobald ein Remote existiert.

Auf Wunsch noch keine Nutzungsdokumentation geschrieben (nur dieser
Entscheidungslog-Eintrag) - Skripte sind selbsterklaerend genug fuer den
Moment (Kommentare + `Get-Help Setup.ps1`).

## 2026-07-17 — Nachtrag Setup-Tooling: Inkonsistenzen gefunden und behoben

Auf Nutzeranfrage die eben fertiggestellten Tooling-Dateien nochmal
geprueft.

- **`config_template.txt` referenzierte noch `setup.py`** (zwei Stellen,
  Kommentare) - Ueberbleibsel vom verworfenen Python-Entwurf. Auf
  `Setup.ps1` korrigiert.
- **Echter Bug gefunden: `[double]$Values["ntc_temp_max_c"]` in
  Setup.ps1 nutzt die aktuelle System-Kultur.** Unter de-DE (dem System,
  auf dem das laeuft) wird `[double]"60,5"` NICHT als Fehler abgelehnt,
  sondern lautlos zu `605` fehlinterpretiert (Komma als
  Tausendertrennzeichen statt Dezimaltrennzeichen) - empirisch
  nachgestellt und bestaetigt. Ein deutschsprachiger Nutzer, der in der
  Konfigurationsvorlage naturgemaess ein Komma statt Punkt tippt, haette
  also einen kaputten Schwellwert unbemerkt aufs Geraet uebertragen.
  Behoben mit einer expliziten `[double]::TryParse(...,
  [CultureInfo]::InvariantCulture)`-Hilfsfunktion
  (`ConvertTo-InvariantDouble`), die bei ungueltigem Format jetzt einen
  klaren Fehler wirft statt einen falschen Wert stillschweigend zu
  akzeptieren. `config_template.txt` weist jetzt explizit auf "Punkt,
  kein Komma" hin.
- **Bekannte, nicht behobene Einschraenkung (kosmetisch, nicht neu):**
  Das `status`-Kommando in `usb_manager.c` (bereits vor dieser Session
  gebaut) sendet Key=Value-Paare leerzeichengetrennt in einer Zeile
  (`wlan_ssid=%s ...`). Eine SSID mit Leerzeichen (z.B. "Mein Heim
  WLAN") wuerde beim leerzeichenbasierten Parsen in
  `Get-EspBmcStatus` falsch aufgeteilt. Betrifft nur die
  Status-Anzeige am Ende von `Setup.ps1` (rein informativ, keine
  Folgeaktion haengt daran) - `wlan scan`/`wlan join` sind davon NICHT
  betroffen (semikolon-getrennt, siehe `cmd_wlan_scan()`). Fix wuerde
  eine Aenderung am Wire-Format von `cmd_status()` erfordern (z.B.
  Anfuehrungszeichen um String-Werte) - bewusst nicht spontan
  mitgemacht, da das den Rahmen dieser Pruefung gesprengt haette.

## 2026-07-17 — Projektstand-Dokument angelegt

Neu: `docs/projektstand.md` - Abgleich Lastenheft/Pflichtenheft gegen
den tatsaechlichen Code-Stand (nicht nur gegen das, was hier im
Entscheidungslog irgendwann mal geplant war). Enthaelt Partitionstabelle
und aktuellen Flash-/RAM-Fuellstand (frisch nachgebaut fuer diesen
Eintrag, nicht aus dem Gedaechtnis zitiert).

Zwei konkrete, bisher nirgends explizit festgehaltene Luecken beim
Abgleich gefunden:
- Tastschutz laesst sich per USB umschalten, aber **nicht ueber die
  Weboberflaeche** - Lastenheft Abschnitt 8 fordert das aber
  ausdruecklich fuer das Webinterface.
- `gpio_manager_set_power_led()`/`_set_hdd_led()` (Gehaeuse-LED-
  Ansteuerung, Lastenheft Abschnitt 5) existieren als GPIO-Funktion,
  werden aber von keiner Stelle im Code aufgerufen - keine Web-/USB-
  Anbindung vorhanden.

Beide als offene Punkte in `docs/projektstand.md` Abschnitt 2
aufgenommen, noch nicht behoben.

## 2026-07-17 — Beide in projektstand.md gefundenen Luecken behoben

### Tastschutz: Web-UI-Schalter nachgezogen

Neue Checkbox-Karte auf der Einstellungen-Seite
(`settings_tastschutz_post_handler`, `/settings/tastschutz`,
Verwalter+). USB-Kommando (`tastschutz set 0|1`) existierte bereits aus
der Setup-Tooling-Arbeit vom selben Tag - hier nur die fehlende
Web-Anbindung ergaenzt, keine neue Logik in `config_manager` noetig.

### Gehaeuse-LED-Ansteuerung: Web + USB nachgezogen

`gpio_manager_set_power_led()`/`_set_hdd_led()` existierten als
GPIO-Funktion, hatten aber keinen Aufrufer. Neu:
- Software-Schattenkopie des gesetzten Zustands
  (`gpio_manager_power_led_out_state()`/`_hdd_led_out_state()`) - aus
  demselben Grund wie bei `TasterKanal.weitergeleitet`: kein
  zuverlaessiges GPIO-Readback auf einem OUTPUT-Pin.
- Web: Checkbox-Karte "Gehaeuse-LEDs" auf der Einstellungen-Seite
  (`settings_led_post_handler`, `/settings/led`, Verwalter+).
- USB: neues Kommando `led set power|hdd 0|1`.

Beide Wege rufen dieselbe `gpio_manager`-Funktion auf, keine doppelte
Logik. Rollen-/Audit-Verhalten konsistent mit den uebrigen
Einstellungen (Verwalter+, Audit-Log-Eintrag, kein Passwort-
Ruecksicherungszwang wie bei der Taster-Steuerung - reine
Anzeige-/Diagnosefunktion, kein physischer Taster-Effekt).

### Ergebnis

Zwei format-truncation-Warnungen beim ersten Build (Event-Puffer zu
knapp bemessen, bekanntes Muster) - Puffer vergroessert, danach beide
Umgebungen sauber gebaut (Flash real 51,3 % / 1.076.741 Byte, RAM 17,5 %;
Wokwi-Sim 50,3 % / 1.055.069 Byte).

`docs/projektstand.md` Abschnitt 1/2 aktualisiert - beide Punkte von
"offen" auf "umgesetzt" verschoben.

## 2026-07-17 — SSH-Server (P7)

Architektur wie mit dem Nutzer festgelegt: der ESP betreibt einen
**eigenen SSH-Server** (nicht Pass-Through/Proxy zu einer sshd auf dem
gesteuerten PC). Ein Nutzer meldet sich direkt am ESP an (dieselbe
user_manager-Kontodatenbank wie Web/USB), die Sitzung steuert danach
dieselbe CDC/HID-Bruecke wie die WebSocket-Konsole.

### Bibliothek: wolfssl/wolfssh ueber den ESP-Component-Registry

wolfssl/wolfssh (v1.4.20) ist als offizielle Managed Component
verfuegbar (components.espressif.com), zieht wolfssl/wolfssl (v5.8.2)
als eigene Abhaengigkeit. Deutlich saubererer Ausgangspunkt als die
WireGuard-Bibliothekssuche (kein Fork noetig) - aber die Integration
selbst war trotzdem eine laengere Fehlerkette, komplett neu fuer dieses
Projekt (kein Vorbild in der Sensormeter-Familie). Reihenfolge der
gefundenen und behobenen Probleme, damit ein zukuenftiges "das baut
ploetzlich nicht mehr"-Problem schneller einsortiert werden kann:

1. wolfssh alleine reicht nicht - idf_component.yml braucht explizit
   auch wolfssl/wolfssl, sonst schlaegt die Komponentenaufloesung fehl
   ("Failed to resolve component 'wolfssl'").
2. WOLFSSL_USER_SETTINGS propagiert nicht komponentenuebergreifend.
   wolfssl/wolfssls eigene CMakeLists.txt setzt dieses Define zwar
   selbst, aber nur fuer die eigene Komponente - wolfssl/wolfssh ist ein
   separates Component-Target und sieht es nicht. Ohne das Define sucht
   wolfssh/ssh.h nach dem autoconf-generierten wolfssl/options.h, das es
   bei einer Managed-Component-Installation nie gibt. Fix:
   -DWOLFSSL_USER_SETTINGS projektweit in firmware/CMakeLists.txt
   (CMAKE_C_FLAGS, vor project()).
3. WOLFSSH_NO_RSA-Redefinitionsfehler - das mitgelieferte
   wolfssl/wolfssl/include/user_settings.h-Template setzt dieses Define
   fuer ESP32-S3 bereits selbst (RSA per Default aus, ECC per Default an
   - passt zufaellig genau zu unserem Plan). Ein eigenes
   -DWOLFSSH_NO_RSA fuehrt zu einer Redefinitions-Fehlermeldung - nicht
   noetig, einfach weglassen.
4. #warning "RSA may be difficult with less than 10KB Stack" wird durch
   das projektweite -Werror zum harten Build-Fehler, obwohl wolfSSH RSA
   in unserer Konfiguration gar nicht nutzt (nur WOLFSSH_NO_RSA, nicht
   wolfCrypts eigenes NO_RSA, war gesetzt - der RSA-Primitive-Codepfad in
   wolfCrypt selbst blieb dadurch aktiv). Fix: zusaetzlich -DNO_RSA
   projektweit.
5. ESP32-S3-Hardwarebeschleunigungs-Ports von wolfSSL sind gegen unsere
   ESP-IDF-Version (6.0.1) kaputt - esp32_aes.c/esp32_sha.c referenzieren
   PERIPH_AES_MODULE und hal/clk_gate_ll.h, beides in dieser
   IDF-Version umbenannt/entfernt (dieselbe Kategorie Problem wie bei
   WireGuard/mbedtls 4.x - IDF6 bricht wiederholt Drittbibliotheken, die
   auf interne ESP-IDF-APIs zugreifen). Reine Software-Kryptografie ist
   fuer eine von Hand bedienten SSH-Konsole (kein Hochdurchsatz-TLS)
   voellig ausreichend. Fix: -DNO_ESP32_CRYPT UND zusaetzlich die
   feingranularen Einzel-Flags (-DNO_WOLFSSL_ESP32_CRYPT_HASH
   -DNO_WOLFSSL_ESP32_CRYPT_AES -DNO_WOLFSSL_ESP32_CRYPT_RSA_PRI) - der
   grobe Schalter alleine reicht nicht, wolfssl/openssl/sha.h prueft
   z.B. nur den feingranularen Schalter direkt und referenziert sonst
   den (jetzt nicht mehr deklarierten) Typ WC_ESP32SHA.
6. CONFIG_ESP_ENABLE_WOLFSSH (Kconfig) fehlte - das mitgelieferte
   user_settings.h-Template schaltet WOLFSSL_WOLFSSH/WOLFSSH_TERM/
   WOLFSSL_KEY_GEN/WOLFSSL_PTHREADS nur frei, wenn dieser
   Kconfig-Schalter aktiv ist (Default aus!) - ohne ihn fehlte u.a.
   wc_SSH_KDF (implicit declaration). In allen drei sdkconfig-Dateien
   ergaenzt.
7. Reserviertes Schluesselwort thread_local kollidiert mit einem
   Enum-Member-Namen in wolfcrypt/src/port/Espressif/esp_sdk_mem_lib.c
   (eine reine Speicher-Introspektions-/Debug-Datei, fuer
   SSH-Funktionalitaet irrelevant) - unter unserem Toolchain (GCC
   15.2.0, vermutlich C23-Default) ist thread_local ein echtes
   Schluesselwort, kein Makro (-Uthread_local bewirkt NICHTS, wurde
   ausprobiert und verworfen). Wichtig fuer die Zukunft: ein Hand-Patch
   der Datei in managed_components/ wird vom
   ESP-IDF-Component-Manager bei jeder CMake-Neukonfiguration wieder
   verworfen (Cache-Integritaetspruefung stellt den Originalzustand
   wieder her) - deshalb liegt der Fix jetzt als idempotenter
   CMake-Schritt in firmware/CMakeLists.txt (nach project(), ueberprueft
   bei jeder Konfiguration ob der Patch noch drin ist und wendet ihn
   sonst erneut an). Das ist robuster als ein manueller Patch, aber
   falls wolfssl/wolfssl auf eine neue Version aktualisiert wird und
   sich die betroffene Codezeile aendert, muss dieser
   CMake-Patch-Block eventuell nachgezogen werden.

### Host-Key

ECC P-256 (RSA projektweit deaktiviert), einmalig erzeugt
(wc_ecc_make_key+wc_EccKeyToDer) und auf /storage/ssh_host_key.der
persistiert - ohne das wuerde bei jedem Neustart ein neuer Host-Key
entstehen und jeder SSH-Client vor einem vermeintlichen MITM warnen.

### Authentifizierung

Passwort UND Public-Key, beide gegen dieselbe user_manager-
Kontodatenbank, Mindestrolle SSH_USER. Public-Key-Vergleich: der Nutzer
hinterlegt seinen oeffentlichen Schluessel im ueblichen OpenSSH-
Zeilenformat ueber ein neues Selbstbedienungs-Formular auf der
Uebersichtsseite (/account/ssh-key, jeder Nutzer nur fuer sich selbst -
kein Admin-Formular fuer fremde Konten, dafuer gibt es keinen Bedarf).
user_manager speichert nur den Base64-Block (der Typ-String steckt
redundant im Wire-Format-Blob selbst), dekodiert ihn bei der Anmeldung
und vergleicht byteweise gegen den vom SSH-Client praesentierten Blob -
wolfSSH prueft die kryptografische Signatur selbst, bevor unser Callback
mit hasSignature=1 aufgerufen wird. Nur ECDSA/Ed25519-Client-Schluessel
funktionieren (RSA projektweit deaktiviert) - im Formular-
Platzhaltertext vermerkt.

### Konsolen-Bruecke und Exklusivitaet

SSH-Session-Ein-/Ausgabe wird auf dieselbe usb_manager-CDC-Queue/
-Schreibfunktion gebrueckt wie die WebSocket-Konsole. Da eine
FreeRTOS-Queue jedes Element nur an EINEN Empfaenger liefert, braucht es
Exklusivitaet zwischen den beiden Konsolen-Quellen - neu:
usb_manager_console_claim()/_release()/_owner() (drei Zustaende:
NONE/WEB/SSH). console_pump_task (Web) leert die Queue jetzt nur noch,
wenn sie tatsaechlich Besitzer ist; die SSH-Sitzung beansprucht den
Besitz erst nach erfolgreicher Anmeldung und gibt ihn beim Sitzungsende
wieder frei. Genau eine gleichzeitige Sitzung ist damit strukturell
erzwungen (TCP-Listen-Backlog zusaetzlich auf 1 gesetzt) - ein zweiter
Verbindungsversuch waehrend einer aktiven Sitzung wird abgelehnt.

wolfSSH_worker() (blockierender High-Level-Kanal-Dispatcher der
Bibliothek) wird in einer Schleife mit kurzem Socket-Timeout (200ms,
SO_RCVTIMEO) aufgerufen, damit dieselbe Schleife auch regelmaessig die
CDC-RX-Queue in Richtung SSH-Client leeren kann (kein echtes
asynchrones I/O, aber fuer eine von Hand bedienten Konsole voellig
ausreichend - dasselbe Muster wie console_pump_tasks Polling).

### Ergebnis

Beide Umgebungen bauen sauber (Wokwi-Sim diese Runde nicht gebaut - auf
Nutzerwunsch pausiert, siehe Projekt-Memory). Real-Hardware-Env: Flash
stieg von 51,3 % (vor P7) auf 59,1 % (1.239.409 Byte von 2 MB) - der
Sprung kam erst mit der tatsaechlichen ssh_manager.c-Implementierung
(der anfangs nur kompilierende Platzhalter hatte praktisch keinen
Effekt, da der Linker unreferenzierten wolfSSH/wolfSSL-Code
herausgeworfen hatte) - unter 2 MB weiterhin komfortabel Platz. RAM
18,6 % (60.864 Byte), weiterhin unkritisch.

Nicht auf echter Hardware/gegen einen echten SSH-Client getestet - wie
bei P1 (WireGuard) gilt: kompiliert, Fussabdruck gemessen,
Protokollkorrektheit (insbesondere der zweistufige
Public-Key-Handshake und das Non-Blocking-Verhalten von
wolfSSH_worker() unter echtem Netzwerk-Jitter) noch nicht verifiziert.

Bewusst nicht umgesetzt:
- Kein USB-Kommando fuer das SSH-Key-Hinterlegen (nur Web) - ein
  100+-Zeichen-Base64-Blob ueber ein rohes Terminal einzutippen waere
  schlechte UX, das Web-Formular ist der sinnvollere Weg.
- Kein Rate-Limiting gegen SSH-Brute-Force - identisch zum bisherigen
  Web-/USB-Login-Verhalten, keine Regression, aber auch keine
  Verbesserung.
- Keine SSH-Sitzungs-Warteschlange - ein zweiter Verbindungsversuch
  waehrend einer aktiven Sitzung wird abgelehnt, nicht verzoegert bis
  die erste endet.

### Nachtrag 2026-07-17 (spaeter): Host-Key-Fingerprint auf der Uebersichtsseite

Auf Nutzerfrage ("ist der Key als vertraulich zu betrachten?"):
**nein** - der oeffentliche Host-Key wird bei jedem Handshake ohnehin an
jeden verbindenden Client uebertragen, das ist kein Geheimnis (im
Gegensatz zum privaten Schluessel, der das Geraet nie verlaesst).
Deshalb auf die Uebersichtsseite gesetzt statt hinter die
Einstellungen-Seite - fuer jeden angemeldeten Nutzer sichtbar, keine
Rollenbeschraenkung noetig, da nur Anzeige (kein Setzen/Aendern).

wolfSSH selbst bietet keine Export-/Fingerprint-Hilfsfunktion (geprueft
in wolfssh/ssh.h - nur WS_UserAuthData_PublicKey/WS_CallbackPublicKeyCheck
fuer die Client-Seite, nichts fuer die eigene Host-Key-Anzeige). Von Hand
ueber wolfCrypt aufgebaut:
- `wc_EccPrivateKeyDecode()` dekodiert den persistierten DER-Host-Key
  zurueck in ein `ecc_key` (ssh_manager haelt sonst nur die rohen
  DER-Bytes, kein Live-`ecc_key`-Objekt nach der Init-Phase).
- `wc_ecc_export_x963()` liefert den oeffentlichen Punkt im Format
  `0x04||X||Y` - das ist bytegleich mit dem "Q"-Feld, das das
  SSH-Wire-Format (RFC 5656) fuer ECDSA-Keys erwartet, direkt
  weiterverwendbar ohne Umrechnung.
- Wire-Format-Blob von Hand zusammengesetzt (laengenpraefixierte
  Strings: Typ "ecdsa-sha2-nistp256", Kurvenname "nistp256", Punkt) -
  daraus per Base64 die volle Public-Key-Zeile (OpenSSH-Format) und per
  SHA256 (`wc_InitSha256`/`wc_Sha256Update`/`wc_Sha256Final` - kein
  Einzelaufruf-Wrapper in dieser Codebasis vorhanden) + Base64-ohne-
  Padding der Fingerprint im selben `SHA256:...`-Format wie
  `ssh-keygen -lf`.
- Ein Base64-**Encoder** war neu (user_manager.c hatte bisher nur einen
  Decoder fuer den SSH-Public-Key-Vergleich) - bewusst lokal in
  ssh_manager.c, kein gemeinsames Utility-Modul (passt zum bisherigen
  Muster dieser Codebasis, z.B. role_name() ist ebenfalls in mehreren
  Komponenten dupliziert statt geteilt).

Berechnung einmalig in `ssh_manager_init()` direkt nachdem der
Host-Key-DER verfuegbar ist (geladen oder neu erzeugt), Ergebnis in
zwei statischen Puffern gecacht (`ssh_manager_get_host_key_fingerprint()`
/ `ssh_manager_get_host_public_key_line()`) - keine wiederholte
Neuberechnung pro Seitenaufruf.

## 2026-07-17 — Hinweis: erster Boot nach dem Flashen dauert laenger

Auf Nutzerfrage ("dann wird der erste Start nach dem ersten Flash
laenger dauern?"): ja, aus zwei voneinander unabhaengigen Gruenden, die
beide **nur beim allerersten Boot** greifen (bzw. nach jedem Loeschen/
Neuformatieren der `storage`-Partition, z.B. durch ein komplettes
Neuflashen mit vorherigem Erase):

1. `storage_manager_init()` (erster Aufruf in `app_main()`,
   `firmware/main/main.c`) mountet LittleFS mit
   `format_if_mount_failed = true` (`storage_manager.c`). Eine frisch
   geflashte `storage`-Partition ist nicht formatiert -> der erste Mount
   formatiert sie einmalig. Das blockiert `app_main()`, bevor
   `usb_manager_init()` (und damit die `##ESPR`-Kommandoschnittstelle,
   die z.B. `tools/Setup.ps1` benutzt) ueberhaupt startet.
2. `ssh_manager_init()` (deutlich spaeter im Boot-Ablauf, nach
   `web_server_manager_init()`/`snmp_manager_init()`) erzeugt beim
   allerersten Mal einen neuen ECC-P256-Host-Key
   (`generate_and_save_host_key()`, siehe SSH-Server (P7) oben) - reine
   Software-Kryptografie (`NO_ESP32_CRYPT`), also nicht so schnell wie
   mit Hardwarebeschleunigung, aber fuer P-256 (im Gegensatz zu RSA)
   ohnehin im Bereich von Millisekunden bis niedrigen
   Hundert-Millisekunden, keine grosse Verzoegerung fuer sich allein.
   Blockiert nur `ssh_manager_init()` selbst, nicht den Rest des
   Bootvorgangs (laeuft parallel zu nichts, aber auch nach nichts
   Kritischem).

Der dominante Faktor ist (1), nicht (2) - exakte Dauer nicht auf echter
Hardware gemessen (noch kein Board vorhanden), da LittleFS-Formatierung
groessenordnungsmaessig von der Flash-Loeschgeschwindigkeit abhaengt.
Bei jedem weiteren Boot entfallen beide Schritte (Mount ohne Format,
Host-Key wird nur geladen) - Bootzeit dann wie gewohnt.

**Reaktion:**
- `tools/Setup.ps1` wies bisher nur mit `Read-Host` zum manuellen
  Wiederverbinden an, ohne auf diesen Umstand hinzuweisen, und versuchte
  danach sofort einmalig zu verbinden (bei einem Timeout waere das
  Skript direkt mit einer Exception abgebrochen). Jetzt: Hinweistext
  beim Flash-Schritt + Retry-Schleife um den ersten Verbindungsversuch
  (`Connect-EspBmc`, bis zu 10 Versuche, 2s Abstand) statt eines
  einzelnen Versuchs, siehe `Connect-EspBmcWithRetry` in `Setup.ps1`.
- Dieser Hinweis hier plus ein Kommentar direkt im Code
  (`storage_manager.c`/`ssh_manager.c`) sind die "im Repo"-Doku-Seite
  der Nutzeranforderung.

Ergebnis: Real-Hardware-Env baut sauber, Flash 59,2 % (1.241.905 Byte,
+2.496 Byte gegenueber dem vorherigen P7-Stand), RAM 18,7 % (61.232
Byte) - beides der erwartete kleine Zuwachs durch Base64-Encoder +
Wire-Format-Aufbau, unkritisch.

## 2026-07-17 — Watchdog-LED (RGB, GPIO48)

Auslöser: beim Betrachten von `board mit bezeichner.bmp`
(Projekt-Root, Pinout-Beschriftung des diymore-Boards) fiel eine
onboard-adressierbare RGB-LED auf - laut Beschriftung "IO48_RGB: WS2812"
an GPIO48, bis dahin von der Firmware nirgends verwendet. Nutzerwunsch:
ein Watchdog, der diese LED Farben wechseln laesst, solange das System
laeuft. **Wichtige Klarstellung im Gespraech**: gemeint ist die
ESP-eigene Firmware/FreeRTOS, NICHT das Betriebssystem des gesteuerten
Host-PCs (das war meine erste - falsche - Annahme und haette ein neues
USB-Kommando plus ein separates PowerShell-Host-Skript gebraucht, das
periodisch heartbeat sendet; nach der Korrektur entfaellt beides
komplett).

### Architektur: zwei Wirkungsebenen, nicht nur Deko

Neue Komponente `watchdog_manager`. Eine einzelne, niedrig priorisierte
FreeRTOS-Task (Prioritaet 2 - ueber Idle, aber unter allen
Anwendungs-Tasks) schiebt alle 40ms den HSV-Farbton der LED um 2° weiter
(voller Farbumlauf ≈ 7,2s) und tut zwei Dinge, die zusammen einen
echten Watchdog ergeben statt nur eine Anzeige:

1. **Sichtbares Lebenszeichen**: allein dass diese Task regelmaessig
   drankommt, beweist, dass der Scheduler nicht durch eine andere Task
   in einer Endlosschleife ohne Yield blockiert ist - ein Einfrieren
   waere durch ein Einfrieren der LED sofort sichtbar.
2. **Echte Selbstheilung**: die Task meldet sich zusaetzlich beim
   ESP-IDF-eigenen Task Watchdog Timer an (`esp_task_wdt_add(NULL)`) und
   fuettert ihn jeden Zyklus (`esp_task_wdt_reset()`). TWDT ist per
   ESP-IDF-Standard bereits aktiv (`CONFIG_ESP_TASK_WDT_INIT=y`, 5s
   Timeout, beide Idle-Tasks angemeldet), loest bei einem Timeout aber
   standardmaessig NUR eine Log-Meldung aus
   (`CONFIG_ESP_TASK_WDT_PANIC=n` per Default) - explizit auf
   Panic+Reboot umgestellt (`CONFIG_ESP_TASK_WDT_PANIC=y`, alle drei
   sdkconfig-Dateien). Der `app_main()`-eigene Endlos-Loop (1s-Zyklus,
   siehe `main.c`) ist ebenfalls angemeldet.

**Bewusst begrenzter Umfang** (Nutzer bestaetigte "Beides" auf die
Rueckfrage nach Umfang, aber das heisst nicht *jede* Task): nur die
neue Watchdog-Task selbst und der `app_main()`-Loop sind beim TWDT
angemeldet - beide sind natuerliche, kurzzyklische Kandidaten ohne
unbegrenzt blockierende Aufrufe. Bestehende Dauer-Tasks mit langen
Schlafphasen (`sensor_manager`, 60s-Zyklus) oder unbegrenzt
blockierenden Calls (`ssh_manager`s `accept()` in der Listen-Schleife)
wurden bewusst NICHT angemeldet - das haette entweder eine Restrukturierung
dieser Schleifen gebraucht (z.B. Socket-Timeout auf die Listen-Schleife,
analog zum bereits bestehenden 200ms-Muster in `handle_session()`) oder
haette zu Fehlalarmen gefuehrt. Ein "sauberer" Deadlock in einer dieser
Tasks (blockiert auf einem Mutex/einer Queue, ohne CPU zu belegen) bleibt
dadurch fuer die LED unsichtbar - abgedeckt ist Fall 1 (CPU-Monopolisierung
durch eine andere Task) vollstaendig, Fall 2 (sauberer Einzeltask-Deadlock)
nur fuer main/Watchdog-Task selbst. Ausweitung auf weitere Tasks ist mit
demselben Muster (`esp_task_wdt_add`/`_reset` in der jeweiligen
Schleife) jederzeit trivial nachruestbar, bewusst nicht in dieser Runde.

### Bibliothek: espressif/led_strip, ein echter Bibliotheksfehler gefunden

`espressif/led_strip` (^2.5, aufgeloest zu 2.5.5) ist die offizielle
Managed Component fuer WS2812/SK6812 ueber RMT oder SPI. RMT-Variante
gewaehlt (Standard fuer eine einzelne adressierbare LED, kein DMA
noetig). API sehr klein: `led_strip_new_rmt_device()`,
`led_strip_set_pixel_hsv()`, `led_strip_refresh()` - direkt aus den
heruntergeladenen Headern uebernommen (gleiches Vorgehen wie bei
wolfSSH: Header direkt lesen statt Web-Doku vertrauen).

**Gefundener Fehler**: `src/led_strip_spi_dev.c` (Teil der Komponente,
wird unabhaengig davon kompiliert, ob wir RMT statt SPI nutzen - die
eigene CMakeLists.txt der Komponente bietet kein Kconfig-Flag zum
Abschalten des SPI-Backends) verwendet
`MALLOC_CAP_DEFAULT`/`MALLOC_CAP_INTERNAL`/`MALLOC_CAP_DMA`/
`heap_caps_calloc`, bindet aber `esp_heap_caps.h` nirgends ein - baut
nicht gegen unsere Toolchain (GCC 15.2.0). Echter, kleiner
Bibliotheksfehler (Version 2.5.5 ist die aktuellste im Registry, kein
Fix im Changelog erwaehnt). Gleiches Patch-Muster wie beim
wolfssl-`thread_local`-Fix: idempotenter CMake-Schritt in
`firmware/CMakeLists.txt` (fuegt `#include "esp_heap_caps.h"` ein,
falls noch nicht vorhanden) statt Hand-Patch, da der Component-Manager
`managed_components` bei jeder Neukonfiguration aus dem Cache
wiederherstellt und einen einmaligen Patch sonst wieder umwuerfe.

### Nebenbefund: GCC-Interner-Compiler-Fehler unter Volllast (nicht code-bezogen)

Ein sauberer `rm -rf .pio/build` + paralleler Build brach zweimal in
Folge deterministisch am selben Punkt mit einem GCC-Absturz
("internal compiler error: Segmentation fault", "during RTL pass: ira")
in `esp_lcd_panel_rgb.c` ab - einer Espressif-Kerndatei fuer den
RGB-LCD-Treiber, den dieses Projekt gar nicht nutzt (kein LCD verbaut).
Nichts in unserem Code oder unseren Abhaengigkeiten fordert diese Datei
an - PlatformIOs ESP-IDF-Integration baut anders als `idf.py` grosszuegig
den gesamten Komponentenbaum mit, unabhaengig von tatsaechlicher
Verwendung. Ein anschliessender **single-threaded** Build (`pio run -j
1`) lief fehlerfrei durch - der ICE ist also ressourcendruckbedingt
(vermutlich Speicherdruck durch mehrere parallele `cc1`-Prozesse unter
Ninjas Standard-Parallelitaet auf dieser Maschine), kein deterministischer
Code-Fehler. Kein dauerhafter Workaround eingebaut (z.B. eine
Compile-Flag-Absenkung nur fuer diese eine Datei) - das waere Aufwand
fuer ein Problem in Code, den wir nicht nutzen. Falls das erneut
auftritt: `pio run -e <env> -j 1` als Fallback, siehe Projekt-Memory.

### Ergebnis

Real-Hardware-Env baut sauber (nach dem oben beschriebenen
Parallelitaets-Nebenbefund), Flash 60,4 % (1.266.069 Byte, +24.164 Byte
gegenueber dem Host-Key-Fingerprint-Stand - der led_strip-Treiber plus
die neue Task sind deutlich groesser als der reine Fingerprint-Code),
RAM 18,8 % (61.648 Byte). Wokwi-Sim nicht gebaut (weiterhin auf
Nutzerwunsch pausiert). Nicht auf echter Hardware getestet - wie
ueblich bis ein Board vorhanden ist.

## 2026-07-17 — Benachrichtigungswege: Syslog + SMTP ohne TLS

Pflichtenheft Abschnitt 12, letzter noch offener Punkt der urspruenglichen
Entscheidungsliste ("Versandweg Benachrichtigungen"): Kosten/Nutzen von
fuenf Kandidaten grob geschaetzt (SNMP-Trap, Syslog, MQTT, HTTPS-Webhook,
SMTP+TLS) - Kernbefund dabei: `esp-tls` (ESP-IDFs eigene HTTP/MQTT-Client-
Infrastruktur) hat in dieser ESP-IDF-Version **kein wolfSSL-Backend**, nur
mbedTLS - jeder TLS-Weg ueber die Standard-Clients haette also einen
**zweiten, vollstaendigen Krypto-Stack** neben dem schon vorhandenen
wolfSSL (SSH, siehe "SSH-Server (P7)") gebraucht, grob geschaetzt +100-200
KB. Entscheidung: **Syslog (UDP) + SMTP OHNE TLS** - beide brauchen keinen
Krypto-Stack, bewusster Kompromiss (Klartext-SMTP funktioniert nur mit
einem vertrauenswuerdigen internen Relay/Smarthost, nicht mit oeffentlichen
Providern wie Gmail, die TLS erzwingen).

### Architektur

Neue Persistenz in `notification_manager` (`/storage/notify.json`,
gleiches JSON-Muster wie config_manager/snmp_manager): Syslog-Server+Port
und SMTP-Server+Port+Absender+Benutzername+Passwort, ueber die
Einstellungen-Seite (Verwalter+) konfigurierbar, neue Karte
"Benachrichtigungen". Beide Wege unabhaengig optional (leerer Server =
aus). SMTP-Passwort wird der Einstellungen-Seite **nie** zurueckgegeben
(`notification_manager_get_smtp()` liefert es bewusst nicht) - ein leeres
Passwort-Feld im Formular bedeutet "unveraendert lassen"
(`notification_manager_set_smtp()` uebernimmt ein leeres Passwort nicht).

E-Mail-Empfaenger sind Sache der einzelnen Benutzerkonten, nicht der
globalen Konfiguration (Nutzerwunsch: "E-Mail Adresse speichern wir bei
jedem Benutzer, mit einer Checkbox Benachrichtigung aktiv") -
`user_manager` gewann zwei neue Felder (`email`, `notify_enabled`),
Selbstbedienung auf der Uebersichtsseite (neue Karte
"E-Mail-Benachrichtigung", `/account/notify`) analog zum SSH-Key, aber
OHNE Rollenbeschraenkung (jede Rolle darf ihre eigene Adresse pflegen).
Eine aktivierte Benachrichtigung ohne (plausible) Adresse wird abgelehnt
(`looks_like_email()` - nur eine Tippfehler-Bremse, keine vollstaendige
RFC-5322-Pruefung).

### SMTP-Client: von Hand ueber einen rohen TCP-Socket

Kein fertiger SMTP-Client in ESP-IDF - Protokollablauf (EHLO, optional
AUTH LOGIN, MAIL FROM, RCPT TO, DATA, QUIT) von Hand implementiert.
Antworten koennen mehrzeilig sein (250-erste\r\n250 letzte\r\n) - am 4.
Zeichen einer Zeile erkennbar ('-' = es folgt noch eine Zeile, ' ' =
letzte). AUTH LOGIN braucht Base64 (neuer, eigener Encoder - der aus
ssh_manager.c wird nicht geteilt, siehe die dortige Begruendung fuer
bewusste kleine Duplizierung statt eines gemeinsamen Utility-Moduls).
`getaddrinfo()` (neu fuer dieses Projekt) loest sowohl Hostnamen als auch
rohe IP-Adressen auf, gemeinsam fuer Syslog (UDP) und SMTP (TCP)
verwendet.

### Alarm-Flut verhindert: flankengetriggert statt Zyklus-getriggert

Vorher loeste `notification_manager_trigger()` nur ein Log-Statement aus
- eine Wiederholung alle 60s (SensorTask-Zyklus) waehrend einer
anhaltenden Schwellwert-Ueberschreitung war harmlos. Mit echtem Versand
waere das eine E-Mail/Syslog-Nachricht alle 60s auf unbestimmte Zeit -
ein echtes Problem, das beim Bauen aufgefallen ist (nicht explizit vom
Nutzer verlangt, aber eine notwendige Ergaenzung fuer sinnvolles
Verhalten). Fix in `sensor_manager.c`: `check_threshold()` bekam einen
`was_breached`-Zustand pro Messgroesse (NTC-Temperatur/DHT11-Temperatur/
DHT11-Luftfeuchte je eigenes `static bool`) - ausgeloest wird nur beim
Uebergang von "unter Schwellwert" zu "drueber", nicht bei jedem Zyklus
waehrend der Wert oben bleibt. Wird erst wieder scharf, wenn der Wert
zurueck unter den Schwellwert faellt.

### Synchron/blockierend, kein eigener Versand-Task

SMTP-Versand (TCP-Connect + mehrere Roundtrips, 5s-Timeout pro
Socket-Operation) laeuft synchron im aufrufenden SensorTask - blockiert
dessen 60s-Zyklus fuer die Dauer des Versands (mehrere Sekunden bei
mehreren Empfaengern). Bewusst keine eigene Versand-Queue/-Task dafuer
gebaut - der SensorTask hat ohnehin ein 60s-Intervall, ein paar Sekunden
Verzoegerung beim Alarmversand sind unkritisch, die Komplexitaet einer
asynchronen Warteschlange stand in keinem Verhaeltnis zum Nutzen hier.

### `was loggen.txt`-Luecke geschlossen

"alert temp dht"/"alert temp 10K B3590" waren in `was loggen.txt` schon
lange gefordert, aber nie ans Audit-Log angebunden (nur `ESP_LOGW`,
keine Persistenz) - `notification_manager_trigger()` ruft jetzt zusaetzlich
`audit_log_add()` auf, unabhaengig davon ob ueberhaupt ein Versandweg
konfiguriert ist.

### Ergebnis

Real-Hardware-Env baut sauber nach zwei Format-Truncation-Fixes
(`notify_card`-Puffer 512->640 Byte, Einstellungen-Seiten-Puffer
9216->12288 Byte - beide durch die zusaetzlichen Formularfelder
ausgeloest, gleiche Ursache wie die wiederholt aufgetretenen
format-truncation-Warnungen bei fruaheren Formular-Ergaenzungen). Flash
60,8 % (1.275.217 Byte, +9.148 Byte gegenueber dem Watchdog-LED-Stand),
RAM 19,2 % (63.024 Byte). Nicht auf echter Hardware/gegen einen echten
SMTP-/Syslog-Server getestet - wie ueblich bis ein Board vorhanden ist.

Bewusst nicht umgesetzt:
- Kein STARTTLS-Upgrade-Pfad fuer SMTP (bewusste Entscheidung gegen
  TLS, siehe oben - ein Nachruesten waere ein groesserer Umbau, kein
  kleiner Zusatz).
- Kein Dot-Stuffing in der SMTP-DATA-Phase (falls eine Zeile mit "."
  beginnt, wuerde das DATA vorzeitig beenden) - fuer die aktuell einzige,
  programmatisch erzeugte, garantiert nicht mit "." beginnende
  Alarmzeile kein praktisches Risiko, waere aber vor einer Erweiterung
  auf freien Nutzertext noetig.
- Keine Wiederholungsversuche bei fehlgeschlagenem Versand (Verbindung
  down, Server nicht erreichbar) - ein Fehlschlag wird geloggt, nicht
  erneut versucht.

### Nachtrag 2026-07-17 (spaeter): E-Mail-Entprellung (Zaehler + Timer) + Sammel-Mail statt Mail pro Empfaenger

Auf Nutzerhinweis: viele SMTP-Server/Relays begrenzen auf ca. 10
Mails/Stunde - ohne Bremse haette eine Haeufung von Schwellwert-Alarmen
(mehrere Messgroessen kurz hintereinander, trotz der Flankentriggerung
aus dem vorherigen Eintrag) dieses Kontingent schnell aufgebraucht.
Zwei Aenderungen:

**1. Eine Mail fuer alle Empfaenger statt eine Mail pro Empfaenger.**
`smtp_send_all()` ersetzt das bisherige `smtp_send_one()`-in-einer-
Schleife-Muster: eine einzige SMTP-Verbindung, ein `MAIL FROM`, aber ein
`RCPT TO` je aktiviertem Empfaenger (Umschlag-Ebene - so liefert der
Server trotzdem an jeden einzeln aus), und alle Adressen sichtbar im
`Cc:`-Header (`To:` zeigt den Absender selbst, da kein einzelner
Haupt-Empfaenger vorgesehen ist). Ein einzelner abgelehnter Empfaenger
(z.B. Tippfehler in der Adresse) bricht nicht den gesamten Versand ab,
wird nur geloggt - die anderen RCPT TO/die Mail selbst laufen weiter.
Reduziert das Mail-Aufkommen pro Alarm sofort von N (eine je Empfaenger)
auf 1, unabhaengig von der Entprellung unten.

**2. Zaehler + Zeitfenster, mit einer Timer-Task gekoppelt (Nutzervorgabe
woertlich: "counter mit einem timer koppeln").** Neuer Zustand in
`notification_manager.c`: ein 60-Minuten-Fenster (`s_window_start_us`),
ein Zaehler bereits versendeter Mails darin, ein "sammelt gerade"-Schalter
und ein Text-Puffer fuer gesammelte Meldungen (1 KB, mit
Ueberlauf-Zaehler statt Absturz/Abschneiden ohne Hinweis). Ablauf pro
Alarm (`notify_email()`):
- Die ersten 4 Mails eines Fensters gehen sofort raus (`smtp_send_all()`
  direkt aufgerufen).
- Wird eine 5. Mail noetig, **und** das Fenster ist noch keine 50 Minuten
  alt: ab jetzt wird gesammelt statt einzeln versendet
  (`append_to_digest()`) - keine weitere Mail, bis der Digest verschickt
  wird.
- Wird eine 5. Mail erst NACH Minute 50 noetig: das Fenster ist ohnehin
  fast vorbei, normal sofort senden (kein Sammeln fuer die letzten paar
  Minuten eines Fensters).
- Waehrend gesammelt wird, landet jede weitere Meldung nur noch im
  Digest-Puffer, nie als Einzelmail.

Der eigentliche **Timer**-Teil: eine neue leichte FreeRTOS-Task
(`digest_flush_task`, Prioritaet 1, alle 30s Poll) prueft unabhaengig von
neuen Alarmen, ob Minute 59 des Fensters erreicht ist, und verschickt
dann genau eine Sammel-Mail mit allen bis dahin aufgelaufenen Meldungen
(`flush_digest()`). Das ist bewusst NICHT rein ereignisgetrieben (nur
beim naechsten Alarm pruefen) - sonst wuerde der Digest nie verschickt,
falls nach dem Start des Sammelns zufaellig kein weiterer Alarm mehr
eintrifft (z.B. genau 5 Alarme, dann bleibt alles ruhig - ohne die
Timer-Task blieben die gesammelten Meldungen fuer immer unversendet im
RAM liegen). Maximal 5 Mails pro Fenster im Extremfall (4 sofort + 1
Digest), komfortabel unter dem 10/Stunde-Server-Limit - selbst mit
Reserve fuer den Fall, dass der Server sein Fenster nicht exakt
deckungsgleich mit unserem 60-Minuten-Fenster zaehlt.

Syslog ist von der Bremse bewusst ausgenommen (UDP, kein Server-
Kontingent-Problem, "fire and forget") - jeder Alarm erzeugt weiterhin
sofort ein Syslog-Paket, unabhaengig vom E-Mail-Sammelzustand.

**Nebenaenderung, durch die Sammel-Mail noetig geworden**: der SMTP-Body
konnte bisher nur eine einzelne Zeile sein (`smtp_send_line()` direkt
mit dem ganzen Body aufgerufen). Eine Sammel-Mail hat mehrere
Zeilen (eine je gesammelter Meldung) - neue `smtp_send_body_lines()`
zerlegt den Body an "\n" und sendet jede Zeile einzeln mit korrektem
CRLF, statt den mehrzeiligen Text als eine "Zeile" misszuverstehen.

Ergebnis: Real-Hardware-Env baut sauber (ein GCC-ICE-Ausreisser beim
parallelen Clean-Build, siehe "Watchdog-LED"-Eintrag - Retry lief
sauber durch), Flash 60,9 % (1.277.137 Byte, +1.920 Byte gegenueber dem
vorherigen Benachrichtigungswege-Stand), RAM 19,5 % (64.048 Byte).
Nicht auf echter Hardware getestet.

## 2026-07-18 — Portierbarkeit auf ESP32-S3-DevKitC-1-N8R8

Auf Nutzerfrage: passt der aktuelle Funktionsumfang groessenmaessig auch
auf die kleinere N8R8-Variante des Boards (8 statt 16 MB Flash, PSRAM bei
beiden identisch 8 MB - nur das "N16"/"N8"-Praefix unterscheidet sich)?

Kurz nachgerechnet statt geraten: `partitions.csv` summiert sich auf
`0x510000` Byte ≈ 5,06 MB (nvs 20K + otadata 8K + 2× 2-MB-OTA-Slot + 1-MB-
Storage) - passt unveraendert unter 8 MB (`0x800000`), es bleiben ≈2,94 MB
ungenutzt (gegenueber ≈10,9 MB auf der 16-MB-Variante). Die einzelne
App-Partition (2-MB-OTA-Slot, aktuell 60,9 % ausgelastet) aendert sich
durch den Boardwechsel gar nicht - deren Groesse haengt nur von der
Partitionstabelle ab, nicht vom Gesamt-Flash. PSRAM-Konfiguration
(`CONFIG_SPIRAM_MODE_OCT` etc.) bleibt unveraendert gueltig, da N8R8
dasselbe Octal-8-MB-PSRAM hat wie N16R8.

**Entscheidung**: neue PlatformIO-Env `esp32-s3-devkitc-1-n8r8` angelegt,
die ab jetzt parallel zur bestehenden N16R8-Env mitgebaut/mitgepflegt
wird (kein einmaliger Wegwerf-Test) - kein eigenes N8R8-Board vorhanden,
genau wie beim P1-WireGuard-Spike geht es hier nur um
Kompilierbarkeit/Fussabdruck-Tracking, nicht um einen tatsaechlichen
Flash-Vorgang. `platformio.ini`: `extends = env:esp32-s3-devkitc-1-n16r8`
mit `board_upload.flash_size = 8MB` ueberschrieben - `partitions.csv`
selbst bleibt unangetastet (identisch fuer beide Boardgroessen). Die
eigentliche Flashgroessen-Kconfig-Auswahl (`CONFIG_ESPTOOLPY_FLASHSIZE_*`
ist ein Kconfig-"choice", nur eine Option kann aktiv sein) wird ueber
eine neue `sdkconfig.defaults.esp32-s3-devkitc-1-n8r8` auf 8 MB
umgeschaltet - gleiches Merge-Muster wie `sdkconfig.defaults.wokwi-sim`
(PlatformIO fuegt `sdkconfig.defaults.<env>` zusaetzlich zur
gemeinsamen `sdkconfig.defaults` hinzu, kconfgen versteht Choice-Gruppen
beim Zusammenfuehren korrekt - kein manuelles "is not set" fuer die
16-MB-Option noetig).

Ergebnis: baut sauber, Flash 60,9 % (1.277.089 Byte), RAM 19,5 % (64.048
Byte) - praktisch identisch zur N16R8-Env (die winzige Differenz kommt
vermutlich aus einer flashgroessenabhaengigen Konstante irgendwo im
Bootloader/Linkerskript). Drei Envs jetzt insgesamt:
`esp32-s3-devkitc-1-n16r8` (echtes Board), `esp32-s3-devkitc-1-n8r8`
(kein Board, nur Fussabdruck-Tracking), `wokwi-sim` (Simulation, nur
nach Nutzerfreigabe). **Standing Convention ab jetzt**: bei jedem
"echten Build zur Verifikation" beide Nicht-Wokwi-Envs bauen, nicht nur
n16r8 - siehe Projekt-Memory.

### Nachtrag 2026-07-18: automatische Varianten-Erkennung vor dem Flashen

Auf Nutzerfrage: kann man vor dem Flashen abfragen, welche Hardware-
Variante (N16R8 vs. N8R8) tatsaechlich angeschlossen ist, statt sich auf
einen von Hand gewaehlten Parameter zu verlassen? Ja - `esptool.py
flash_id` spricht den ROM-Bootloader direkt an (kein vorheriges
Firmware-Image noetig, funktioniert also auch auf einem leeren/
unbekannten Chip) und gibt u.a. eine Zeile "Detected flash size: 8MB"
aus - exakt dasselbe String-Format ("8MB"/"16MB") wie
`board_upload.flash_size` in `platformio.ini`, praktischerweise direkt
weiterverwendbar ohne Umrechnung. PSRAM-Groesse laesst sich dagegen NICHT
vorab abfragen (das ist reine Laufzeit-/Firmware-Erkennung beim Boot,
kein ROM-Bootloader-Feature) - spielt hier aber keine Rolle, da N16R8
und N8R8 identisches PSRAM haben (nur die Flash-Groesse unterscheidet
sich, siehe Eintrag oben).

Neue Funktionen in `tools/EspBmcLink.psm1`: `Get-EspBmcFlashSize` (ruft
`~/.platformio/packages/tool-esptoolpy/esptool.py flash_id` ueber
PlatformIOs eigenes venv-Python auf, `-Port` optional - ohne Port sucht
esptool selbst, gleiches Auto-Erkennungsverhalten wie `pio run -t
upload`, das ebenfalls nie einen expliziten Port braucht) und
`Resolve-EspBmcEnvironment` (bildet die erkannte Groesse auf den
passenden Env-Namen ab, `$null` bei einer unbekannten Groesse).

`tools/Setup.ps1` nutzt beides vor dem Flash-Schritt: wurde `-EnvName`
NICHT explizit vom Nutzer angegeben, wird automatisch auf die erkannte
Umgebung umgeschaltet. Wurde `-EnvName` explizit angegeben, wird das
respektiert (nur eine Warnung bei einer Abweichung, kein Ueberschreiben -
der Nutzer koennte bewusst einen Sonderfall testen wollen). Schlaegt die
Erkennung fehl (kein Geraet verbunden, esptool.py nicht gefunden,
unbekannte Chipgroesse) wird nur gewarnt und mit dem bisherigen
Standardverhalten fortgefahren - rein informativ/best-effort, kein
Abbruch des Flash-Vorgangs.

PowerShell-Syntax geprueft (Parser-Check, kein Laufzeitfehler), und die
esptool-Kommandozeile (`esptool.py --port <port> flash_id`,
`DETECTED_FLASH_SIZES`-Werteformat "8MB"/"16MB") direkt gegen die im
Projekt gebundelte esptool.py-Version (v4.11.0) verifiziert statt nur
angenommen. Nicht gegen echte Hardware getestet - kein Board vorhanden.

## 2026-07-18 — Verdrahtungsplan final + Pinbelegung fixiert

Auf Nutzerwunsch: kompletter Neuaufbau von `docs/verdrahtungsplan.html`
(vorher ein 68-KB-Base64-Foto-Overlay mit generischen "ESP-Eingang"/
"ESP-Ausgang"-Beschriftungen ohne echte Pin-Nummern) - jetzt ein reines
SVG/HTML-Dokument (keine eingebetteten Fotos mehr, 42 statt 93 KB,
deutlich wartbarer), zwei interaktiv umschaltbare Ausbaustufen fuer die
Taster-Weiterleitung, vollstaendige 44-Pin-Referenztabelle, USB-Port-
Zuordnung, ATX-+5VSB-Alternativversorgung, Dual-Powering-Warnung.

### Pinbelegung final: die bisherigen Wokwi-Platzhalter waren schon korrekt

Cross-Check der 10 bereits im Code verwendeten GPIOs (`gpio_manager.h`:
4/5/6/7/15/16/17/18, `sensor_manager.h`: 1/2, plus GPIO48 fuer die
Watchdog-LED) gegen die Vendor-Pinout-Bilder und die ESP32-S3-
Strapping-/JTAG-/UART-/USB-Pins (0, 3, 39-46) ergab: **keine einzige
Kollision** - die urspruenglich "AUSDRUECKLICH PROVISORISCH" nur fuer
Wokwi gedachten Werte waren die ganze Zeit schon gueltig fuer das echte
Board. Kommentare in beiden Headern entsprechend aktualisiert (nicht
mehr "Platzhalter", sondern "final festgelegt, gegen Vendor-Bilder +
ESP-IDF-SoC-Header abgeglichen, noch nicht auf Hardware verifiziert").
Schliesst den letzten offenen Punkt aus Pflichtenheft Abschnitt 12
("GPIO-Pinbelegung ... noch offen").

### USB-Port-Zuordnung + ein gefundener Vendor-Beschriftungsfehler

Port-Zuordnung (welcher der beiden USB-C-Anschluesse Flash/UART vs.
natives USB ist) nur mit **mittlerem Vertrauen** aus Fotos bestimmt (Lage
des Bridge-Chips relativ zu den Ports auf `docs/board-foto.jpg`) - explizit
im Dokument als vor dem ersten Einsatz zu verifizieren markiert, kein
Silkscreen-Aufdruck gefunden, der es eindeutig bestaetigt.

Waehrend der Recherche ein echter Fehler in der Vendor-Pinout-Grafik
gefunden: die Bilder beschriften `GPIO21=USB_D+, GPIO20=USB_D-` - ein
Blick in ESP-IDFs eigenen SoC-Header
(`components/soc/esp32s3/include/soc/usb_pins.h`:
`USBPHY_DP_NUM=20, USBPHY_DM_NUM=19`) zeigt, dass das vertauscht/falsch
ist: D+ ist tatsaechlich GPIO20, D- ist GPIO19, und GPIO21 hat mit
nativem USB gar nichts zu tun (bleibt frei nutzbar). Der ESP-IDF-Header
ist hier die verlaessliche Quelle (fest im Silizium verdrahtet, nicht
vendor-abhaengig) - im Dokument als Korrektur zur Vendor-Beschriftung
festgehalten, nicht die Fotos blind uebernommen. Gleiches Prinzip wie
schon bei der Sensormeter-Familie ("GPIO16 IST auf dem Header ... Datenblatt
statt Foto-Lesen").

### Sicherheitsluecke geschlossen: Spannungsteiler fuer LED-Erfassung

Lastenheft 10.1 nannte fuer die Power-/HDD-LED-Erfassung "kein
zusaetzliches Bauteil" - das stimmt nur, wenn der Mainboard-Header
sicher auf 3,3 V referenziert ist. Viele Mainboards ziehen ihre
Front-Panel-LED-Header aber auf 5 V hoch, was den absoluten Grenzwert
eines ESP32-S3-GPIO (3,3 V + 0,3 V) ueberschreiten kann - ein bislang
unbemerkter Luecke (in `docs/bom.md` schon als "noch offen/zu klaeren"
vermerkt, aber nie geschlossen). Jetzt ergaenzt: 10 kΩ/20 kΩ-
Spannungsteiler pro Kanal, ergibt 3,33 V bei 5 V Eingang (sicherer
Abstand zum 3,6-V-Abs.-Max.), bei einem bereits 3,3-V-referenzierten
Header weiterhin unschaedlich (liefert dann nur ~2,2 V, noch sicher als
"High" erkennbar). `docs/bom.md` entsprechend erweitert (#6b).

### Zwei Ausbaustufen fuer die Taster-Weiterleitung, "mit Optokoppler" empfohlen

Lastenheft 10.3 fordert PC817 auf beiden Weiterleitungskanaelen -
trotzdem beide Varianten dokumentiert (Nutzerwunsch), mit klarer
Kennzeichnung der optokoppler-Variante als empfohlen, nicht
gleichwertig:
- **Ohne Optokoppler**: NPN-Transistor (BC547/2N2222) + 1-kΩ-
  Basiswiderstand pro Kanal als einfacher Schalter - bewusst kein
  blanker GPIO-Draht direkt zum Header (5-V-Ueberspannungsrisiko wie
  beim LED-Sense-Fall oben), aber Mainboard-GND und ESP-GND werden dabei
  elektrisch verbunden, keine Trennung.
- **Mit Optokoppler (empfohlen)**: PC817 pro Kanal (deckt sich mit
  `docs/bom.md` #5/#6), Fototransistor-Seite ueberbrueckt die
  Mainboard-Header-Pins komplett getrennt vom ESP-GND.
- Begruendung fuer die Empfehlung ueber die reine Lastenheft-Vorgabe
  hinaus: die Trennung bleibt auch dann sinnvoll, wenn ESP und Mainboard
  ohnehin dieselbe Stromquelle teilen (ATX +5VSB desselben PCs, siehe
  unten) - sie begrenzt einen Verdrahtungsfehler auf einen 30-Cent-
  Optokoppler statt auf den GPIO oder einen daran haengenden
  USB-Host-Rechner.

### Alternative Stromversorgung: ATX +5VSB + Dual-Powering-Warnung

Neue Anforderung (nicht vorher in Lasten-/Pflichtenheft), damit der
ESP-BMC auch bei vollstaendig ausgeschaltetem PC weiterlaeuft (Voraus-
setzung fuer Fern-Einschalten ueberhaupt). ATX-24-Pin Pin 9 (+5VSB,
violett) + eine COM/GND-Ader (schwarz) auf die Header-Pins `5Vin`/`GND`
- nicht ueber einen USB-Port.

Prominent herausgestellt: **Dual-Powering-Gefahr**. 5Vin und beide
USB-VBUS-Anschluesse liegen intern auf demselben 5V-Netz - zwei aktive
Quellen gleichzeitig (z.B. ATX +5VSB an 5Vin UND ein USB-Kabel zu einem
eingeschalteten Host gleichzeitig an einem der beiden Ports) speisen
unkoordiniert ein, ohne Power-ORing-Schaltung kann Strom in die jeweils
andere Quelle zurueckfliessen - moegliche Folgen: beschaedigter
USB-Host-Controller, beschaedigte ATX-Standby-Regelung, oder eine
undefinierte Spannung auf dem Board-eigenen 5V-Netz. Regel im Dokument:
nie beide gleichzeitig, USB-Kabel vor dauerhaftem ATX-Betrieb abziehen;
eine Schottky-Dioden-ODER-Schaltung als Option genannt, aber bewusst
nicht mitgeliefert (kein bekannter Anwendungsfall dafuer).

### Ergebnis

`docs/verdrahtungsplan.html` neu aufgebaut (JS-Syntax mit `node -e`
geprueft, HTML-Tag-Balance verifiziert - beides nach einem echten Fund:
ein erster Entwurf hatte drei JS-String-Literale mit einem
unausgewichenen ASCII-Anfuehrungszeichen mitten im String, das den
String vorzeitig beendet und einen Syntaxfehler ausgeloest haette,
gefixt durch passende typografische Anfuehrungszeichen). `gpio_manager.h`/
`sensor_manager.h`-Kommentare aktualisiert, `docs/bom.md` um die zweite
Ausbaustufe + Spannungsteiler + ATX-Anzapfkabel erweitert,
`docs/projektstand.md`s GPIO-Pinbelegungs-Zeile auf entschieden
umgestellt. Beide Firmware-Envs (n16r8 + n8r8, neue Standing Convention)
nach den reinen Kommentar-Aenderungen sauber neu gebaut. Nicht auf
echter Hardware getestet - kein Board vorhanden.

## 2026-07-18 — OTA-Update im Webinterface, Identitaets-/Downgrade-Pruefung wie bei der Sensormeter-Familie

Auf Nutzerwunsch: lokaler .bin-Upload ueber die Einstellungen-Seite,
Admin-only, mit demselben Identitaets-/Downgrade-Pruefungsmuster wie die
Sensormeter-Familie (`OtaManager.cpp`/`.h` dort) - Implementierung dort
direkt studiert statt aus der Erinnerung rekonstruiert, um den dort
bereits gefundenen und behobenen Fehler von vornherein zu vermeiden statt
ihn erst selbst wiederzuentdecken.

### Erste Beta: Versionsnummer wie bei sm

Neue Datei `firmware/components/firmware_version/include/firmware_version.h`:
`FIRMWARE_PROJECT_ID "ESP-BMC"`, `DEVICE_FIRMWARE_VERSION "0.9.0-rc4"` -
exakt dieselbe Versionsnummer wie die Sensormeter-Familie aktuell
(Nutzervorgabe: "diese Firmware wird unsere erste Beta, gleiche Version
wie bei sm setzen") - beide Projekte bleiben trotzdem unabhaengig
versioniert, keine gemeinsame Release-Klammer, nur zufaellig derselbe
Stand. Eigene kleine Komponente statt eines Headers unter `main/`, damit
`ota_manager` (siehe unten) sie ohne unschoenen relativen
Include-Pfad/Zirkelbezug zu `main` erreichen kann.

### Marker-Format bewusst NICHT deckungsgleich mit Sensormeter

Sensormeter nutzt "SM-FW-ID:...:SM-FW-END". Fuer ESP-BMC bewusst ein
eigenes, nicht-ueberlappendes Format gewaehlt: "ESPBMC-FW-ID:...:ESPBMC-FW-END".
Grund: "FW-ID:" waere als generischer, kuerzerer Praefix ein Teilstring
von Sensormeters "SM-FW-ID:" gewesen (ebenso ":FW-END" ein Teilstring von
":SM-FW-END") - ein Scan nach dem kuerzeren Muster haette in einer
tatsaechlichen Sensormeter-.bin faelschlich einen Treffer gefunden und
"SENSORMETER" als (falsche) Projekt-ID eingelesen, was die ganze
Schutzfunktion (Verwechslung zwischen Schwesterprojekten verhindern)
untergraben haette. Mit den vollstaendig unterschiedlichen Praefixen/
Suffixen ist das ausgeschlossen (keine Teilstring-Beziehung in beide
Richtungen, nachgeprueft).

### Der eigentliche Punkt: von Anfang an byte-sicher, nicht erst nach einem eigenen Vorfall

Sensormeters `OtaManager` scannt den Byte-Stream waehrend des Uploads
nach dem Marker - urspruenglich mit `String::indexOf()` (Arduino), das
intern auf `strstr()` basiert und am ersten eingebetteten Null-Byte
abbricht. Eine echte ESP32-.bin enthaelt bereits ab Byte 9 (im
Image-Header) Null-Bytes - der Marker wurde dadurch in der Praxis NIE
gefunden, jeder echte Firmware-Upload wurde faelschlich als "kein
Erkennungsmerkmal" abgelehnt, bis der Fehler bei Sensormeter gefunden und
auf einen handgeschriebenen `memcmp`-basierten `findBytes()`
umgeschrieben wurde (siehe deren docs/entscheidungen.md).

Fuer ESP-BMC direkt mit dem korrigierten Muster gebaut (`find_bytes()` in
`ota_manager.c`, `memcmp`-basiert, byte-sicher) - kein `strstr()`/keine
C-String-Funktion kommt beim Marker-Scan ueberhaupt in Beruehrung mit den
Binaerdaten. Gleiches Tail-Puffer-Prinzip wie Sensormeter (Praefix/Suffix
koennen an einer Chunk-Grenze zerschnitten sein, `s_tail_buf`/
`s_capture_buf`).

### Downgrade-Vergleich: identisches a.b.c[-rcN]-Schema

`compare_versions()`/`parse_version()` in `ota_manager.c` sind eine
direkte C-Portierung von Sensormeters `compareVersions()`/
`parseVersion()` (kein vollstaendiger Semver-Parser, kein Build-
Metadaten-"+...", deckt aber das hier genutzte Schema ab) - bei
gleicher a.b.c-Kernversion hat "kein Suffix" Vorrang vor "mit Suffix"
(ein Release gilt als neuer als jede eigene Vorabversion), bei zwei
Suffixen entscheidet der lexikografische Vergleich ("rc3" < "rc4").

### ESP-IDF-natives OTA statt Arduino-`Update`-Bibliothek

Sensormeter nutzt Arduino-ESP32s `Update`-Klasse
(`Update.begin()/write()/end()`). ESP-BMC ist reines ESP-IDF - direktes
Aequivalent ist `esp_ota_ops.h`
(`esp_ota_get_next_update_partition()`, `esp_ota_begin()`,
`esp_ota_write()`, `esp_ota_end()`, `esp_ota_abort()`,
`esp_ota_set_boot_partition()`) - nutzt dieselbe `ota_0`/`ota_1`-
Partitionsstruktur, die seit der P0-Partitionstabellen-Entscheidung schon
vorhanden, aber nie tatsaechlich verdrahtet war.

### Kein eingebauter Multipart-Parser in esp_http_server - von Hand geloest

Sensormeter nutzt ESPAsyncWebServer, das Multipart-Formulare eingebaut
zerlegt (inkl. eines Textfelds - der "Downgrade erzwingen"-Checkbox - VOR
dem Datei-Feld im selben Formular). ESP-IDFs `esp_http_server` bietet das
nicht. Zwei Konsequenzen:
- Multipart-Header-Block (bis zur ersten Leerzeile) und der
  abschliessende Boundary-Trenner am Dateiende werden von Hand erkannt
  (`settings_ota_upload_post_handler` in `web_server_manager.c`) - fuer
  den Trenner dasselbe Tail-Puffer-Prinzip wie beim Marker-Scan (kann an
  einer Chunk-Grenze zerschnitten sein), damit die Boundary-Bytes NICHT
  versehentlich mit in die OTA-Partition geschrieben werden.
- Statt einer Checkbox im selben Multipart-Body (haette eine
  zusaetzliche Feld-Extraktion vor dem Datei-Feld gebraucht) gibt es zwei
  getrennte `<form>`-Elemente auf der Einstellungen-Seite - "Downgrade
  erzwingen" kommt als Query-Parameter
  (`/settings/ota/upload?force_downgrade=1`) auf der zweiten
  Formular-Action. Bewusster Komplexitaets-/Sicherheits-Tradeoff: minimal
  schlechtere UX (Datei muss im zweiten Formular erneut ausgewaehlt
  werden), aber kein zusaetzlicher Multipart-Feld-Parser noetig -
  passt auch zur bisherigen Linie dieses Projekts (server-gerendertes
  HTML, minimales Client-JS).

### Zusatz ueber die reine Sensormeter-Paritaet hinaus: Bootloader-Rollback

Sensormeter implementiert keinen automatischen Rollback bei einer
haengenden/abstuerzenden neuen Firmware. ESP-IDF bietet das nahezu
kostenlos eingebaut (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, per Default
aus) - jetzt aktiviert (alle drei sdkconfig-Dateien). Bootloader markiert
eine frisch geOTAte Firmware als "Pending Verify" und rollt automatisch
auf die vorherige Partition zurueck, falls sie nicht bis zu einem
bestaetigenden Aufruf durchstartet (Absturz, Watchdog-Reset). ESP-BMC
ruft `esp_ota_mark_app_valid_cancel_rollback()` bewusst ganz am Anfang
von `app_main()` auf (kein eigener Health-Check gebaut - "startet
ueberhaupt bis hierher" reicht als Kriterium fuer dieses Projekt).
Klargestellt: das war nicht explizit verlangt, aber ein natuerlicher,
fast kostenloser Zusatz zu einer OTA-Funktion, die ohne ihn ein
bootloop-anfaelliges Firmware-Update ohne Netz und doppelten Boden waere.

### Admin-only, Audit-Log, kein `was loggen.txt`-Pflichteintrag

OTA-Update ist auf `USER_ROLE_ADMIN` beschraenkt (hoehere Schwelle als
der Rest der Einstellungen-Seite, ein Fehlgriff hier ist folgenreicher
als z.B. eine falsche SNMP-Community) - sowohl das Anzeigen der Karte auf
der Einstellungen-Seite als auch der Upload-Endpunkt selbst pruefen das.
`was loggen.txt` verlangt fuer OTA nichts explizit, trotzdem
audit-geloggt (Erfolg mit Versionsnummer, Ablehnung mit Grund) - passt
zur bisherigen Linie dieses Projekts, sicherheitsrelevante Aktionen
grosszuegiger zu loggen als das Minimum.

### Ergebnis

Real-Hardware-Env baut sauber nach zwei Format-Truncation-Fixes
(`event`-Puffer 112->160 Byte, `ota_card`-Puffer 900->1280 Byte - gleiche
wiederkehrende Ursache wie bei jeder bisherigen Formular-Ergaenzung).
Beide Firmware-Envs zusaetzlich mit einem sauberen `rm -rf .pio/build`-
Neubau bestaetigt (n16r8: Flash 61,7 % / 1.293.289 Byte, RAM 19,9 % /
65.280 Byte; n8r8: Flash 61,7 % / 1.294.733 Byte, RAM 19,9 % / 65.296
Byte - praktisch identisch, wie erwartet). Nicht auf echter Hardware/
gegen einen echten OTA-Upload getestet - kein Board vorhanden.

Bewusst nicht umgesetzt:
- Kein Firmware-Signatur-/Secure-Boot-Schutz - der Marker ist wie bei
  Sensormeter nur eine Verwechslungsbremse, kein kryptografischer Schutz.
  Wer physischen/Admin-Zugriff auf das Webinterface hat, koennte
  theoretisch eine praeparierte .bin mit passendem Marker hochladen.
- Kein Fortschrittsbalken/Live-Status waehrend des Uploads (Browser zeigt
  nur den nativen Datei-Upload-Fortschritt, keine eigene Anzeige) -
  waere zusaetzliches Client-JS, passt nicht zur bisherigen Linie.
- Keine automatische Update-Pruefung/kein Download von einem Server -
  weiterhin nur lokaler .bin-Upload, exakt wie bei Sensormeter (dort
  bewusst so entschieden, um den TLS-Client/mbedTLS-Flash-Bedarf zu
  sparen - dieselbe Kosten-/Nutzen-Abwaegung wie bei ESP-BMCs eigener
  Benachrichtigungswege-Entscheidung, Syslog+SMTP ohne TLS).

## 2026-07-18 — SNMP-Firmwareversion + Zabbix-Template

Auf Nutzerwunsch: SNMP soll die Firmwareversion ausgeben, plus ein
Zabbix-Template nach dem Vorbild von `zabbix-template-sensormeter-wlan.yaml`.

### Neues Objekt firmwareVersion (.17)

Neue OID `1.3.6.1.4.1.99999.10.17.0` (String, read-only), Wert kommt aus
`ota_manager_get_version()` (dieselbe Quelle wie die Einstellungen-Seite
und der OTA-Marker, siehe vorheriger Eintrag) - keine eigene
Versions-Kopie, ein einziger Wahrheits-Ursprung
(`firmware_version.h`/`DEVICE_FIRMWARE_VERSION`). An das Ende der
`s_oids[]`-Tabelle angehaengt (muss aufsteigend sortiert bleiben, .17 ist
die bisher hoechste Nummer - passt automatisch). `snmp_manager`
REQUIRES jetzt zusaetzlich `ota_manager` - nach dem Aendern der
CMakeLists.txt REQUIRES-Zeile war wie immer ein `rm -rf .pio/build`
noetig (Component-Manager-Cache-Gotcha, siehe fruehere Eintraege).

### Zabbix-Template: eigener Enterprise-Zweig, eigene Geraetegruppe

`docs/zabbix-template-esp-bmc.yaml` - Struktur/Stil direkt von
`zabbix-template-sensormeter-wlan.yaml` uebernommen (Makros fuer
Community + Schwellwerte, Item-Tags nach Komponente, Delay-Staffelung
nach Aenderungshaeufigkeit: 1h fuer statische Werte, 5m fuer Netzwerk/
Status, 1m fuer Sensoren), aber bewusst NICHT als "IoT Sensoren"-Gruppe
einsortiert - ESP-BMC ist primaer ein PC-Fernsteuerungs-/BMC-lite-Geraet,
Sensorik ist ein Nebenfeature (Lastenheft Abschnitt 4), anders als bei
der reinen Sensormeter-Familie. Eigene Template-Gruppe "ESP-BMC".

Alle 17 SNMP-Objekte als Items abgebildet (inkl. dem neuen
firmwareVersion), direkt gegen `snmp_manager.c`s OID-Tabelle
durchnummeriert und mit einem Python-`yaml.safe_load()`-Check auf
strukturelle Konsistenz geprueft (17 Items = 17 eindeutige OIDs = 17
eindeutige Keys, 25 eindeutige UUIDs insgesamt) - nicht nur "sieht
plausibel aus", sondern nachgezaehlt.

**Ein Struktur-Stolperstein beim ersten Validierungsversuch**: `triggers:`
liegt in Zabbix' YAML-Exportformat auf derselben Ebene wie `templates:`
(direkt unter `zabbix_export`), NICHT verschachtelt innerhalb des
Template-Objekts - beim ersten Python-Check danach gesucht
(`templates[0]['triggers']`) und einen `KeyError` bekommen, obwohl die
YAML-Datei selbst korrekt war (`zabbix_export.triggers`). Korrigiert im
Pruefskript, nicht in der Vorlage - ein Beleg dafuer, dass sich der
"gegen sm-wlan validieren, nicht nur plausibel hinschreiben"-Ansatz
gelohnt hat.

**Bewusst weggelassen**: kein Zabbix-`valuemap` fuer die sechs 0/1-Felder
(vpn.up, network.wlanstatic, status.powerled/hddled/powerkey/resetkey) -
waere fuers Dashboard lesbarer ("An"/"Aus" statt "1"/"0"), aber KEINES
der vier existierenden Sensormeter-Templates nutzt dieses Zabbix-YAML-
Feature, also gab es keine verifizierte Vorlage fuer die exakte Syntax -
freihaendig geraten haette bei einem Fehler den Import der gesamten
Datei brechen koennen. Stattdessen die 0/1-Bedeutung nur in der
Item-`description` dokumentiert, gleiche Einfachheitsstufe wie die
Sensormeter-Vorlagen.

**Keine Zabbix-Aktion fuer powerKey/resetKey SET** - das Template bildet
nur den Lesezustand ab. Ein SNMP-SET-Trigger haette ein zusaetzliches
Schreib-Community-Makro plus eine Zabbix-Action/ein Skript gebraucht -
kein Vorbild dafuer in sm-wlan (das Template hat gar keine
SET-faehigen Objekte), also bewusst ausserhalb des angefragten Scopes
("Vorbild sm-wlan") gelassen.

### Ergebnis

Beide Firmware-Envs sauber neu gebaut (nach dem REQUIRES-Cache-Wipe):
n16r8 Flash 61,8 % (1.295.317 Byte), RAM 19,9 % (65.264 Byte); n8r8
Flash 61,6 % (1.292.053 Byte), RAM 19,9 % (65.280 Byte). Zabbix-Template
mit `yaml.safe_load()` strukturell verifiziert, nicht gegen eine echte
Zabbix-Instanz importiert (keine vorhanden). Nicht auf echter Hardware/
gegen einen echten SNMP-Client getestet - kein Board vorhanden.

## 2026-07-18 — Admin-Guide (HTML + PDF), Vorbild sm-wlan

Auf Nutzerwunsch: `docs/admin-guide.html` neu angelegt (existierte bisher
nicht), Struktur/Druck-CSS direkt von
`sensormeter-wlan/repo/docs/admin-guide.html` uebernommen (Cover-Seite,
nummerierte Abschnitte mit Inhaltsverzeichnis, Callout-Klassen
warn/good/neutral, druckoptimierte Tabellen/Code-Bloecke mit
`page-break-inside: avoid`), aber inhaltlich komplett neu geschrieben und
mit ESP-BMCs eigener Copper/Patina/Indigo-Farbgebung (statt Sensormeters
Navy/Orange) an die bereits etablierte Optik von
`docs/verdrahtungsplan.html`/`implementierungsplan.html` angeglichen -
eigene Bildsprache, kein blindes Re-Branding der Vorlage.

### Bewusste Abweichungen vom Vorbild (kein 1:1-Nachbau)

- **Kein OLED-Skizzen-Abschnitt** - ESP-BMC hat kein Display, die
  gesamte CSS-Maschinerie fuer die OLED-Bildschirm-Sketches (`.device`,
  `.screen`, `.oled-text`) aus der Vorlage wurde konsequent weggelassen
  statt leer mitgeschleppt.
- **Kein AP-Fallback-Abschnitt** - anders als Sensormeter WLAN hat
  ESP-BMC keinen eigenen Access-Point-Ersteinrichtungsmodus (im Code
  bestaetigt: `network_manager.c` kennt kein `WIFI_MODE_AP`) - die
  Ersteinrichtung laeuft komplett ueber USB (`tools/Setup.ps1`), eigener
  Abschnitt 2 dafuer statt eines AP-Abschnitts.
- **Kein mDNS-Hostname-Zugriff** erwaehnt (`http://<name>.local/`) - nicht
  implementiert, waere erfunden gewesen.
- **Kein XML-Konfigurationsexport/-import-Zyklus** wie bei Sensormeter
  (`config.xml` dump/upload) - ESP-BMC hat nur einen JSON-Export
  (`config download`, USB/Web), noch kein Upload/Restore-Gegenstueck
  (siehe Projekt-Memory, bewusst zurueckgestellt) - im Guide entsprechend
  nur als Read-Only-Export beschrieben, kein erfundenes Upload-Kommando.
- **Kein GitHub-Link im Cover/Footer** - Repo hat noch keinen Remote
  (siehe Projekt-Memory), ein Link waere erfunden gewesen.
- Neue, ESP-BMC-eigene Abschnitte ohne Sensormeter-Vorbild: Watchdog-LED
  (Abschnitt 3), SSH-Zugang (6), Firmware-Updates/OTA mit Identitaets-/
  Downgrade-Pruefung (7), Verdrahtung inkl. Dual-Powering-Warnung (10).

### Inhaltliche Quelle: Code direkt gelesen, nicht aus dem Gedaechtnis

Jeder USB-Kommando-Eintrag (Abschnitt 9.1) direkt gegen
`usb_manager.c`s `dispatch_command()` abgeglichen (exakte
Unterkommando-Syntax, z.B. `taster power_push|power_hold|reset
[passwort]`, `reset settings|settings_values`) statt aus fruehen
Session-Notizen uebernommen - an dieser Stelle waere ein Erinnerungsfehler
besonders aergerlich (falsche Kommandosyntax in einer gedruckten
Anleitung).

### PDF-Erzeugung: headless Chrome, wie beim etablierten Workaround

`chrome.exe --headless --disable-gpu --no-pdf-header-footer
--print-to-pdf=... file:///...html` - dieselbe Methode wie beim
fruehen "JS-lastige Preisseiten"-Workaround dieser Session (siehe
Projekt-Memory). Ergebnis: 10 Seiten, 302 KB. Verifiziert mit
`pdftotext -enc UTF-8` (ohne das Encoding-Flag zeigt das Tool Umlaute
als "?" - reines Anzeigeartefakt der Terminal-Kodierung, nicht im PDF
selbst) - Inhalt/Struktur/Sonderzeichen korrekt, kein Datenverlust bei
der HTML-zu-PDF-Konvertierung.

### Ergebnis

`docs/admin-guide.html` (neu) + `docs/admin-guide.pdf` (neu, 302 KB, 10
Seiten). HTML-Tag-Balance geprueft (div/h1/h2/h3/table/tr/td/th/ul/ol/
li/p alle ausgeglichen). Keine Firmware-Aenderung in diesem Schritt,
kein Build noetig.

## 2026-07-18 — One-Pager (HTML + PDF), Vorbild sm-Familie

Auf Nutzerwunsch: `docs/esp-bmc-onepager.html` + `.pdf`, HTML diesmal
bewusst behalten (anders als der reine PDF-Only-Ansatz beim frueheren
Sensormeter-Modul-Onepager, siehe Projekt-Memory - hier war "HTML
behalten" explizit verlangt). `sensormeter-wlan`s eigenes Repo hatte
selbst keine Onepager-HTML-Quelle (nur die PDF) - stattdessen
`sensormeter/repo/docs/sensormeter-onepager.html` (Basisprojekt der
Familie) als Strukturvorlage herangezogen: A4-Einzelseite, dreispaltiges
Grid, Kopfzeile mit Logo/Titel/Badges, Fusszeile mit Kerndokument-Liste -
inhaltlich komplett neu fuer ESP-BMC geschrieben, Farbgebung wieder auf
Copper/Patina/Indigo umgestellt (nicht Sensormeters Navy/Orange).

### Eigenes Icon statt der Sensormeter-Familienmarke

Das Vorbild nutzt ein Dreieck/Orbit-Symbol als feststehende Sensormeter-
Familienmarke - fuer ESP-BMC waere die Wiederverwendung irrefuehrendes
Cross-Branding gewesen (andere Produktfamilie). Stattdessen ein neues,
einfaches Icon entworfen (Kreis + Power-Button-Glyph in
Copper/Patina) - passt inhaltlich zum "Fernsteuerung/Power-Management"-
Thema und ist mit wenigen SVG-Primitiven umgesetzt.

### Erster Render: zwei statt eine Seite - Typografie nachjustiert

Der erste Entwurf lief auf 2 PDF-Seiten hinaus (per Skript gezaehlt:
`/Type /Page` im PDF-Rohtext), fuer einen "One"-Pager nicht akzeptabel.
Nicht per Augenmass korrigiert, sondern gezielt: Basis-Schriftgroesse
10,5px -> 9,6px, Zeilenhoehe 1,32 -> 1,24, Abschnittsabstaende/
Innenabstaende leicht verringert, ein Absatz in der Einleitung
zusammengefasst. Nach der Anpassung erneut gerendert und mit demselben
Seitenzahl-Check bestaetigt: genau 1 Seite. Zusaetzlich einen
Screenshot des HTML (nicht der PDF) per `chrome --headless
--screenshot` angefertigt und visuell geprueft - kein Text
ueberlappt/wird abgeschnitten, sinnvolle Spaltenbalance.

### Ergebnis

`docs/esp-bmc-onepager.html` (neu) + `docs/esp-bmc-onepager.pdf` (neu,
1 Seite, 114 KB). Keine Firmware-Aenderung in diesem Schritt, kein Build
noetig.

## 2026-07-18 — OTA-Marker-Scan: Chunkgroessen-Bug aus der Sensormeter-Familie uebernommen und behoben

Sensormeters erster echter End-to-End-OTA-Test (HTTP-Upload ueber LAN)
deckte auf, dass der urspruengliche NUL-Byte-sichere Marker-Scan zwar
das eigentliche Problem loeste, dabei aber einen neuen, subtileren Bug
einbaute: `scan_chunk_for_marker()` (`ota_manager.c`) kopierte jeden
eingehenden Chunk in einen auf `TAIL_CAP+1024` = 1040 Byte GEDECKELTEN
Zwischenpuffer (`s_join_buf`) und durchsuchte nur diesen - der
Multipart-Upload-Handler in `web_server_manager.c` liefert Chunks aber
bis `OTA_RECV_BUF` = 2048 Byte, wodurch alles jenseits der Deckelung
STILLSCHWEIGEND uebersprungen wurde (weder gescannt noch als Tail
vorgemerkt). Bei sensormeter wurde das durch einen echten Netzwerk-Test
aufgedeckt und behoben, dort per Live-OTA-Upload verifiziert; hier
identisch uebernommen, da ESP-BMC bislang noch nie an echter Hardware
geflasht wurde und ein analoger Live-Test noch nicht moeglich ist.

Fix identisch zum Sensormeter-Muster: kein kopierter Zwischenpuffer mehr
fuer den Chunk selbst - `find_bytes()` durchsucht `data`/`len` jetzt
direkt (beliebig gross, keine Kopie noetig). Ein kleiner Join-Puffer
(`TAIL_CAP+MARKER_PREFIX_LEN` = 29 Byte, Stack-lokal statt statisch) wird
nur noch fuer den echten Grenzfall gebraucht, dass der Prefix im Tail des
vorigen Chunks beginnt und in den ersten Bytes dieses Chunks endet. Das
alte statische `s_join_buf[JOIN_CAP]` (1040 Byte BSS) entfaellt komplett.

Verifiziert per `pio run` fuer **beide** Umgebungen (n16r8 UND n8r8, wie
inzwischen Standard fuer jeden Verifikations-Durchlauf): beide sauber,
Flash/RAM praktisch unveraendert (n16r8: 61,8%/19,6%, n8r8: 61,6%/19,6%).
Noch nicht auf echter Hardware getestet - ESP-BMC wurde diese Sitzung
weiterhin nicht geflasht.

## 2026-07-18, spaeter am selben Tag — Lernuebertrag aus der Sensormeter-Familie: kritischer HTTP-Server-Stack-Overflow gefunden

Nach dem produktiven Bringup-Tag bei sensormeter-poe (SNMP-Konstruktor-
Absturz, loopTask-Stack-Overflow, SSD1306-Fehlinit, TZ-Bug, CSS-Layoutfehler)
gezielt geprueft, welche dieser Lektionen auf ESP-BMC uebertragbar sind:

- **TZ-Bug** (POSIX-TZ faellt nach Software-Reset auf UTC zurueck, wenn
  `configTzTime()` uebersprungen wird): **nicht anwendbar** -
  `time_manager_init()` setzt `setenv("TZ",...); tzset();` bereits
  unbedingt am Anfang, unabhaengig vom Sync-Status. War von Anfang an
  richtig gebaut.
- **CSS-Select-Breitenbug** (Werksreset-Dropdown ohne Breitenbegrenzung):
  **nicht anwendbar** - alle drei `<style>`-Bloecke in
  `web_server_manager.c` geprueft; der einzige mit echten
  Formularfeldern hat bereits `input,select,textarea{width:100%%;...}`.
- **Arduino-loopTask-Stack-Overflow** (viele Manager in einer synchronen
  `loop()`): **Analogie nicht direkt uebertragbar** - ESP-BMCs
  Architektur hat keine vergleichbare "viele Module pro Durchlauf"-
  Schleife (jede Komponente laeuft in ihrem eigenen Task), und
  `app_main()`s Init-Pfad selbst hat keine grossen lokalen Puffer
  (gezielt in allen `*_init()`-Funktionen gesucht, keine gefunden).

**Aber die Ueberpruefung deckte ein staerkeres, eigenstaendiges Problem
auf**: `httpd_config_t`s Default-Stackgroesse (`HTTPD_DEFAULT_CONFIG()`)
betraegt nur 4096 Byte. `settings_get_handler()` (Zeile 476-742) summiert
allein bei seinen groesseren lokalen Puffern (`page[12288]` +
`ota_card[1280]` + `scan_html[1024]` + `users_html[512]` +
`taster_pw_html[160]` + diverse kleinere) auf ueber 15 KB - mehr als das
Dreifache des verfuegbaren Stacks. `root_get_handler()` (Zeile 202-369,
die Uebersichtsseite) liegt mit `final_page[4800]` + `notify_card[640]` +
`ssh_host_card[400]` + `ssh_card[512]` + `current_key[256]` ebenfalls
bereits ueber 4096 Byte. **Das waere ein garantierter, reproduzierbarer
Stack-Overflow beim allerersten Aufruf der Uebersichts- oder
Einstellungsseite gewesen** - nie aufgefallen, weil ESP-BMC in dieser
gesamten Sitzung nie auf echter Hardware geflasht bzw. das Web-Interface
nie tatsaechlich aufgerufen wurde.

Fix: `httpd_config.stack_size` explizit auf 24576 Byte gesetzt (deutliche
Reserve ueber die ermittelten ~15-16 KB des groessten Handlers) - ein
einzelner dedizierter Task, RAM-Kosten fuer ein Geraet mit 320KB+ SRAM
vertretbar. Die Einstellung gilt serverweit fuer alle Handler (ein
gemeinsamer Worker-Task), behebt damit sowohl `settings_get_handler()`
als auch `root_get_handler()` gleichzeitig.

Verifiziert per `pio run` fuer beide Umgebungen (n16r8/n8r8), beide
sauber. Noch nicht auf echter Hardware getestet.

## 2026-07-20 — Sensor-Pins auf die linke Leiste verschoben (Huckepack-Platine, nur eine Stiftleiste)

Anlass: geplante Huckepack-Platine fuer die Optokoppler-/LED-/Sensor-
Beschaltung soll nur eine einzige Stiftleiste zum ESP32-S3-DevKitC-1
brauchen statt beider. Cross-Check der vollstaendigen 44-Pin-Tabelle aus
`docs/verdrahtungsplan.html` ergab: die acht Taster-/LED-Pins aus
`gpio_manager.h` (4/5/6/7/15/16/17/18) lagen bereits komplett auf der
linken Leiste - nur die beiden Sensor-Pins aus `sensor_manager.h` (NTC auf
GPIO1, DHT11 auf GPIO2) sassen auf der rechten.

Beide auf zuvor freie, unkritische linke Pins verschoben:
- NTC: GPIO1 (ADC1_CH0) -> **GPIO9 (ADC1_CH8)** - Kanalzuordnung gegen
  `soc/esp32s3/include/soc/adc_channel.h` verifiziert (`ADC1_GPIO9_CHANNEL
  = 8`), keine andere Verwendung auf GPIO9.
- DHT11: GPIO2 -> **GPIO11** - reiner Digital-Pin, kein Strapping/JTAG/
  UART/USB-Konflikt laut derselben 44-Pin-Tabelle.

Damit liegen jetzt alle zehn genutzten Signale auf der linken Leiste;
GPIO1/GPIO2 auf der rechten Leiste sind wieder frei. `sensor_manager.h`
und `gpio_manager.h` Kommentare aktualisiert, `docs/verdrahtungsplan.html`
(Sensorik-Tabelle + vollstaendige 44-Pin-Tabelle) entsprechend
nachgezogen. Verifiziert per `pio run` fuer beide Umgebungen (n16r8/n8r8),
beide sauber. Noch nicht auf echter Hardware getestet (kein Board
vorhanden) - insbesondere die ADC-Kalibrierwerte fuer GPIO9 sind bislang
nur aus dem SoC-Header abgeleitet, nicht real gemessen.

## 2026-07-23 — Pinbeschriftung auf Silkscreen umgestellt (docs/verdrahtungsplan.html)

Auf Nutzerwunsch alle Pin-Bezeichnungen in `docs/verdrahtungsplan.html`
von der generischen `GPIOxx`-Schreibweise auf das umgestellt, was
tatsaechlich auf der Platine aufgedruckt ist. Dabei ein Unterschied
zwischen den beiden bisherigen Bildquellen entdeckt:

- `board.jpg`/`board mit bezeichner.bmp` (Vendor-Referenzgrafiken) drucken
  bare Zahlen ohne Praefix (`4`, `43`, `0`, ...) und `TX`/`RX` fuer die
  Debug-UART-Pins.
- `docs/board-foto.jpg` (echtes Foto des tatsaechlich vorliegenden Boards)
  zeigt stattdessen `G`-Praefix (`G4`, `G43`, `G0`, ...) und `TX0`/`RX0`
  fuer dieselben Pins - eine andere Silkscreen-Revision derselben
  Modulfamilie.

Als massgeblich das echte Foto (`board-foto.jpg`) behandelt, da es das
tatsaechlich vorliegende Bauteil zeigt, nicht nur eine generische
Vendor-Abbildung. Ergebnis: durchgaengig `Gxx` statt `GPIOxx` (reine
Textumstellung, 1:1 selbe Nummer), plus `TX0`/`RX0` statt `GPIO43`/`GPIO44`
an den entsprechenden Stellen (Abschnitt 1 USB-Beschreibung, Abschnitt 7
Pintabelle, Diagramm-Beschriftungen in Abschnitt 4). Die zugrundeliegende,
bereits mehrfach verifizierte Pin-zu-GPIO-Zuordnung selbst (Abschnitt 7,
`pinout.md`) ist davon nicht betroffen - reine Anzeige-/Beschriftungsfrage,
keine electrische Aenderung. Bilder ausgewertet per Zuschnitt/Vergroesserung
(PowerShell `System.Drawing`), keine echte Hardware vorhanden zum
Gegenpruefen mit einem Multimeter/der Platine selbst.

## 2026-07-23 — Erster echter Hardware-Bring-up: drei Boot-Abstuerze gefunden, zwei behoben

Erstes Mal, dass die Firmware auf echtem Silizium lief (N16R8-Board,
vorher nur Wokwi-Simulation/Build-Verifikation). Board-Identitaet vorab
per `esptool.py flash_id` bestaetigt (echtes ESP32-S3, 16 MB Flash, 8 MB
PSRAM - passt zum Ziel-Environment). Drei reproduzierbare Abstuerze
gefunden:

1. **WireGuard-Boot-Crash** (Guru Meditation LoadProhibited in
   `psa_crypto_init()`, ausgeloest von `esp_wireguard`/
   `wireguard-platform.c`): vermutlich eine Inkompatibilitaet zwischen
   mbedTLS 4.x's "tf-psa-crypto"-Architektur (IDF 6.0.1) und dem
   `esp_wireguard`-Fork - `CONFIG_MBEDTLS_THREADING_C`/`_PTHREAD` waren
   bereits korrekt gesetzt, also keine einfache Kconfig-Luecke. Nicht an
   der Wurzel behoben, nur umgangen: `wireguard_manager_init()` wird jetzt
   nur noch aufgerufen, wenn tatsaechlich eine hochgeladene Konfiguration
   existiert (`wireguard_manager_config_available()`, neu) oder der
   Kconfig-Entwicklertest explizit gewuenscht ist (siehe Kommentar in
   `main.c`). `wireguard_manager_is_up()`/`_disconnect()` zusaetzlich gegen
   `s_ctx.netif == NULL` abgesichert, da sie sonst crashen wuerden, wenn
   init() uebersprungen wurde.
2. **SSH-Stack-Overflow** (`Transform_Sha256` in wolfSSL, waehrend
   `ssh_manager_init()`s Host-Key-Erzeugung auf dem main-Task): der
   main-Task selbst war mit dem ESP-IDF-Default (3584 Byte) zu knapp
   bemessen, obwohl der dedizierte `ssh_task` bereits grosszuegig war.
   Behoben: `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384` in
   `sdkconfig.defaults`.
3. **Task-Watchdog-Absturz (IDLE0/CPU0)**: siehe eigener Abschnitt unten
   ("WLAN-Reconnect-Sturm") - Ursache gefunden und behoben.

### WLAN-Reconnect-Sturm ohne Backoff hat den Task-Watchdog ausgehungert

Der dritte Absturz (Task-Watchdog-Panic, IDLE0 auf CPU0 kam nicht mehr
zum Zug) zeigte im Panic-Snapshot zunaechst `gpio_manager` als
laufende Task - das war aber eine Fehlspur: alle vier Sense-Pins
(`GPIO_REMOTE_POWER_SENSE`, `_RESET_SENSE`, `_POWER_LED_IN`,
`_HDD_LED_IN`) haben bereits interne Pull-ups (`configure_input_pullup()`
in `gpio_manager_init()`), floaten also nicht, selbst wenn (wie auf dem
Testaufbau) kein echter PC angeschlossen ist.

Tatsaechliche Ursache: `network_manager.c`s `WIFI_EVENT_STA_DISCONNECTED`-
Handler rief bislang synchron und ohne jede Verzoegerung
`esp_wifi_connect()` auf. Auf dem Testaufbau war noch kein WLAN
hochgeladen, die Firmware versuchte also dauerhaft die
Kconfig-Platzhalter-SSID `CHANGE_ME_SSID` zu joinen - das schlaegt sofort
fehl, loest sofort das naechste DISCONNECTED-Event aus, usw. Dieser
ungebremste Sturm hat den Task-Watchdog ausgehungert.

Behoben nach demselben bewaehrten Muster wie `sensormeter-wlan`s
`NetworkManager.cpp`: der Disconnect-Handler loest keinen Reconnect mehr
direkt aus, sondern setzt nur noch `s_connected = false`. Ein neuer,
periodischer `esp_timer` (`network_manager_tick()`, 5s-Takt) treibt
stattdessen eine kleine Zustandsmaschine (`WLAN_CHECK` / `FALLBACK_MODE`
/ `RUN_NORMAL`), die Reconnect-Versuche auf maximal einen pro 20s
begrenzt (`RECONNECT_RETRY_INTERVAL_US`) und nur, wenn ueberhaupt eine
SSID gesetzt ist.

### Fallback-Access-Point (Nachtrag zur Entscheidung "Kein AP-Fallback-Abschnitt")

Der obige Eintrag "Bewusste Abweichungen vom Vorbild" (Admin-Guide,
weiter oben in diesem Dokument) haelt fest, dass ESP-BMC anders als
sensormeter-wlan bewusst KEINEN Access-Point-Ersteinrichtungsmodus hat,
weil die Ersteinrichtung ueber USB (`tools/Setup.ps1`) laeuft. Auf
ausdruecklichen Nutzerwunsch nachtraeglich doch ergaenzt: bleibt ein
konfiguriertes WLAN 5 Minuten lang (`WLAN_CHECK_TIMEOUT_US`, bewaehrter
Wert aus sensormeter-wlan) unerreichbar, startet `network_manager.c`
einen eigenen Access Point (`installer`/`installer`, 192.168.4.1/24,
eigener DHCP-Server, kein Routing ins Internet - 1:1 dieselben Werte wie
sensormeter-wlan). `web_server_manager` bleibt darueber erreichbar, neue
WLAN-Zugangsdaten koennen wie gewohnt per `network_manager_join()`
eingetragen werden, was zurueck auf STA-Modus schaltet. Die
USB-Ersteinrichtung bleibt der primaere, empfohlene Weg fuer die
Erstinbetriebnahme - der AP ist ein zusaetzliches Sicherheitsnetz fuer
den Fall, dass ein einmal funktionierendes WLAN spaeter dauerhaft
wegfaellt (Router-Tausch, falsches Passwort nach einer Aenderung, etc.),
ohne dass USB-Zugriff auf das Geraet noch moeglich/bequem waere.
Sicherheitsaspekt bewusst nicht weiter gehaertet (festes
Standardpasswort "installer", wie im Vorbild) - falls das Geraet in
einer Umgebung eingesetzt wird, in der ein offener Fallback-AP ein
inakzeptables Risiko waere, muesste das gesondert adressiert werden.

## 2026-07-23, spaeter am selben Tag — SNMP-Test: Stack-Overflow bei SET-Anfragen gefunden und behoben

Erster echter SNMP-Test gegen das Geraet (net-snmp `snmpwalk`/`snmpget`/
`snmpset` aus WSL, da auf dem Windows-Host keine SNMP-Tools verfuegbar
waren). GET-Anfragen gegen alle 17 Custom-OIDs
(`1.3.6.1.4.1.99999.10.x`) liefen sofort einwandfrei und lieferten
plausible Werte (SSID, IP, Firmware-Version, Sensorwerte, Uptime, ...).

Ein SET (`OID_POWER_KEY`, mit der `private`-Community, wie ein SNMP-
Fernauslösen des Power-Tasters) fuehrte dagegen reproduzierbar zu einem
Absturz - Seriellausgabe zeigte explizit "A stack overflow in task
snmp_manager has been detected". Ursache: der SNMP-Task
(`xTaskCreate(snmp_task, "snmp_manager", 4096, ...)`) hatte nur 4096
Byte Stack - fuer reine GET-Antworten (BER-Kodierung ohne weitere
Aufrufe) ausreichend, aber `set_power_key()` ruft zusaetzlich
`gpio_manager_trigger_power()` sowie `audit_log_add()` mit einem
lokalen 96-Byte-Formatpuffer auf, alles noch auf demselben Stack
oberhalb der laufenden BER-Anfrageverarbeitung - in Summe zu viel fuer
4096 Byte. Behoben durch Anheben auf 8192 Byte (`snmp_manager.c`,
`snmp_manager_init()`) - gleiche Groessenordnung wie `ssh_manager`s
dedizierter Task, aus demselben strukturellen Grund (mehrschichtige
Aufrufe mit lokalen Formatpuffern, siehe dortigen Kommentar
"sm-Stack-Overflow-Lehre"). Nach dem Fix auf echter Hardware verifiziert:
SET liefert den gesetzten Wert korrekt zurueck, kein Absturz mehr,
Zugriffskontrolle (SET mit der `public`/Read-only-Community wird
weiterhin korrekt mit einem SNMP-Fehler abgelehnt) unveraendert intakt.

## 2026-07-23, Fortsetzung — WireGuard-Tunnel echt zum Laufen gebracht (4 Bugs) + IDLE0-Starvation root-caused

Ziel dieser Sitzung: den beim ersten Bring-up nur umgangenen WireGuard-Boot-
Absturz an der Wurzel beheben und einen echten Tunnel gegen den realen Peer
(`gate.sps-cloud.de`, Config `WG-SPS-CLOUD.conf`) aufbauen. Ergebnis: Tunnel
steht (`vpnUp=1` per SNMP, lokale Tunnel-IP 10.2.2.67), Geraet stabil. Es
kamen VIER echte Fehler nacheinander zum Vorschein - jeder hatte den
naechsten maskiert, weil der Code-Pfad vorher immer schon abbrach.

**Bug 1 - `psa_crypto_init()`-Absturz (der urspruenglich nur umgangene).**
`esp_wireguard`/`wireguard-platform.c` ruft unter mbedTLS 4.x (IDF 6.0.1)
`psa_crypto_init()` auf und zieht Zufallsbytes ueber `psa_generate_random()`.
Genau dieser Aufruf stuerzte reproduzierbar ab (LoadProhibited). Erkenntnis:
WireGuards eigentliche Krypto nutzt PSA GAR NICHT - BLAKE2s/ChaCha20Poly1305
liegen als C-Referenz in `crypto/refc/`, X25519 kommt aus libsodium. PSA
diente ausschliesslich der Zufallsbyte-Erzeugung. Fix: den PSA-Pfad durch den
ESP32-Hardware-RNG `esp_fill_random()` ersetzen (genau wie die ESP8266-/
LibreTiny-Zweige derselben Datei), `psa_crypto_init()` faellt weg. Umgesetzt
als idempotenter CMake-Patch in `firmware/CMakeLists.txt` (GLOB ueber
`.pio/libdeps/*/esp_wireguard/src/wireguard-platform.c`), gleiches Muster wie
die wolfssl-/led_strip-Patches. `main.c`: WireGuard wird weiterhin nur bei
vorhandener Konfiguration initialisiert - das ist jetzt aber kein
Absturzschutz mehr, sondern schlicht sinnvoll.

**Bug 2 - `config->address == (null)` beim Boot (Dangling-Pointer).**
`esp_wireguard_init()` speichert nur den POINTER auf die uebergebene
`wireguard_config_t` (`ctx->config = config`), kopiert sie nicht.
`build_and_init()` in `wireguard_manager.c` uebergab aber eine STACK-LOKALE
`wg_config`. Nach dem Return war deren Stack ungueltig; der spaetere,
separate `esp_wireguard_connect()`-Aufruf (im Boot-Pfad mit einer
`vTaskDelay`-Warteschleife dazwischen, die den Stack ueberschreibt) las dann
`config->address` als `(null)` -> `netif_create` schlug fehl, Tunnel kam nie
hoch. Ueber den Web-Upload-Pfad (init+connect direkt hintereinander, kein
Delay) hatte es per Zufall funktioniert. Fix: `wg_config` `static` - alle
referenzierten Felder (`s_private_key` etc.) sind ohnehin schon statisch.

**Bug 3 - Guru Meditation in `esp_netif_internal_dhcpc_cb` (LoadProhibited,
EXCVADDR=0).** Erst nach Fix 2 erreichte der Code erstmals `netif_add()`.
`esp_wireguard` legt sein Interface per ROHEM lwIP-`netif_add()` an (nicht
ueber esp_netif); `netif->state` zeigt dabei auf die wireguardif-Struktur.
Ohne Bridge/PPP ist `LWIP_ESP_NETIF_DATA=0`, d.h. esp_netif nutzt genau
`netif->state`, um jedem netif seinen esp_netif-Wrapper zuzuordnen. Der
globale esp_netif-DHCP-Callback (feuert bei JEDEM netif_add) hielt das rohe
WG-netif dann faelschlich fuer ein esp_netif und dereferenzierte dessen
NULL-`ip_info`. Fix: `CONFIG_ESP_NETIF_BRIDGE_EN=y` in `sdkconfig.defaults`
schaltet `LWIP_ESP_NETIF_DATA=1` (siehe
`components/lwip/port/include/lwipopts.h` - der Kommentar dort beschreibt
exakt diesen Fall: "special lwip interfaces" muessen den esp_netif-Zeiger in
`netif->client_data` statt `netif->state` ablegen). Damit liefert das rohe
WG-netif korrekt NULL und wird vom Callback sauber uebersprungen. Der
Bridge-Code selbst wird nicht genutzt - reiner Nebeneffekt der Option.

**Bug 4 - Tunnel blieb unten trotz erfolgreichem netif (async-DNS-Timing).**
`esp_wireguard_connect()` loest den Endpoint-Hostnamen asynchron auf und gibt
`ESP_ERR_RETRY` zurueck, solange DNS laeuft; der DNS-Callback stoesst den
Handshake NICHT selbst an - das passiert erst beim naechsten connect()-Aufruf
nach fertigem DNS. `main.c` rief connect() aber nur einmal. Fix: begrenzte
Retry-Schleife (15x, 2s) im Boot-Pfad (Blockieren im app_main-Task
unkritisch; im Web-Upload-Handler waere es das nicht, daher bewusst nur hier).
Der netif wird beim ersten Versuch erzeugt, Folgeversuche warten nur auf die
dann gecachte DNS-Antwort. Danach: `vpnUp=1`, ueber mehrere Minuten stabil.

**Bonus - die "IDLE0-Starvation" ist root-caused (und war NICHT der
WLAN-Sturm).** Beim Serial-Mitschnitt fiel auf, dass der Task-Watchdog nach
wie vor alle ~5s anschlug (IDLE0 auf CPU0, `gpio_manager` als laufende Task) -
also NICHT durch den WLAN-Reconnect-Sturm allein erklaerbar, wie weiter oben
in diesem Log angenommen. Backtrace-Decode zeigte: `gpio_manager_task` haengt
bei `vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_POLL_MS))` mit `DEBOUNCE_POLL_MS=5`.
Bei `CONFIG_FREERTOS_HZ=100` ist ein Tick 10ms, also `pdMS_TO_TICKS(5)=0`, und
`vTaskDelay(0)` blockiert NICHT, sondern yieldet nur. Da `gpio_manager`
(Prio 1, an Core 0 gepinnt) hoehere Prio als IDLE0 (Prio 0) hat, wurde er
sofort wieder eingeplant -> Dauerschleife, IDLE0 kam nie zum Zug, Watchdog
schlug an. Das erklaert das "deterministisch ~5,98s nach jedem Boot"-Symptom
vollstaendig und lag seit dem allerersten Boot vor. Fix in `gpio_manager.c`:
`DEBOUNCE_POLL_MS 5->10` (= genau 1 Tick, blockiert echt),
`DEBOUNCE_STABLE_CYCLES 6->3` (haelt das 30ms-Entprellfenster), plus ein
Guard `if (poll_ticks==0) poll_ticks=1`. Nach dem Fix auf Hardware
verifiziert: 0x Task-Watchdog in >40s Dauerbetrieb (vorher alle 5s). Nebenbei
wird damit die in `docs/systemlast.md` geschaetzte Grundlast (<2% fuer
gpio_manager) erstmals real zutreffend - vorher lief der Task real bei ~100%
auf Core 0.

Offen geblieben: `CONFIG_ESP_TASK_WDT_PANIC` steht weiterhin auf `n` (beim
Bring-up als Workaround gesetzt). Da die IDLE0-Ursache jetzt gefunden UND
behoben ist, kann/sollte Panic+Reboot als echte Selbstheilung wieder auf `y` -
bewusst noch nicht in dieser Sitzung umgestellt, um die WG-Fixes isoliert zu
verifizieren. NTC-Verdrahtung (Vadc=0, fehlender Vorwiderstand) weiterhin
offen (Hardware). Alle Firmware-Aenderungen auf n16r8 gebaut und auf die
echte Hardware geflasht; n8r8-Paritaetsbuild noch ausstehend.

## 2026-07-23, Fortsetzung — VPN-Audit, WG-Config auf Settings, Sensor-Graph 5min/24h, WDT-Panic wieder scharf

Nutzerwuensche nach dem WireGuard-Bring-up, alle auf echter Hardware
verifiziert (Login admin/admin, SNMP-Gegenpruefung):

**VPN-Ereignisse ins Audit-Log.** `wireguard_manager` band `audit_log` bisher
gar nicht ein. Ergaenzt: Upload ("VPN-Konfiguration hochgeladen (Endpoint
...)") und Loeschen ("VPN-Konfiguration geloescht") loggen direkt in
apply/delete. Fuer Auf-/Abbau des Tunnels ein neuer Monitor-Task
(`wg_monitor`, 15s-Takt, Prio 1, 3072 Byte): erkennt is_up()-Flanken und
loggt "VPN-Tunnel aufgebaut (Peer host:port)" bzw. "VPN-Tunnel getrennt".
Bewusst ein eigener Task statt esp_timer, weil `audit_log_add()` in den
Flash-Storage schreibt (gehoert nicht in einen Timer-Callback mit knappem
Stack). Gestartet in main.c nach dem Connect-Gate. Verifiziert: nach Boot
steht "VPN-Tunnel aufgebaut (Peer gate.sps-cloud.de:51820)" mit echter
Uhrzeit im Audit-Log. CMakeLists: `audit_log` zu REQUIRES.

**Aktuelle WireGuard-Konfiguration auf der Einstellungsseite.** Die WG-Karte
zeigte nur "Konfiguration: hochgeladen/Platzhalter + Tunnel-IP". Jetzt ein
Detailblock: Status, Tunnel aktiv/inaktiv (is_up), Endpoint (Peer),
Tunnel-IP (lokal), Peer-PublicKey, AllowedIPs. Neue Getter
`wireguard_manager_get_public_key()` und `_get_allowed_ips()` (Letzterer
rechnet die gespeicherte Netzmaske zurueck in eine Prefixlaenge). Der
**PrivateKey wird bewusst NIE ausgegeben** (kein Getter dafuer). Das
Upload-Textfeld zum Anpassen bleibt darunter. Verifiziert: zeigt
gate.sps-cloud.de:51820 / 10.2.2.67 / Peer-Key / 10.22.20.0/22.

**Sensor-Graph war leer / kein DHT geloggt - Ursache und Fix.** Der DHT wird
einwandfrei gelesen (Serial: 27C/30%RH). `sensor_history` schrieb aber nur
1 Wert pro STUNDE, RAM-only, und wird bei jedem Reboot geleert - waehrend des
WG-Debuggings wurde zig Mal rebootet, daher max. 1 Punkt, ein Linienchart
wirkt damit leer. Auf Nutzerwunsch auf **alle 5 Minuten, 288 Slots (24h)**
umgestellt (SENSOR_HISTORY_SLOTS in den Header gezogen, RECORD_INTERVAL_US =
5min). RAM-only bleibt bewusst (Flash-Verschleiss). Folgeaenderungen: die
JSON-/CSV-Puffer in `web_server_manager` (api_graph, logs/sensors.csv) waren
mit 2048 Byte zu klein fuer 288 Eintraege - jetzt aus dem Heap (PSRAM-gedeckt,
12K bzw. 16K) statt vom httpd-Worker-Stack alloziert (entries ~9K + body ~12K
haetten den 24576-Stack gesprengt). Chart-Beschriftung von "-Nh" auf echte
Uhrzeit HH:MM umgestellt (aus der Wall-Clock je Messpunkt rueckgerechnet).
RAM real 19,6% -> 22,2% (der groessere Ringpuffer). Verifiziert: /api/graph
liefert direkt nach Boot einen Punkt mit dht_temp=27.0, dht_humidity=30.3,
ntc=null (korrekt, NTC-Verdrahtungsluecke).

**Task-Watchdog-Panic wieder aktiv.** Da die IDLE0-Starvation root-caused und
behoben ist (vTaskDelay(0), siehe vorheriger Eintrag) und der Dauerbetrieb
inkl. WireGuard-Tunnel sauber lief, `CONFIG_ESP_TASK_WDT_PANIC` zurueck auf
`y` - echte Selbstheilung (Reboot bei haengender Task) statt nur Logging.
Verifiziert: Geraet laeuft mit scharfem Panic stabil weit ueber die alte
~6s-Panic-Schwelle hinaus, Tunnel bleibt oben.

Offen weiterhin: NTC-Vorwiderstand (Hardware), n8r8-Paritaetsbuild.

## 2026-07-23, Fortsetzung — Benutzer loeschen (Web-UI) + SSH-Tests abgeschlossen

**Benutzer loeschen.** `user_manager_delete()` existierte im Backend, war aber
an keinen Endpoint gebunden - es gab also keinen Weg, ein Konto zu loeschen.
Neu: in der Benutzerverwaltung (Einstellungsseite) hat jeder Eintrag einen
"Loeschen"-Knopf (POST /settings/users/delete, VERWALTER+, wie das Anlegen),
mit JS-confirm(). Neuer Handler mit zwei Aussperr-Schutzguards: (1) das eigene
angemeldete Konto ist nicht loeschbar (auch serverseitig geprueft, der eigene
Listeneintrag zeigt statt Knopf "angemeldet"), (2) der letzte verbleibende
Admin ist nicht loeschbar. Jede Loeschung wird ins Audit-Log geschrieben
("Benutzer \"x\" geloescht durch y"). Dafuer musste users_html 512->2560 und
page 12288->16384 wachsen (Loeschen-Formulare je Konto), der httpd-Worker-
Stack entsprechend 24576->32768. Verifiziert: anlegen->erscheint,
loeschen->weg, Selbst-Loeschen wird mit ?failed=user_self abgewiesen.

**SSH-Tests abgeschlossen (Passwort UND Public-Key, beide auf Hardware
verifiziert).** Testkonto `tester` (Rolle SSH User) aus der ersten Bring-up-
Sitzung ist im Storage erhalten geblieben.
- **Passwort:** `plink -pw` als tester -> Serial-Log "Login (SSH, Passwort):
  tester (SSH User)" + "SSH-Konsole aktiv". Erfolgreich. (Der plink-Hostkey-
  Prompt haengt nicht-interaktiv; mit `-hostkey SHA256:...` umgangen.)
- **Public-Key:** WICHTIGE KORREKTUR zur bisherigen Annahme "ECDSA/Ed25519":
  Der wolfSSH-Server bietet in server-sig-algs NUR ECDSA an
  (ecdsa-sha2-nistp256/384/521), KEIN Ed25519. Ein ed25519-Testschluessel
  wurde daher mit "Permission denied (publickey)" abgelehnt (Auth-Callback
  wurde gar nicht erst erreicht). Mit einem ECDSA-nistp256-Schluessel
  (via ssh-keygen -t ecdsa, ueber /account/ssh-key hinterlegt) dann
  erfolgreich: Serial-Log "Login (SSH, Public-Key): tester (SSH User)".
  Der Platzhalter im SSH-Key-Feld, der faelschlich "ssh-ed25519 AAAA..."
  vorschlug, wurde entsprechend auf "nur ECDSA/nistp256-384-521, kein
  Ed25519/RSA" korrigiert.
- **Session-Verhalten (bekannt, bestaetigt):** ssh_manager implementiert nur
  den "shell"-Kanal, keinen "exec"-Kanal - ein `ssh host <kommando>` oeffnet
  daher eine Konsole (Bridge zum PC) statt das Kommando auszufuehren, der
  Client "haengt" bis Timeout. Fuer eine BMC-Konsole ist genau das der
  Anwendungsfall (interaktive Sitzung). Es gibt genau EINE Konsolensitzung
  gleichzeitig (Web ODER SSH); eine abrupt beendete Client-Sitzung gibt die
  Konsole erst nach dem 200ms-Socket-Timeout wieder frei, weshalb schnelle
  Back-to-Back-SSH-Tests einander blockieren koennen - einzeln getestet
  laeuft beides sauber. Mit gefixtem Watchdog + scharfem Panic blieb das
  Geraet ueber alle Tests stabil (kein Reboot).

Moegliche Folgearbeiten (nicht gemacht): Ed25519-Public-Key-Auth in wolfSSL/
wolfSSH aktivieren (HAVE_ED25519), falls ed25519-Schluessel gewuenscht sind;
"exec"-Kanal fuer nicht-interaktive SSH-Kommandos.

## 2026-07-24 — Ed25519-SSH-Pubkey-Auth aktiviert (ein Flag: WOLFSSL_ED25519_STREAMING_VERIFY)

Nachtrag zum SSH-Test-Abschluss: ed25519-Client-Keys wurden bislang abgelehnt,
der Server bot nur ECDSA in server-sig-algs an. Ed25519 ist heute der
De-facto-Standard fuer SSH-Keys (OpenSSH-Default seit 8.x), daher aktiviert.

Ursachensuche war laenger als erwartet (typische wolfSSL/wolfSSH-Config-
Verschachtelung). Zwischenstationen, die NICHT die Ursache waren: HAVE_ED25519
ist im wolfSSL-user_settings.h-Template fuer ESP32-S3 bereits an, und
settings.h leitet daraus automatisch alle vier Sub-Features ab
(HAVE_ED25519_SIGN/VERIFY/KEY_IMPORT/KEY_EXPORT) - die sind also da (ein
Versuch, sie zusaetzlich in user_settings.h zu definieren, scheiterte prompt
mit "redefined"/-Werror, was das bestaetigt). Auch das Setzen per -D-Flag in
CMAKE_C_FLAGS brachte nichts.

Tatsaechliche Ursache (wolfssh/internal.h): das Gate, das WOLFSSH_NO_ED25519
setzt, verlangt VIER Bedingungen -
`!defined(HAVE_ED25519) || !defined(WOLFSSL_ED25519_STREAMING_VERIFY) ||
!defined(HAVE_ED25519_KEY_IMPORT) || !defined(HAVE_ED25519_KEY_EXPORT)`.
Die einzige nicht erfuellte war **WOLFSSL_ED25519_STREAMING_VERIFY**, ein
separates wolfSSL-Opt-in (per Default aus, wird nirgends automatisch gesetzt).
wolfSSH braucht es, weil es die ed25519-Signatur ueber einen inkrementell
gefuetterten Hash prueft. Ohne dieses Flag deaktiviert wolfSSH ed25519
komplett - unabhaengig davon, dass wolfSSL ed25519 voll unterstuetzt.

Fix: `#define WOLFSSL_ED25519_STREAMING_VERIFY` neben dem vorhandenen
`#define HAVE_ED25519` in wolfSSLs user_settings.h, per idempotentem
CMake-Patch in firmware/CMakeLists.txt (gleiches Muster wie die wolfssl-
thread_local-/led_strip-/esp_wireguard-Patches - managed_components werden
vom Component-Manager bei jeder Neukonfiguration wiederhergestellt). Bewusst
in user_settings.h statt per -D, damit wolfSSL UND wolfSSH es konsistent im
selben Config-Kontext sehen (settings.h leitet daraus auch
WOLFSSL_ED25519_PERSISTENT_SHA ab). Das Flag wird sonst nirgends definiert,
also keine Redefinition. Kein App-Code-Eingriff noetig
(user_manager_verify_ssh_public_key vergleicht den Key-Blob byteweise,
algorithmus-agnostisch).

Verifiziert auf Hardware: server-sig-algs enthaelt jetzt ssh-ed25519, und ein
ed25519-Client-Key meldet sich erfolgreich an (Serial-Log "Login (SSH,
Public-Key): tester"). Flash-Zuwachs ~62 KB (62,3% -> 65,3%) durch die
ed25519-Sign/Verify-/Streaming-Codepfade; weiterhin ~35% frei. Der irrefuehrend
auf "kein Ed25519" korrigierte Platzhalter im SSH-Key-Feld wurde wieder auf
"ssh-ed25519 ... oder ecdsa-... (kein RSA)" gesetzt.
