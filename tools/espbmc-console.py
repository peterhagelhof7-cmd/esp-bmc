#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP-BMC Serienkonsolen-Bridge - LINUX-Gegenstueck zu Install-EspBmcConsole.ps1.

Stellt auf DIESEM Linux-Host eine zeilenbasierte Shell ueber die serielle
Schnittstelle des ESP-BMC bereit - genau wie das Windows/PowerShell-Pendant,
nur mit /bin/sh statt PowerShell und systemd statt Scheduled Task.

Der ESP-BMC wird per EXAKTER USB-ID (nativer USB / USB-Serial-JTAG-CDC des
ESP32-S3, VID 303a PID 1001) gefunden - NICHT ueber einen festen Geraetenamen,
der sich pro Host aendert. Faellt der Port weg (ESP-Reboot/Umstecken), verbindet
sich die Bridge automatisch neu.

Nur Python-3-Standardbibliothek, keine externen Pakete (kein pyserial noetig).

Verwendung:
    sudo ./espbmc-console.py install     # systemd-Dienst einrichten (Autostart)
    sudo ./espbmc-console.py uninstall   # Dienst + installierte Kopie entfernen
    ./espbmc-console.py run              # Bridge im Vordergrund starten (Test)

SICHERHEIT: Die Konsole bietet vollen Shell-Zugriff (als root, wenn per systemd
installiert) auf diesen Host an JEDEN, der die serielle ESP-BMC-Schnittstelle
erreicht. Geschuetzt ist der Zugang ausschliesslich durch die (authentifizierte)
Web-/SSH-Konsole des ESP-BMC - genau wie bei einem Hardware-BMC. Nur in dafuer
vorgesehenen, zugangsgeschuetzten Umgebungen einsetzen.
"""

import os
import re
import sys
import glob
import time
import fcntl
import struct
import socket
import termios
import subprocess
from datetime import datetime

# --- Konstanten -------------------------------------------------------------
USB_VID = "303a"          # ESP32-S3 USB-Serial-JTAG
USB_PID = "1001"
BAUD = termios.B115200

SERVICE_NAME = "esp-bmc-console.service"
SERVICE_PATH = "/etc/systemd/system/" + SERVICE_NAME
INSTALL_PATH = "/usr/local/sbin/espbmc-console.py"
LOG_FILE = "/var/log/esp-bmc-console.log"

# ioctl-Konstanten zum Setzen der Modemleitungen (DTR signalisiert dem ESP
# "Host verbunden", analog zu DtrEnable=$true im PowerShell-Pendant).
TIOCMBIS = getattr(termios, "TIOCMBIS", 0x5416)  # Bits setzen
TIOCMBIC = getattr(termios, "TIOCMBIC", 0x5417)  # Bits loeschen
TIOCM_DTR = getattr(termios, "TIOCM_DTR", 0x002)
TIOCM_RTS = getattr(termios, "TIOCM_RTS", 0x004)

# ESP-IDF-Log-/Boot-Zeilen (z.B. "I (983) esp_psram: ...") sickern ueber
# dieselbe USB-Serial-JTAG-Leitung in die Konsole. Nicht als Befehl ausfuehren.
IDF_LOG_RE = re.compile(rb"^[EWIDV] \(\d+\)")


def log(msg):
    line = "%s  %s" % (datetime.now().strftime("%Y-%m-%d %H:%M:%S"), msg)
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass
    # Bei Vordergrundstart (run) zusaetzlich nach stderr, damit man was sieht.
    if sys.stderr and sys.stderr.isatty():
        print(line, file=sys.stderr)


def current_user():
    try:
        import pwd
        return pwd.getpwuid(os.getuid()).pw_name
    except Exception:
        return os.environ.get("USER") or "unbekannt"


# --- Port-Findung -----------------------------------------------------------
def find_espbmc_port():
    """Sucht das tty des ESP-BMC anhand der USB-VID/PID in sysfs.

    Nur die CDC-ACM-Schnittstelle des ESP-BMC hat ein /dev/ttyACM*; das
    JTAG-Interface derselben USB-ID hat keins. Erster Treffer gewinnt (im
    BMC-Einsatz haengt genau ein ESP-BMC am jeweiligen Host).
    """
    for dev in sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")):
        base = os.path.basename(dev)
        p = os.path.realpath("/sys/class/tty/%s/device" % base)
        # Vom tty-Geraet in der USB-Topologie nach oben laufen, bis idVendor/
        # idProduct gefunden sind (dort sitzt das USB-Interface-Elternteil).
        for _ in range(6):
            vfile = os.path.join(p, "idVendor")
            pfile = os.path.join(p, "idProduct")
            if os.path.exists(vfile) and os.path.exists(pfile):
                try:
                    vid = open(vfile).read().strip().lower()
                    pid = open(pfile).read().strip().lower()
                except OSError:
                    break
                if vid == USB_VID and pid == USB_PID:
                    return dev
                break
            parent = os.path.dirname(p)
            if parent == p:
                break
            p = parent
    return None


def open_serial(dev):
    """Oeffnet den Port roh mit 115200 8N1, hebt DTR an, senkt RTS."""
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    # Zurueck auf blockierendes Lesen mit VTIME-Timeout (s.u.).
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl & ~os.O_NONBLOCK)

    attrs = termios.tcgetattr(fd)
    # [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
    attrs[0] = 0                                   # iflag: keine Eingangsverarbeitung
    attrs[1] = 0                                   # oflag: keine Ausgangsverarbeitung (roh)
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # 8N1, RX an, Modemleitungen fuers Oeffnen ignorieren
    attrs[3] = 0                                   # lflag: kein Echo/Canonical
    attrs[4] = BAUD                                # ispeed
    attrs[5] = BAUD                                # ospeed
    attrs[6] = list(attrs[6])
    attrs[6][termios.VMIN] = 0                     # nicht auf ein Mindestbyte warten...
    attrs[6][termios.VTIME] = 5                    # ...sondern 0,5 s Lese-Timeout (Leerlauf erkennen)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

    # DTR anheben (Host-verbunden-Signal), RTS senken - wie DtrEnable/RtsEnable im PS-Pendant.
    fcntl.ioctl(fd, TIOCMBIS, struct.pack("I", TIOCM_DTR))
    fcntl.ioctl(fd, TIOCMBIC, struct.pack("I", TIOCM_RTS))
    return fd


# --- Befehlsausfuehrung -----------------------------------------------------
def run_command(line, state):
    """Fuehrt eine Eingabezeile aus. 'cd' wird intern behandelt, damit das
    Arbeitsverzeichnis ueber Befehle hinweg erhalten bleibt (wie Get-Location
    im PS-Pendant); alles andere laeuft ueber /bin/sh -c im aktuellen cwd."""
    parts = line.split(None, 1)
    if parts and parts[0] == "cd":
        arg = parts[1].strip() if len(parts) > 1 else os.path.expanduser("~")
        target = arg if os.path.isabs(arg) else os.path.join(state["cwd"], arg)
        target = os.path.normpath(target)
        if os.path.isdir(target):
            state["cwd"] = target
            return b""
        return ("cd: %s: Kein Verzeichnis\n" % arg).encode("utf-8")
    try:
        r = subprocess.run(
            ["/bin/sh", "-c", line],
            cwd=state["cwd"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=120,
        )
        return r.stdout
    except subprocess.TimeoutExpired:
        return b"FEHLER: Zeitueberschreitung (120 s)\n"
    except Exception as exc:  # noqa: BLE001 - Konsole soll nie sterben
        return ("FEHLER: %s\n" % exc).encode("utf-8")


def to_crlf(data):
    """LF -> CRLF fuer saubere Terminaldarstellung (idempotent)."""
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def make_prompt(state):
    return ("%s@%s:%s$ " % (state["user"], state["host"], state["cwd"])).encode("utf-8")


def session(fd, state):
    """Zeilenbasierte Konsole ueber den offenen Port. Kehrt bei Portverlust/
    -fehler zurueck (dann Reconnect durch den Aufrufer)."""
    banner = "\r\n=== ESP-BMC-Konsole auf %s (%s) - %s ===\r\n" % (
        state["host"], state["user"], datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    os.write(fd, banner.encode("utf-8"))
    os.write(fd, make_prompt(state))

    buf = bytearray()
    skip_lf = False
    while True:
        try:
            chunk = os.read(fd, 1)
        except OSError as exc:
            log("Lesefehler/Trennung: %s" % exc)
            return
        if not chunk:
            continue  # Leerlauf-Timeout -> weiter (Port-Status prueft der Aufrufer beim Reconnect)
        c = chunk[0:1]

        # CRLF: LF direkt nach CR schlucken.
        if c == b"\n" and skip_lf:
            skip_lf = False
            continue
        skip_lf = False

        if c == b"\r" or c == b"\n":                 # Zeilenende (CR, LF oder CRLF)
            if c == b"\r":
                skip_lf = True
            try:
                line = bytes(buf).decode("utf-8", "replace").strip(" \t")
            except Exception:
                line = ""
            del buf[:]
            os.write(fd, b"\r\n")                     # Zeilenumbruch-Echo
            if line == "":
                os.write(fd, make_prompt(state))
                continue
            if line in ("exit", "quit", "logout"):
                os.write(fd, b"(Konsole bleibt bestehen)\r\n" + make_prompt(state))
                continue
            # Asynchrone ESP-IDF-Logzeilen still verwerfen (kein Prompt).
            if IDF_LOG_RE.match(line.encode("utf-8", "replace")):
                continue
            out = run_command(line, state)
            if out:
                os.write(fd, to_crlf(out))
            os.write(fd, make_prompt(state))
            continue

        if c == b"\x08" or c == b"\x7f":             # Backspace / DEL
            if buf:
                del buf[-1:]
                os.write(fd, b"\b \b")
            continue

        buf += c
        os.write(fd, c)                              # lokales Echo


def bridge_loop():
    state = {"cwd": os.path.expanduser("~") if os.path.isdir(os.path.expanduser("~")) else "/",
             "host": socket.gethostname(),
             "user": current_user()}
    log("Bridge gestartet (Suche USB %s:%s)." % (USB_VID, USB_PID))
    while True:
        dev = find_espbmc_port()
        if not dev:
            time.sleep(3)
            continue
        fd = None
        try:
            fd = open_serial(dev)
            log("Verbunden auf %s." % dev)
            session(fd, state)
        except OSError as exc:
            log("Fehler/Trennung auf %s: %s" % (dev, exc))
        finally:
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        time.sleep(2)  # kurz warten, dann Port neu suchen (ESP-Reboot/Umstecken)


# --- Installation (systemd) -------------------------------------------------
SERVICE_UNIT = """\
[Unit]
Description=ESP-BMC serial console bridge (host shell over ESP-BMC serial)
Documentation=https://github.com/peterhagelhof7-cmd/esp-bmc
After=multi-user.target

[Service]
Type=simple
ExecStart=/usr/bin/env python3 {install_path} run
Restart=always
RestartSec=3
User=root

[Install]
WantedBy=multi-user.target
"""


def require_root(action):
    if os.geteuid() != 0:
        sys.exit("'%s' muss als root laufen. Bitte mit sudo erneut ausfuehren." % action)


def systemctl(*args):
    subprocess.run(["systemctl", *args], check=True)


def do_install():
    require_root("install")
    print("Installiere ESP-BMC-Konsole (USB %s:%s) ..." % (USB_VID, USB_PID))
    # Dieses Skript an einen stabilen Ort kopieren (Quelle kann irgendwo liegen).
    src = os.path.realpath(__file__)
    os.makedirs(os.path.dirname(INSTALL_PATH), exist_ok=True)
    if src != os.path.realpath(INSTALL_PATH):
        with open(src, "rb") as fsrc, open(INSTALL_PATH, "wb") as fdst:
            fdst.write(fsrc.read())
    os.chmod(INSTALL_PATH, 0o755)
    print("  Bridge-Skript: %s" % INSTALL_PATH)

    with open(SERVICE_PATH, "w", encoding="utf-8") as f:
        f.write(SERVICE_UNIT.format(install_path=INSTALL_PATH))
    print("  systemd-Unit:  %s" % SERVICE_PATH)

    systemctl("daemon-reload")
    systemctl("enable", "--now", SERVICE_NAME)
    print("  Dienst aktiviert und gestartet (Trigger: Boot, als root).")
    print("")
    print("Fertig. Die Konsole ist ab jetzt bei jedem Start aktiv und haengt sich")
    print("automatisch auf den ESP-BMC-Port (USB %s:%s), sobald er erkannt wird." % (USB_VID, USB_PID))
    print("Log:  %s   (oder: journalctl -u %s -f)" % (LOG_FILE, SERVICE_NAME))
    print("Deinstallieren:  sudo %s uninstall" % INSTALL_PATH)


def do_uninstall():
    require_root("uninstall")
    if os.path.exists(SERVICE_PATH):
        subprocess.run(["systemctl", "disable", "--now", SERVICE_NAME], check=False)
        os.remove(SERVICE_PATH)
        subprocess.run(["systemctl", "daemon-reload"], check=False)
        print("Dienst '%s' entfernt." % SERVICE_NAME)
    else:
        print("Kein Dienst '%s' gefunden." % SERVICE_NAME)
    if os.path.exists(INSTALL_PATH):
        os.remove(INSTALL_PATH)
        print("Bridge-Skript '%s' entfernt." % INSTALL_PATH)


USAGE = """ESP-BMC Serienkonsolen-Bridge (Linux-Gegenstueck zu Install-EspBmcConsole.ps1)

  sudo ./espbmc-console.py install     systemd-Dienst einrichten (Autostart, root)
  sudo ./espbmc-console.py uninstall   Dienst + installierte Kopie entfernen
       ./espbmc-console.py run         Bridge im Vordergrund starten (Test/Debug)
"""


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "run":
        bridge_loop()
    elif cmd == "install":
        do_install()
    elif cmd == "uninstall":
        do_uninstall()
    else:
        print(USAGE)
        sys.exit(0 if cmd in ("", "-h", "--help", "help") else 2)


if __name__ == "__main__":
    main()
