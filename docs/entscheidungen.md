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
