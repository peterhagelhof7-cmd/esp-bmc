#requires -Version 5.1
<#
.SYNOPSIS
    Richtet auf DIESEM PC einen ESP-BMC-Serienkonsolen-Dienst ein (einmal pro PC ausfuehren).

.DESCRIPTION
    Legt eine geplante Aufgabe an, die bei JEDEM Systemstart (vor dem Login, als SYSTEM)
    ein Bridge-Skript startet. Das Bridge-Skript sucht anhand der EXAKTEN USB-ID des
    ESP-BMC (nativer USB, VID_303A&PID_1001 - NICHT die feste COM-Nummer, die pro PC
    variiert) den zugehoerigen COM-Port und stellt darueber eine zeilenbasierte
    PowerShell-Konsole dieses PCs bereit. Faellt der Port weg (ESP-Reboot/Umstecken),
    verbindet es sich automatisch neu.

    -> Einmal pro PC als Administrator ausfuehren. Danach ist die Konsole auf dem
       jeweiligen System bei jedem Start dauerhaft verfuegbar - erreichbar ueber die
       Web-/SSH-Konsole des ESP-BMC.

.PARAMETER Deinstallieren
    Entfernt die geplante Aufgabe und das Bridge-Skript wieder.

.NOTES
    SICHERHEIT: Die Konsole laeuft als SYSTEM und bietet vollen PowerShell-Zugriff auf
    diesen PC an JEDEN, der die serielle ESP-BMC-Schnittstelle erreicht. Der Zugang ist
    ausschliesslich durch die (authentifizierte) Web-/SSH-Konsole des ESP-BMC geschuetzt -
    genau wie bei einem Hardware-BMC. Nur in dafuer vorgesehenen, zugangsgeschuetzten
    Umgebungen einsetzen.
#>
param(
    [switch]$Deinstallieren
)

$ErrorActionPreference = 'Stop'

# --- Konstanten -------------------------------------------------------------
$TaskName   = 'ESP-BMC Serial Console'
$InstallDir = Join-Path $env:ProgramData 'ESP-BMC-Console'
$BridgePath = Join-Path $InstallDir 'EspBmcConsoleBridge.ps1'
# Exakte USB-ID des ESP-BMC (nativer USB / USB-Serial-JTAG-CDC des ESP32-S3).
# Bewusst per VID&PID gematcht, damit es unabhaengig von der jeweiligen
# COM-Nummer auf jedem PC den richtigen Port trifft.
$UsbId      = 'VID_303A&PID_1001'

# --- Admin-Pruefung ---------------------------------------------------------
$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
    Write-Host 'Dieses Skript muss als Administrator laufen. Bitte PowerShell "Als Administrator" starten.' -ForegroundColor Red
    exit 1
}

# --- Deinstallation ---------------------------------------------------------
if ($Deinstallieren) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "Geplante Aufgabe '$TaskName' entfernt." -ForegroundColor Green
    } else {
        Write-Host "Keine geplante Aufgabe '$TaskName' gefunden."
    }
    if (Test-Path $InstallDir) {
        Remove-Item -Recurse -Force $InstallDir
        Write-Host "Verzeichnis '$InstallDir' entfernt."
    }
    exit 0
}

# --- Bridge-Skript-Inhalt (wird nach ProgramData geschrieben) ---------------
# Single-quoted Here-String: nichts wird beim Installieren expandiert; alle
# $-Variablen/Backticks gelten erst zur Laufzeit des Bridge-Skripts.
$bridgeSource = @'
#requires -Version 5.1
# ESP-BMC Serienkonsolen-Bridge - laeuft dauerhaft als geplante Aufgabe (SYSTEM).
# Sucht den ESP-BMC per USB-ID (nicht feste COM-Nummer), oeffnet den Port und
# stellt eine zeilenbasierte PowerShell-Konsole bereit; Reconnect bei Portverlust.
$ErrorActionPreference = 'Continue'
$UsbId = 'VID_303A&PID_1001'
$LogFile = Join-Path $env:ProgramData 'ESP-BMC-Console\bridge.log'

function Write-Log([string]$m) {
    try { Add-Content -Path $LogFile -Value ("{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m) -Encoding utf8 } catch {}
}

function Get-EspBmcComPort {
    # Nur die CDC-Schnittstelle des ESP-BMC hat einen COM-Port ("(COMx)" im Namen);
    # das JTAG-Interface derselben USB-ID hat keinen. Erster Treffer gewinnt (im
    # BMC-Einsatz haengt genau ein ESP-BMC am jeweiligen PC).
    $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -match [regex]::Escape($UsbId) -and $_.Name -match '\(COM\d+\)' } |
        Select-Object -First 1
    if ($dev -and $dev.Name -match '\((COM\d+)\)') { return $Matches[1] }
    return $null
}

Write-Log 'Bridge gestartet.'
while ($true) {
    $com = Get-EspBmcComPort
    if (-not $com) { Start-Sleep -Seconds 3; continue }

    $port = $null
    try {
        $port = New-Object System.IO.Ports.SerialPort $com, 115200, 'None', 8, 'One'
        $port.DtrEnable  = $true      # signalisiert dem ESP "Host verbunden"
        $port.RtsEnable  = $false
        $port.ReadTimeout = 500
        $port.Open()
        Write-Log "Verbunden auf $com."

        $prompt = { "PS " + (Get-Location).Path + "> " }
        $port.Write("`r`n=== ESP-BMC-Konsole auf $env:COMPUTERNAME ($([System.Security.Principal.WindowsIdentity]::GetCurrent().Name)) - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===`r`n")
        $port.Write((& $prompt))

        # Zeilen ZEICHENWEISE einlesen: CR, LF ODER CRLF gelten als Zeilenende.
        # Wichtig: PuTTY sendet bei Enter nur ein CR - ReadLine() mit NewLine=LF
        # wuerde die Zeile nie abschliessen (-> "nur Banner, kein Prompt"-Bug).
        # Zusaetzlich lokales Echo (Nutzer sieht seine Eingabe) + Backspace.
        $sb = New-Object System.Text.StringBuilder
        $skipLf = $false
        while ($port.IsOpen) {
            try { $ch = $port.ReadChar() }
            catch [TimeoutException] { continue }        # nur Leerlauf -> Port-Status weiter pruefen
            catch { throw }                               # echter I/O-Fehler -> reconnect
            $c = [char]$ch

            if ($c -eq "`n" -and $skipLf) { $skipLf = $false; continue }  # LF direkt nach CR schlucken (CRLF)
            $skipLf = $false

            if ($c -eq "`r" -or $c -eq "`n") {           # Zeilenende (egal ob CR, LF oder CRLF)
                if ($c -eq "`r") { $skipLf = $true }
                $line = $sb.ToString().Trim(" `t")
                [void]$sb.Clear()
                $port.Write("`r`n")                       # Zeilenumbruch-Echo
                if ($line -eq '') { $port.Write((& $prompt)); continue }
                if ($line -in @('exit', 'quit', 'logout')) {
                    $port.Write("(Konsole bleibt bestehen)`r`n" + (& $prompt)); continue
                }
                try {
                    $out = Invoke-Expression $line 2>&1 | Out-String
                } catch {
                    $out = "FEHLER: " + $_.Exception.Message + "`r`n"
                }
                # LF -> CRLF fuer saubere Terminaldarstellung
                $out = ($out -replace "`r`n", "`n") -replace "`n", "`r`n"
                $port.Write($out)
                $port.Write((& $prompt))
                continue
            }

            if ($c -eq [char]8 -or $c -eq [char]127) {    # Backspace / DEL
                if ($sb.Length -gt 0) { [void]$sb.Remove($sb.Length - 1, 1); $port.Write("`b `b") }
                continue
            }

            [void]$sb.Append($c)
            $port.Write($c)                               # lokales Echo des Zeichens
        }
    } catch {
        Write-Log ("Fehler/Trennung: " + $_.Exception.Message)
    } finally {
        if ($port) { try { if ($port.IsOpen) { $port.Close() } } catch {}; try { $port.Dispose() } catch {} }
    }
    Start-Sleep -Seconds 2   # kurz warten, dann Port neu suchen (ESP-Reboot/Umstecken)
}
'@

# --- Installieren -----------------------------------------------------------
Write-Host "Installiere ESP-BMC-Konsole (USB-ID $UsbId) ..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Set-Content -Path $BridgePath -Value $bridgeSource -Encoding UTF8
Write-Host "  Bridge-Skript: $BridgePath"

# Syntax-Check des geschriebenen Bridge-Skripts (frueh scheitern, nicht erst beim Boot).
$null = [System.Management.Automation.Language.Parser]::ParseFile($BridgePath, [ref]$null, [ref]$null)

# Vorhandene Aufgabe ersetzen (idempotent).
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}

# Evtl. noch laufende Bridge-Prozesse beenden (auch verwaiste ohne Task),
# damit der neue Prozess den COM-Port sauber uebernehmen kann.
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match 'EspBmcConsoleBridge\.ps1' } |
    ForEach-Object {
        try { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop; Write-Host "  Alten Bridge-Prozess (PID $($_.ProcessId)) beendet." }
        catch { Write-Host "  Konnte Bridge-Prozess (PID $($_.ProcessId)) nicht beenden: $($_.Exception.Message)" -ForegroundColor Yellow }
    }

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$BridgePath`""
$trigger   = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -StartWhenAvailable -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Principal $principal -Settings $settings `
    -Description 'Stellt eine PowerShell-Konsole dieses PCs ueber die serielle Schnittstelle des ESP-BMC bereit (per USB-ID, Auto-Reconnect).' | Out-Null
Write-Host "  Geplante Aufgabe '$TaskName' registriert (Trigger: Systemstart, als SYSTEM)."

# Sofort starten, damit die Konsole ohne Neustart schon laeuft.
Start-ScheduledTask -TaskName $TaskName
Write-Host "  Aufgabe gestartet." -ForegroundColor Green

Write-Host ""
Write-Host "Fertig. Die Konsole ist ab jetzt bei jedem Start aktiv und haengt sich" -ForegroundColor Green
Write-Host "automatisch auf den ESP-BMC-Port (USB $UsbId), sobald dieser erkannt wird." -ForegroundColor Green
Write-Host "Log: $(Join-Path $InstallDir 'bridge.log')"
Write-Host "Deinstallieren: .\Install-EspBmcConsole.ps1 -Deinstallieren"
