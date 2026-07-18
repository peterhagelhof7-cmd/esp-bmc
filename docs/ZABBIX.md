# Zabbix-Integration

Das Board beantwortet SNMP-v1/v2c-GET-Anfragen direkt (siehe
[docs/entscheidungen.md](entscheidungen.md) "SNMP-Agent") — Zabbix kann
es also wie jedes andere SNMP-Gerät abfragen, ganz ohne zusätzliche
Software auf dem Board oder einem Gateway. Das Template ist gegen
`firmware/components/snmp_manager/snmp_manager.c` verifiziert (siehe
Kommentar im YAML selbst), nicht nur aus einer Beschreibung übernommen.

**Read-only in der Praxis, obwohl teilweise schreibbar:** `powerKey` und
`resetKey` sind auf dem Gerät zusätzlich per SNMP `SET` schreibbar (lösen
einen simulierten Tastendruck aus, eigene Schreib-Community) — dieses
Template und diese Anleitung bilden **nur den Lesezustand** ab, keine
Zabbix-Aktion zum Auslösen ist hier eingerichtet (bewusst außerhalb des
Templates, siehe YAML-Kommentar).

**Nicht zu verwechseln mit den Sensormeter-Templates:** ESP-BMC nutzt
einen eigenen, **nicht kompatiblen** Enterprise-OID-Zweig
(`1.3.6.1.4.1.99999.10`) — die Sensormeter-Familie belegt `.1` bis `.5`
direkt unter `.99999`. Unterschiedliche Geräteklassen, unterschiedliche
Templates, nicht austauschbar.

## OIDs (Basis `.1.3.6.1.4.1.99999.10`, 17 Objekte)

Temperatur-/Luftfeuchte-Werte werden vom Gerät als Ganzzahl **×10**
übertragen und im Template per Preprocessing (Multiplikator `0.1`)
zurückgerechnet. Fehlt ein gültiger Messwert (Sensor nicht
angeschlossen/Lesefehler), liefert das Gerät den Sentinel-Wert `-32768`
(nach Umrechnung `-3276.8`) — im Zabbix-Dashboard als offensichtlicher
Ausreißer erkennbar; bewusst keine zusätzliche
Preprocessing-Fehlerbehandlung dafür (gleiche Einfachheit wie beim
Sensormeter-Vorbild).

| OID (Suffix nach `.99999.10`) | Bedeutung | Typ | Zabbix-Key | Intervall |
|---|---|---|---|---|
| `.1.0` | Systemname (Einstellungsseite, frei vergebbar) | CHAR | `system.name` | 1h |
| `.2.0` | Uptime (SNMP TimeTicks, Zentisekunden) | TEXT | `status.uptime` | 5m |
| `.3.0` | WLAN-IP | CHAR | `network.wlanip` | 5m |
| `.4.0` | WLAN-SSID | CHAR | `network.wlanssid` | 5m |
| `.5.0` | VPN-Tunnel-Status (0/1) | UNSIGNED | `vpn.up` | 1m |
| `.6.0` | VPN lokale Tunnel-IP | CHAR | `vpn.localip` | 5m |
| `.7.0` | NTC-Temperatur (×10) | FLOAT, °C | `sensor.ntctemp` | 1m |
| `.8.0` | DHT11-Temperatur (×10) | FLOAT, °C | `sensor.dhttemp` | 1m |
| `.9.0` | DHT11-Luftfeuchtigkeit (×10) | FLOAT, % | `sensor.dhthumidity` | 1m |
| `.10.0` | Power-LED erfasst (Gehäuse, 0/1) | UNSIGNED | `status.powerled` | 1m |
| `.11.0` | HDD-LED-Aktivität (letzte 10s, 0/1) | UNSIGNED | `status.hddled` | 1m |
| `.12.0` | Freier Heap (Bytes) | UNSIGNED | `status.freeheap` | 5m |
| `.13.0` | WLAN statische IP aktiv (0=DHCP, 1=statisch) | UNSIGNED | `network.wlanstatic` | 5m |
| `.14.0` | Systemtyp, fest `"ESP-BMC"` | CHAR | `system.type` | 1h |
| `.15.0` | Power-Taste wird gerade weitergeleitet (0/1) | UNSIGNED | `status.powerkey` | 1m |
| `.16.0` | Reset-Taste wird gerade weitergeleitet (0/1) | UNSIGNED | `status.resetkey` | 1m |
| `.17.0` | Firmwareversion | CHAR | `system.firmware` | 1h |

(Volle OID z. B. für Item `.7.0`: `.1.3.6.1.4.1.99999.10.7.0`.)
Community-String ist auf der Einstellungsseite des Geräts konfigurierbar
(Default `public`).

## Host-Makros (im Template vordefiniert)

| Makro | Default | Zweck |
|---|---|---|
| `{$SNMP_COMMUNITY}` | `public` | SNMP-Lese-Community (muss mit der Einstellungsseite des Geräts übereinstimmen) |
| `{$NTC_TEMP_MAX_C}` | `60` | Schwellwert Hoch-Temperatur-Warnung, NTC 10K B3590 |
| `{$DHT_TEMP_MAX_C}` | `40` | Schwellwert Hoch-Temperatur-Warnung, DHT11 |
| `{$DHT_HUMIDITY_MAX_PERCENT}` | `70` | Schwellwert Hoch-Luftfeuchtigkeit-Warnung, DHT11 |
| `{$HEAP_MIN_BYTES}` | `20000` | Schwellwert Warnung bei niedrigem freien Heap |

## Template importieren

1. In Zabbix: **Data collection → Templates → Import**
2. Datei [`zabbix-template-esp-bmc.yaml`](zabbix-template-esp-bmc.yaml)
   auswählen
3. Import bestätigen — Template-Gruppe **ESP-BMC** wird mit angelegt

## Host anlegen

1. **Data collection → Hosts → Create host**
2. Name vergeben (z. B. "ESP-BMC Büro-PC")
3. Template **"ESP-BMC"** zuweisen
4. Interface hinzufügen: Typ **SNMP**, IP-Adresse des Boards, Port
   `161`, SNMP-Version **SNMPv2** (das Gerät antwortet unabhängig davon
   auch v1-Clients korrekt, siehe `docs/entscheidungen.md`), Community
   `public` (oder eigener Wert)
5. Falls die Community auf der Einstellungsseite des Geräts geändert
   wurde: Host-Makro `{$SNMP_COMMUNITY}` im Host auf denselben Wert
   setzen
6. Schwellwert-Makros (`{$NTC_TEMP_MAX_C}` etc.) anpassen, falls sie von
   den auf der Einstellungsseite des Geräts konfigurierten Werten
   abweichen

## Mehrere Geräte

Jedes ESP-BMC-Gerät bekommt in Zabbix einen eigenen Host mit demselben
Template, nur mit unterschiedlicher IP-Adresse im SNMP-Interface. Der
Systemname (Einstellungsseite, Item `system.name`) hilft, Geräte in
Zabbix wiederzuerkennen — getrennt vom festen Systemtyp
(`system.type`, immer `"ESP-BMC"`), der nur zur groben Geräteklassen-
Unterscheidung dient, falls künftig weitere BMC-lite-Varianten
dazukommen.

## Mitgelieferte Trigger

| Trigger | Ausdruck | Priorität |
|---|---|---|
| NTC-Temperatur zu hoch | `last(/ESP-BMC/sensor.ntctemp)>{$NTC_TEMP_MAX_C}` | WARNING |
| DHT11-Temperatur zu hoch | `last(/ESP-BMC/sensor.dhttemp)>{$DHT_TEMP_MAX_C}` | WARNING |
| DHT11-Luftfeuchtigkeit zu hoch | `last(/ESP-BMC/sensor.dhthumidity)>{$DHT_HUMIDITY_MAX_PERCENT}` | WARNING |
| Freier Heap niedrig | `last(/ESP-BMC/status.freeheap)<{$HEAP_MIN_BYTES}` | WARNING |
| VPN-Tunnel down | `last(/ESP-BMC/vpn.up)=0` | HIGH |
| Keine SNMP-Daten seit 10 Minuten | `nodata(/ESP-BMC/status.uptime,10m)=1` | HIGH |

Die beiden zuletzt genannten Trigger (VPN-Tunnel down, keine Daten) sind
mit **HIGH** eingestuft, nicht WARNING — bewusst höher als bei der
Sensormeter-Familie: ein ausgefallener VPN-Tunnel bedeutet bei ESP-BMC
konkret den Verlust der Fernzugriffsmöglichkeit auf den verwalteten PC
(Kernfunktion des Geräts), nicht nur den Ausfall einer Messreihe.

**Sentinel-Werte beachten:** Ist kein gültiger Sensormesswert vorhanden,
liefert das Gerät `-32768` (roh) bzw. `-3276.8` (nach Preprocessing) —
das triggert die Temperatur-Trigger fälschlich nicht (Wert liegt weit
*unter* dem Schwellwert), erscheint aber im Dashboard als eindeutiger
Ausreißer und sollte optisch auffallen.

## Testen ohne Zabbix

Mit Net-SNMP-Tools (`apt install snmp` unter Linux, oder die
Windows-Variante von Net-SNMP):

```
snmpget -v1 -c public <board-ip> .1.3.6.1.4.1.99999.10.7.0
snmpget -v2c -c public <board-ip> .1.3.6.1.4.1.99999.10.12.0
```

## Hinweis zu den zwei Firmware-Umgebungen

`esp32-s3-devkitc-1-n16r8` und `esp32-s3-devkitc-1-n8r8` (siehe
`firmware/platformio.ini`) unterscheiden sich nur in der konfigurierten
Flash-Zielgröße — der SNMP-Agent-Code (`snmp_manager.c`) ist in beiden
identisch, das Template funktioniert unverändert unabhängig davon,
welche Umgebung geflasht wurde.

## Kein PRTG-Support

Für ESP-BMC ist — anders als teils bei anderen Projekten evaluiert —
**kein PRTG-Sensor/-Template vorgesehen**; SNMP ist offen genug, dass
PRTG das Gerät bei Bedarf generisch über eigene SNMP-Sensoren einbinden
könnte, aber es gibt dafür kein mitgeliefertes Template und keine
projektseitige Dokumentation dazu.
