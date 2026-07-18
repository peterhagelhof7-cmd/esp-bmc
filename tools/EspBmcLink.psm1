# Client fuer das ESP-BMC USB-Kommandoprotokoll (##ESPR ..., siehe
# firmware/components/usb_manager/usb_manager.c und
# docs/entscheidungen.md "USB-Kommandoprotokoll").
#
# Nur das Protokoll selbst - keine Interaktivitaet. Wird von Setup.ps1
# benutzt (Import-Module .\EspBmcLink.psm1), kann aber auch eigenstaendig
# importiert werden.

Set-StrictMode -Version Latest

# Fragt vor dem Flashen die tatsaechlich verbaute Flash-Groesse des
# verbundenen Chips ab (esptool.py flash_id spricht den ROM-Bootloader
# direkt an, kein vorheriges Firmware-Image noetig - funktioniert also
# auch auf einem leeren/unbekannten Chip). Damit kann Setup.ps1
# automatisch die passende PlatformIO-Umgebung waehlen (N16R8 vs. N8R8,
# siehe docs/entscheidungen.md "Portierbarkeit auf ESP32-S3-DevKitC-1-
# N8R8") statt sich auf einen von Hand gewaehlten -EnvName-Parameter zu
# verlassen - ein Fehlgriff dort (z.B. N16R8-Firmware auf einen
# 8-MB-Chip) kann zu einem nicht bootenden Geraet fuehren.
#
# -Port ist bewusst optional: esptool.py kann den Port selbst suchen
# (gleiches Auto-Erkennungsverhalten wie "pio run -t upload", das in
# diesem Skript ebenfalls nie einen expliziten Port braucht).
function Get-EspBmcFlashSize {
    [OutputType([string])]
    param([string]$Port)

    $pythonExe = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
    $esptoolPy = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"
    if (-not (Test-Path $pythonExe) -or -not (Test-Path $esptoolPy)) {
        throw "esptool.py nicht gefunden ($esptoolPy) - ist PlatformIO installiert?"
    }

    $esptoolArgs = @()
    if ($Port) { $esptoolArgs += @("--port", $Port) }
    $esptoolArgs += "flash_id"

    $output = & $pythonExe $esptoolPy @esptoolArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "esptool.py flash_id fehlgeschlagen (Exit $LASTEXITCODE):`n$output"
    }
    $match = $output | Select-String -Pattern 'Detected flash size:\s*(\S+)'
    if (-not $match) {
        throw "Flash-Groesse nicht in der esptool-Ausgabe gefunden:`n$output"
    }
    return $match.Matches[0].Groups[1].Value
}

# Ordnet eine von esptool erkannte Flash-Groesse (z.B. "8MB", "16MB" -
# exakt dieselben Werte wie board_upload.flash_size in platformio.ini)
# der passenden PlatformIO-Umgebung zu. $null, falls keine unserer
# beiden gepflegten Varianten passt (z.B. ein ganz anderes Board).
function Resolve-EspBmcEnvironment {
    [OutputType([string])]
    param([Parameter(Mandatory)][string]$FlashSize)

    switch ($FlashSize) {
        "16MB" { return "esp32-s3-devkitc-1-n16r8" }
        "8MB" { return "esp32-s3-devkitc-1-n8r8" }
        default { return $null }
    }
}

function Open-EspBmcLink {
    [OutputType([System.IO.Ports.SerialPort])]
    param(
        [Parameter(Mandatory)][string]$PortName,
        [int]$Baud = 115200  # bei nativem USB-CDC vom Geraet ignoriert, .NET braucht trotzdem einen Wert
    )
    $link = New-Object System.IO.Ports.SerialPort $PortName, $Baud
    $link.NewLine = "`r`n"
    $link.Encoding = [System.Text.Encoding]::UTF8
    $link.ReadTimeout = 5000
    $link.WriteTimeout = 5000
    $link.Open()
    $link.DtrEnable = $true  # wie ein echtes Terminalprogramm
    Start-Sleep -Milliseconds 300
    $link.DiscardInBuffer()
    return $link
}

function Close-EspBmcLink {
    param([Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link)
    if ($Link.IsOpen) { $Link.Close() }
}

function Read-EspBmcResponse {
    param([Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link)
    $ok = $null
    $detail = ""
    $payload = @()
    while ($true) {
        $line = $Link.ReadLine()
        if ($line.StartsWith("##ESPR ")) {
            $meta = $line.Substring(7)
            if ($meta -eq "END") { break }
            if ($meta.StartsWith("OK")) {
                $ok = $true
                $detail = if ($meta.Length -gt 2) { $meta.Substring(3) } else { "" }
            } elseif ($meta.StartsWith("ERR")) {
                $ok = $false
                $detail = if ($meta.Length -gt 3) { $meta.Substring(4) } else { "" }
            }
            # unbekannte Metazeile ignorieren, nicht abbrechen
        } else {
            $payload += $line
        }
    }
    if ($null -eq $ok) { throw "Antwort ohne OK/ERR erhalten" }
    return [PSCustomObject]@{ Ok = $ok; Detail = $detail; Payload = $payload }
}

function Invoke-EspBmcCommand {
    param(
        [Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link,
        [Parameter(Mandatory)][string]$Command,
        [switch]$IgnoreError
    )
    $Link.Write("##ESPR $Command`r`n")
    $resp = Read-EspBmcResponse -Link $Link
    if (-not $resp.Ok -and -not $IgnoreError) {
        throw "`"$Command`" fehlgeschlagen: $($resp.Detail)"
    }
    return $resp
}

# wg upload braucht Rohdaten-Uebertragung (erst Laenge ankuendigen, dann
# exakt so viele Rohbytes ohne Zeilenpufferung - siehe
# cmd_wg_upload_start()/finish_wg_upload() in usb_manager.c).
function Send-EspBmcWireguardConfig {
    param(
        [Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link,
        [Parameter(Mandatory)][string]$ConfText
    )
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($ConfText)
    $Link.Write("##ESPR wg upload $($bytes.Length)`r`n")
    $Link.Write($bytes, 0, $bytes.Length)
    $resp = Read-EspBmcResponse -Link $Link
    if (-not $resp.Ok) {
        throw "wg upload fehlgeschlagen: $($resp.Detail)"
    }
    return $resp
}

# --- Komfort-Wrapper um haeufige Kommandos ---

function Connect-EspBmc {
    [OutputType([string])]
    param(
        [Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link,
        [Parameter(Mandatory)][string]$Username,
        [Parameter(Mandatory)][string]$Password
    )
    $resp = Invoke-EspBmcCommand -Link $Link -Command "login $Username $Password"
    return $resp.Detail.Trim()
}

function Get-EspBmcStatus {
    param([Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link)
    $resp = Invoke-EspBmcCommand -Link $Link -Command "status"
    $out = [ordered]@{}
    foreach ($line in $resp.Payload) {
        foreach ($token in ($line -split '\s+')) {
            if ($token -match '^([^=]+)=(.*)$') {
                $out[$matches[1]] = $matches[2]
            }
        }
    }
    return $out
}

function Get-EspBmcWlanScan {
    param([Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link)
    $resp = Invoke-EspBmcCommand -Link $Link -Command "wlan scan"
    $results = @()
    foreach ($line in $resp.Payload) {
        $parts = $line -split ';'
        if ($parts.Count -eq 4) {
            $results += [PSCustomObject]@{
                Index = $parts[0]
                Ssid  = $parts[1]
                Mode  = $parts[2]
                Rssi  = $parts[3]
            }
        }
    }
    return $results
}

function Join-EspBmcWlan {
    param(
        [Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link,
        [Parameter(Mandatory)][int]$Index,
        [string]$Psk = ""
    )
    Invoke-EspBmcCommand -Link $Link -Command ("wlan join $Index $Psk").TrimEnd() | Out-Null
}

function Set-EspBmcSystemName {
    param(
        [Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link,
        [Parameter(Mandatory)][string]$Name
    )
    Invoke-EspBmcCommand -Link $Link -Command "system set $Name" | Out-Null
}

function Set-EspBmcSnmpCommunity {
    param(
        [Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link,
        [Parameter(Mandatory)][string]$ReadOnly,
        [Parameter(Mandatory)][string]$ReadWrite
    )
    Invoke-EspBmcCommand -Link $Link -Command "snmp set $ReadOnly $ReadWrite" | Out-Null
}

function Set-EspBmcThresholds {
    param(
        [Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link,
        [Parameter(Mandatory)][double]$NtcMaxC,
        [Parameter(Mandatory)][double]$DhtMaxC,
        [Parameter(Mandatory)][double]$DhtHumidityMaxPct
    )
    Invoke-EspBmcCommand -Link $Link -Command "thresholds set $NtcMaxC $DhtMaxC $DhtHumidityMaxPct" | Out-Null
}

function Set-EspBmcTastschutz {
    param(
        [Parameter(Mandatory)][System.IO.Ports.SerialPort]$Link,
        [Parameter(Mandatory)][bool]$Active
    )
    $val = if ($Active) { 1 } else { 0 }
    Invoke-EspBmcCommand -Link $Link -Command "tastschutz set $val" | Out-Null
}

Export-ModuleMember -Function *
