#pragma once

#include <stdbool.h>
#include <stddef.h>

// SnmpManager - SNMPv1-Agent (UDP Port 161) fuer Monitoring-Systeme
// (Zabbix o.ae.), analog dem bereits produktiven Muster der
// Sensormeter-Familie: eigene private Enterprise-MIB unter
// 1.3.6.1.4.1.99999.10 (kein Standard-MIB-2), Community-basierte
// Zugriffskontrolle, kein SNMPv3, kein GetBulk - volle Begruendung siehe
// docs/entscheidungen.md "SNMP-Agent".
//
// GET (exaktes OID-Match) und GETNEXT (naechstgroessere OID in der
// Tabelle) werden fuer alle Objekte unterstuetzt - reicht fuer Zabbix'
// direkte Item-OID-Abfragen UND fuer einen manuellen snmpwalk zum
// Testen. Wenn das Zabbix-Interface "Use bulk requests" verwendet, muss
// das am Host-Interface deaktiviert werden (GetBulk wird mit genErr
// beantwortet).
//
// SET ist nur fuer zwei Objekte erlaubt (powerKey/resetKey - loesen
// einen Tastendruck aus, respektieren denselben Tastschutz wie
// Web/USB) und braucht dafuer die separate Schreib-Community (nicht die
// Lese-Community) - SNMP kennt anders als Web/USB keine
// Benutzeranmeldung, deshalb dieses eigene, staerker zu schuetzende
// Geheimnis statt einer Passwort-Rueckbestaetigung.

void snmp_manager_init(void);

// Lese-Community (Default "public") - gewaehrt GET/GETNEXT auf alle
// Objekte. Persistiert auf der storage-Partition wie WLAN/WireGuard.
void snmp_manager_get_community(char* out, size_t out_len);
bool snmp_manager_set_community(const char* community);

// Schreib-Community (Default "private") - zusaetzlich noetig fuer SET
// auf powerKey/resetKey. Falsche/unbekannte Community wird
// stillschweigend ignoriert (kein Response); eine bekannte Lese- aber
// nicht Schreib-Community bekommt bei einem SET-Versuch eine explizite
// Fehlerantwort.
void snmp_manager_get_rw_community(char* out, size_t out_len);
bool snmp_manager_set_rw_community(const char* community);
