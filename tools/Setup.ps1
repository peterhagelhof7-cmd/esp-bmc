<#
.SYNOPSIS
ESP-BMC Erstinbetriebnahme - siehe "inital setup.txt" (Projekt-Root).

.DESCRIPTION
Ablauf:
  1. Firmware flashen (optional)
  2. Konfigurationsvorlage in einem Editor bearbeiten, per USB anwenden
  3. WireGuard-Konfiguration importieren? (.conf im aktuellen Ordner suchen)
  4. WLAN konfigurieren? (Scan + Auswahl + PSK)

Noch nicht umgesetzt (siehe docs/entscheidungen.md): Download von
Firmware/Vorlage aus einem Git-Remote - es existiert noch keins, das
Skript arbeitet bislang mit dem lokalen Checkout (-EnvName/-Template
zeigen bei Bedarf auf etwas anderes).

.EXAMPLE
.\Setup.ps1
.\Setup.ps1 -Port COM7 -SkipFlash
#>

param(
    [string]$Port,
    [switch]$SkipFlash,
    [string]$EnvName = "esp32-s3-devkitc-1-n16r8",
    [string]$Template,
    [string]$Username = "admin",
    [string]$Password = "admin"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ToolsDir = $PSScriptRoot
$RepoRoot = Split-Path $ToolsDir -Parent
$FirmwareDir = Join-Path $RepoRoot "firmware"
if (-not $Template) { $Template = Join-Path $ToolsDir "config_template.txt" }

Import-Module (Join-Path $ToolsDir "EspBmcLink.psm1") -Force

# [double]$str verwendet die aktuelle System-Kultur - unter de-DE wird
# "60,5" NICHT als Fehler abgelehnt, sondern lautlos als 605 (Komma als
# Tausendertrennzeichen) fehlinterpretiert. Schwellwerte muessen deshalb
# explizit kulturunabhaengig (Punkt als Dezimaltrennzeichen) geparst
# werden - siehe docs/entscheidungen.md.
function ConvertTo-InvariantDouble {
    param([Parameter(Mandatory)][string]$Value, [Parameter(Mandatory)][string]$FieldName)
    $result = 0.0
    $ok = [double]::TryParse(
        $Value,
        [System.Globalization.NumberStyles]::Float,
        [System.Globalization.CultureInfo]::InvariantCulture,
        [ref]$result)
    if (-not $ok) {
        throw "Ungueltiger Zahlenwert fuer $FieldName`: `"$Value`" (Dezimaltrennzeichen muss ein Punkt sein, z.B. 60.5)"
    }
    return $result
}

function Confirm-YesNo {
    param([string]$Question, [bool]$DefaultYes = $false)
    $suffix = if ($DefaultYes) { " [J/n] " } else { " [j/N] " }
    $answer = Read-Host ($Question + $suffix)
    if ([string]::IsNullOrWhiteSpace($answer)) { return $DefaultYes }
    return $answer -match '^(j|ja|y|yes)$'
}

function Select-ComPort {
    $devices = @(Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\(COM\d+\)' })
    if ($devices.Count -gt 0) {
        Write-Host "Gefundene serielle Ports:"
        for ($i = 0; $i -lt $devices.Count; $i++) {
            Write-Host "  ${i}: $($devices[$i].Name)"
        }
        $choice = Read-Host "Index waehlen oder Port-Namen eingeben (z.B. COM7)"
        if ($choice -match '^\d+$' -and [int]$choice -lt $devices.Count -and $devices[[int]$choice].Name -match '\((COM\d+)\)') {
            return $matches[1]
        }
        return $choice
    }
    return Read-Host "COM-Port (z.B. COM7)"
}

function Invoke-FlashFirmware {
    param([string]$Environment)
    Write-Host "`n=== Firmware bauen und flashen ($Environment) ==="
    Push-Location $FirmwareDir
    try {
        & pio run -e $Environment -t upload
        if ($LASTEXITCODE -ne 0) { throw "pio run fehlgeschlagen (Exit $LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
}

# Der allererste Boot nach dem Flashen (bzw. nach einem Erase der
# storage-Partition) dauert spuerbar laenger als jeder folgende: die
# storage-Partition wird einmalig formatiert (LittleFS,
# format_if_mount_failed) UND ein neuer SSH-Host-Key wird erzeugt -
# siehe docs/entscheidungen.md "Hinweis: erster Boot nach dem Flashen
# dauert laenger". Deshalb hier mit Retries statt eines einzelnen
# Verbindungsversuchs, der sonst bei einem zu fruehen Enter-Druck mit
# einer rohen TimeoutException abbricht.
function Connect-EspBmcWithRetry {
    param(
        [Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link,
        [Parameter(Mandatory)][string]$Username,
        [Parameter(Mandatory)][string]$Password,
        [int]$MaxAttempts = 10,
        [int]$DelaySeconds = 2
    )
    for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
        try {
            return Connect-EspBmc -Link $Link -Username $Username -Password $Password
        } catch {
            if ($attempt -eq $MaxAttempts) { throw }
            Write-Host "Geraet antwortet noch nicht (Versuch $attempt/$MaxAttempts) - warte $DelaySeconds s ..."
            Start-Sleep -Seconds $DelaySeconds
        }
    }
}

function Edit-ConfigTemplate {
    param([string]$TemplatePath)
    Write-Host "`n=== Konfigurationsvorlage bearbeiten ($(Split-Path $TemplatePath -Leaf)) ==="
    $tmpPath = [System.IO.Path]::GetTempFileName() + ".txt"
    Copy-Item $TemplatePath $tmpPath -Force

    $editorCmd = if ($env:EDITOR) { $env:EDITOR } else { "notepad.exe" }
    Write-Host "Oeffne $editorCmd - Datei speichern und Editor schliessen, um fortzufahren."
    Start-Process -FilePath $editorCmd -ArgumentList $tmpPath -Wait

    $values = [ordered]@{}
    foreach ($line in Get-Content $tmpPath) {
        $trimmed = $line.Trim()
        if (-not $trimmed -or $trimmed.StartsWith("#") -or -not $trimmed.Contains("=")) { continue }
        $parts = $trimmed -split '=', 2
        $values[$parts[0].Trim()] = $parts[1].Trim()
    }
    Remove-Item $tmpPath -ErrorAction SilentlyContinue
    return $values
}

function Set-DeviceConfig {
    param(
        [System.IO.Ports.SerialPort]$Link,
        [System.Collections.Specialized.OrderedDictionary]$Values
    )
    if ($Values.Contains("system_name")) {
        Set-EspBmcSystemName -Link $Link -Name $Values["system_name"]
        Write-Host "Systemname gesetzt: $($Values['system_name'])"
    }
    if ($Values.Contains("snmp_community") -and $Values.Contains("snmp_rw_community")) {
        Set-EspBmcSnmpCommunity -Link $Link -ReadOnly $Values["snmp_community"] -ReadWrite $Values["snmp_rw_community"]
        Write-Host "SNMP-Communities gesetzt"
    }
    if ($Values.Contains("ntc_temp_max_c") -and $Values.Contains("dht_temp_max_c") -and $Values.Contains("dht_humidity_max_pct")) {
        Set-EspBmcThresholds -Link $Link `
            -NtcMaxC (ConvertTo-InvariantDouble -Value $Values["ntc_temp_max_c"] -FieldName "ntc_temp_max_c") `
            -DhtMaxC (ConvertTo-InvariantDouble -Value $Values["dht_temp_max_c"] -FieldName "dht_temp_max_c") `
            -DhtHumidityMaxPct (ConvertTo-InvariantDouble -Value $Values["dht_humidity_max_pct"] -FieldName "dht_humidity_max_pct")
        Write-Host "Schwellwerte gesetzt"
    }
    if ($Values.Contains("tastschutz")) {
        Set-EspBmcTastschutz -Link $Link -Active ($Values["tastschutz"] -eq "1")
        Write-Host "Tastschutz gesetzt: $($Values['tastschutz'])"
    }
}

function Import-WireguardConfig {
    param([System.IO.Ports.SerialPort]$Link)
    if (-not (Confirm-YesNo "WireGuard-Konfiguration importieren?")) { return }
    $candidates = @(Get-ChildItem -Path . -Filter *.conf -File | Sort-Object Name)
    if ($candidates.Count -eq 0) {
        Write-Host "Keine .conf-Datei im aktuellen Ordner gefunden - ueberspringe."
        return
    }
    if ($candidates.Count -eq 1) {
        $chosen = $candidates[0]
    } else {
        Write-Host "Gefundene .conf-Dateien:"
        for ($i = 0; $i -lt $candidates.Count; $i++) {
            Write-Host "  ${i}: $($candidates[$i].Name)"
        }
        $idx = Read-Host "Index waehlen"
        if ($idx -notmatch '^\d+$' -or [int]$idx -ge $candidates.Count) {
            Write-Host "Ungueltige Auswahl - ueberspringe."
            return
        }
        $chosen = $candidates[[int]$idx]
    }
    $confText = Get-Content $chosen.FullName -Raw
    Send-EspBmcWireguardConfig -Link $Link -ConfText $confText | Out-Null
    Write-Host "WireGuard-Konfiguration `"$($chosen.Name)`" hochgeladen und verbunden."
}

function Set-WlanConfig {
    param([System.IO.Ports.SerialPort]$Link)
    if (-not (Confirm-YesNo "WLAN konfigurieren?")) { return }
    $results = @(Get-EspBmcWlanScan -Link $Link)
    if ($results.Count -eq 0) {
        Write-Host "Keine WLANs gefunden - ueberspringe."
        return
    }
    Write-Host "Gefundene WLANs:"
    foreach ($r in $results) {
        $lock = if ($r.Mode -eq "OPEN") { "" } else { " (verschluesselt)" }
        Write-Host "  $($r.Index): $($r.Ssid)$lock, $($r.Rssi) dBm"
    }
    $idxStr = Read-Host "Index waehlen"
    $chosen = $results | Where-Object { $_.Index -eq $idxStr }
    if (-not $chosen) {
        Write-Host "Ungueltige Auswahl - ueberspringe."
        return
    }
    $psk = ""
    if ($chosen.Mode -ne "OPEN") {
        $secure = Read-Host "WLAN-Passwort fuer `"$($chosen.Ssid)`"" -AsSecureString
        $bstr = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
        try {
            $psk = [System.Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
        } finally {
            [System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
        }
    }
    Join-EspBmcWlan -Link $Link -Index ([int]$idxStr) -Psk $psk
    Write-Host "WLAN-Zugangsdaten fuer `"$($chosen.Ssid)`" gesetzt, Verbindung wird aufgebaut."
}

# --- Ablauf ---

if (-not $SkipFlash -and (Confirm-YesNo "Firmware jetzt flashen?" -DefaultYes $true)) {
    # Flash-Groesse des verbundenen Chips automatisch erkennen und die
    # passende Umgebung waehlen (N16R8 vs. N8R8), statt sich blind auf
    # -EnvName zu verlassen - ein Fehlgriff dort kann zu einem nicht
    # bootenden Geraet fuehren. Wurde -EnvName vom Nutzer EXPLIZIT
    # angegeben, wird das respektiert (nur gewarnt, nicht ueberschrieben) -
    # sonst automatisch auf den erkannten Wert umgeschaltet. Rein
    # informativ/best-effort: schlaegt die Erkennung fehl (kein Geraet
    # verbunden, unbekannte Chipgroesse, esptool.py nicht gefunden), wird
    # nur gewarnt und mit $EnvName wie bisher fortgefahren, kein Abbruch.
    $envNameExplicit = $PSBoundParameters.ContainsKey("EnvName")
    try {
        $detectedSize = Get-EspBmcFlashSize
        $resolvedEnv = Resolve-EspBmcEnvironment -FlashSize $detectedSize
        if (-not $resolvedEnv) {
            Write-Host "Warnung: Erkannte Flash-Groesse ($detectedSize) hat keine passende Umgebung - verwende $EnvName."
        } elseif ($envNameExplicit) {
            if ($resolvedEnv -ne $EnvName) {
                Write-Host "Warnung: Erkannte Flash-Groesse ($detectedSize) passt nicht zur angegebenen Umgebung ($EnvName, erwartet waere $resolvedEnv) - fahre trotzdem mit $EnvName fort (explizit angegeben)."
            }
        } elseif ($resolvedEnv -ne $EnvName) {
            Write-Host "Erkannt: $detectedSize Flash -> verwende Umgebung $resolvedEnv."
            $EnvName = $resolvedEnv
        }
    } catch {
        Write-Host "Warnung: Flash-Groesse konnte nicht automatisch erkannt werden ($_) - verwende $EnvName."
    }

    Invoke-FlashFirmware -Environment $EnvName
    Write-Host "Hinweis: der erste Start nach dem Flashen dauert laenger als jeder folgende (einmalige LittleFS-Formatierung der storage-Partition + SSH-Host-Key-Erzeugung)."
    Read-Host "Flashen abgeschlossen. Geraet neu per USB verbinden und Enter druecken"
}

if (-not $Port) { $Port = Select-ComPort }
Write-Host "`nVerbinde mit $Port ..."
$link = Open-EspBmcLink -PortName $Port
try {
    $role = Connect-EspBmcWithRetry -Link $link -Username $Username -Password $Password
    Write-Host "Angemeldet als $Username (Rolle: $role)"

    $values = Edit-ConfigTemplate -TemplatePath $Template
    Set-DeviceConfig -Link $link -Values $values

    Import-WireguardConfig -Link $link
    Set-WlanConfig -Link $link

    $status = Get-EspBmcStatus -Link $link
    Write-Host "`n=== Aktueller Status ==="
    foreach ($key in $status.Keys) {
        Write-Host "  $key = $($status[$key])"
    }
} finally {
    Close-EspBmcLink -Link $link
}

Write-Host "`nErstinbetriebnahme abgeschlossen."
