#!/usr/bin/env python3
"""
Rockbox Installer for iPod Nano 3G (N46 / S5L8702)

A self-contained Windows installer. Everything it needs is bundled: the
mks5lboot DFU/NOR tool, the Rockbox bootloader images, the complete .rockbox
tree, and Zadig for USB driver setup.

WHY THE STEP ORDER MATTERS
--------------------------
The NOR write happens LAST. Before that, the Rockbox bootloader is run
*temporarily* from DFU -- volatile, nothing is written to the device -- purely
so the raw NAND is exposed to Windows as a mass-storage disk through Rockbox's
own flash translation layer. Only once the filesystem is formatted, the files
copied and verified, and the volume flushed, is the persistent bootloader
written to NOR.

Two benefits: a failure at any earlier stage leaves NOR untouched and the
device exactly as it was, and when the patch does land the device immediately
boots a working Rockbox instead of coming up to
"Can't load rockbox.ipod: File not found".

IMPORTANT, AND NOT REVERSIBLE WITHOUT iTUNES
--------------------------------------------
Rockbox's FTL uses its own on-flash format, incompatible with Apple's.
Installing reformats the NAND and therefore destroys Apple's OS, which lives
on the NAND (NOR is only 1 MB and holds just Apple's 128 KB bootloader).

Original work for this project. GPLv2 or later, as Rockbox.
"""

import ctypes
import ctypes.wintypes as wintypes
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from datetime import datetime
from tkinter import messagebox, ttk

import os as _os

APP_NAME = "Rockbox Installer for iPod Nano 3G"
# Used by Windows to associate the running process with this app's taskbar
# identity and its packaged icon instead of Python's default icon.
APP_ID = "org.rockbox.ipodnano3.installer"
# Single source of truth for the app version. Bump this on every rebuild that
# changes behaviour, and the PyInstaller spec derives the EXE filename from it
# (via APP_VERSION here) so the two can never drift apart again.
#   2.0.0  first page-level (v2 FTL) build
#   2.1.0  Hynix 8GB-unit support (FTL bound fix + "Apple Hynix" disk match),
#          pop-up-free install/uninstall, streamlined text
#   2.2.0  Toshiba 8GB-unit support (validated 4KB-page MLC, "Apple Toshiba"
#          disk match)
#   2.2.1  bundle the eraser rebuilt with the current chip table (a stale
#          eraser reported "no banks found" on a newly validated chip);
#          DFU/completion wording tweaks
#   2.3.0  Hynix 4GB MA978 support (validated D3 / A5 MLC, exact-table row)
#   2.3.1  rebuild the bundled full-NAND eraser after adding the D3 chip row,
#          so Uninstall recognises and erases MA978 units too
#   2.3.2  clarify the two DFU sessions during Uninstall and keep the progress
#          status truthful until the NAND eraser is actually running
#   2.3.3  compact the header and use the Rockbox Nano 3 icon consistently in
#          the EXE, title bar, Alt-Tab and taskbar
#   2.3.4  tighten all main-window margins, remove Open log, and add the
#          app/version and creator footer
#   2.3.5  track the full on-device NAND erase during Uninstall and complete
#          the progress bar only when the restore is ready to reboot
#   2.3.6  compact the control row and window footprint; align Zadig with the
#          primary buttons and standardize the main-window margins
#   2.3.7  remove the log scrollbar; scroll the log with the mouse wheel /
#          trackpad instead
#   2.3.8  add a full diagnostic file logger (single log file on the Desktop)
#          that records every command, its full output, and disk/PnP
#          enumeration snapshots around the temporary-bootloader step, to
#          diagnose installs that stall waiting for the iPod to appear as a disk
#   2.3.9  extend USB-stall diagnostics with active 05AC:1262 PnP node
#          properties, bound driver/service details, and recent USB/Kernel-PnP
#          system events; distinguishes an Apple-driver collision from a USB
#          bulk-storage transport failure
#   2.4.0  extend the read-only NAND-check diagnostics with before/after
#          disk snapshots and detailed CreateFile/ReadFile results for the
#          report sector, to isolate a transient check disk that disconnects
#   2.4.1  rebuild the previously stale NAND-check image from current source;
#          fix deep PnP diagnostic capture so it records the active device's
#          driver/service properties instead of a PowerShell parse error
#   2.4.2  change Nano 3G Rockbox USB identity from Apple's retail 05AC:1262
#          to Rockbox transport 05AC:127F; prevents iTunes' AppleIPod driver
#          from binding and causing USBSTOR Code 10 on affected Windows PCs
APP_VERSION = "2.4.2"
# Optional FTL-variant tag, set at build time for the page-level v2 build so
# the window title makes clear which FTL the payload installs (the on-flash
# formats are incompatible, so this is worth surfacing). Empty for the default
# v1 build, leaving its appearance unchanged. Frozen builds bake it in via a
# sibling _ftlvariant.txt written by the build; otherwise the env var is read.
def _detect_ftl_variant():
    try:
        base = getattr(__import__("sys"), "_MEIPASS", _os.path.dirname(__file__))
        tag_file = _os.path.join(base, "_ftlvariant.txt")
        if _os.path.isfile(tag_file):
            with open(tag_file, "r", encoding="utf-8") as fh:
                return fh.read().strip()
    except Exception:
        pass
    return _os.environ.get("NANO3G_FTL_VARIANT", "")

FTL_VARIANT = _detect_ftl_variant()

# Light palette. Deliberately soft: this tool performs destructive, irreversible
# operations, so the warning panel should read as a clear notice rather than as
# alarm-styling competing with the rest of the window.
BG          = "#f4f5f7"   # window
BG_HEAD     = "#ffffff"   # header strip
BG_PANEL    = "#ffffff"   # log / panels
BG_WARN     = "#fdf3f3"   # warning panel
BORDER      = "#dfe1e5"
BORDER_WARN = "#e8c4c4"
FG          = "#1f2328"   # primary text
FG_MUTED    = "#6b7280"   # secondary text
FG_WARN     = "#a12d2d"   # warning heading
FG_WARN2    = "#8a3a3a"   # warning body

# Log line colours, chosen to stay legible on a white background.
LOG_FG    = "#24292f"
LOG_OK    = "#1a7f37"
LOG_WARN  = "#9a6700"
LOG_ERR   = "#b42318"
LOG_STEP  = "#0b62d0"

# The disk this installer is permitted to touch. Every destructive step
# re-checks all three properties, so a disk number changing between steps can
# never cause the wrong device to be formatted.
#
# DISK_NAMES come from Rockbox's own SCSI inquiry strings: nand_get_info() in
# nand-nano3g.c reports vendor "Apple" and product = the NAND maker name, so a
# unit running the Rockbox FTL shows up as "Apple <maker>". Which maker depends
# on the NAND soldered into that particular unit. Measured on hardware:
#
#   Rockbox FTL (Samsung unit)  Apple Samsung   8321499136 B   4096 B sectors
#   Rockbox FTL (Hynix unit)    Apple Hynix     8256000000 B   4096 B sectors
#   Rockbox FTL (Toshiba unit)  Apple Toshiba   ~2GiB*4 die    4096 B sectors
#   Apple's OS (any unit)       Apple iPod      7952142336 B   4096 B sectors  MBR
#
# The name is the *only* usable discriminator (sector size is identical across
# all three, and every size falls inside the safety range below). So this is an
# EXACT match against a fixed allow-list of the Rockbox-FTL product strings for
# the makers this installer supports -- NOT a substring or prefix match. That
# keeps the critical safety property intact: "Apple iPod" (Apple's own FTL) is
# never in the list, so we never format/copy through Apple's FTL (where Rockbox
# cannot see the result and the install would silently boot to "Can't load
# rockbox.ipod: File not found"). Each name here corresponds to a validated
# chip in nand_vendor.c's nano3g_validated_chips[]; add a maker here only when
# its chip has been hardware-validated and added there too.
DISK_NAMES = ("Apple Samsung", "Apple Hynix", "Apple Toshiba")
DISK_BUS = "USB"
DISK_MIN = 1 * 1024**3
DISK_MAX = 20 * 1024**3

# DFU states. 2 = dfuIDLE, which mks5lboot needs before a download.
# 9 = dfuERROR, which is what wInd3x's "Haxed DFU" leaves behind; --dfureset
# does not clear it, only a power cycle back into stock DFU does.
DFU_IDLE = 2

# How the user enters DFU, shown in the LOG (not a pop-up) wherever a flow
# needs it, then wait_dfu_idle() auto-detects DFU and continues on its own.
# The physical gesture is identical from every state the device can be in when
# we ask (idle/charging, running Apple's OS, running the temporary Rockbox
# bootloader in USB disk mode, or running the eraser): keep it plugged in and
# hold MENU + SELECT -- that reboots the iPod and it then drops into DFU,
# screen fully black. Never tell the user to eject/unplug; the cable stays
# connected the whole time.

CREATE_NO_WINDOW = 0x08000000


# --------------------------------------------------------------------------
# bundled resources
# --------------------------------------------------------------------------

def resource_dir() -> str:
    """Directory holding bundled files, whether frozen by PyInstaller or not."""
    if getattr(sys, "frozen", False):
        return sys._MEIPASS  # type: ignore[attr-defined]
    return os.path.dirname(os.path.abspath(__file__))


def resource(*parts: str) -> str:
    return os.path.join(resource_dir(), *parts)


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------

def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


# --------------------------------------------------------------------------
# Diagnostic file logger
# --------------------------------------------------------------------------
#
# Writes a complete, timestamped transcript to a file the user can send back.
# This is separate from the on-screen log (which is summarised): the file
# captures every command executed, its exit code and FULL output, plus disk /
# PnP enumeration snapshots around the "temporary bootloader" step. That step
# depends on Windows enumerating the iPod as a mass-storage disk, which varies
# by PC, so the snapshots are what let us see why find_disk() may not match on
# a machine where the install stalls.

def _default_log_path():
    """Pick a visible, writable location for the diagnostic log."""
    candidates = []
    up = os.environ.get("USERPROFILE")
    if up:
        candidates.append(os.path.join(up, "Desktop"))
        candidates.append(up)
    candidates.append(os.environ.get("TEMP") or os.getcwd())
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    name = f"Nano3RockboxInstaller-log-{stamp}.txt"
    for folder in candidates:
        try:
            if folder and os.path.isdir(folder):
                return os.path.join(folder, name)
        except Exception:
            pass
    return os.path.join(os.getcwd(), name)


class DiagLogger:
    """Thread-safe append-only file logger. One instance per app run."""

    def __init__(self, path=None):
        self.path = path or _default_log_path()
        self._lock = threading.Lock()
        self.enabled = True
        try:
            with open(self.path, "w", encoding="utf-8") as fh:
                fh.write(f"{APP_NAME} v{APP_VERSION}"
                         + (f" [{FTL_VARIANT}]" if FTL_VARIANT else "") + "\n")
                fh.write(f"Diagnostic log started {datetime.now().isoformat()}\n")
                fh.write(f"Log file: {self.path}\n")
                fh.write("=" * 70 + "\n")
        except Exception:
            # Never let logging failures break the install.
            self.enabled = False

    def write(self, text):
        if not self.enabled:
            return
        line = f"[{datetime.now().strftime('%H:%M:%S.%f')[:-3]}] {text}\n"
        try:
            with self._lock:
                with open(self.path, "a", encoding="utf-8") as fh:
                    fh.write(line)
        except Exception:
            pass

    def section(self, title):
        self.write("")
        self.write("-" * 60)
        self.write(title)
        self.write("-" * 60)


# Global logger; created in main() so every command can record itself even
# before the Installer/App objects exist.
DIAG = None


def run(args, timeout=None):
    """Run a command with no console window. Returns (rc, combined output)."""
    if DIAG:
        try:
            shown = args if isinstance(args, str) else " ".join(str(a) for a in args)
        except Exception:
            shown = repr(args)
        DIAG.write(f"RUN: {shown[:500]}")
    try:
        p = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=timeout,
            creationflags=CREATE_NO_WINDOW,
        )
        out = (p.stdout or "") + (p.stderr or "")
        if DIAG:
            DIAG.write(f"  rc={p.returncode}  out_len={len(out)}")
            if out.strip():
                for ln in out.strip().splitlines():
                    DIAG.write(f"    | {ln}")
        return p.returncode, out
    except subprocess.TimeoutExpired:
        if DIAG:
            DIAG.write("  rc=-1  (timed out)")
        return -1, "(timed out)"
    except Exception as exc:  # noqa: BLE001
        if DIAG:
            DIAG.write(f"  rc=-1  (failed to run: {exc})")
        return -1, f"(failed to run: {exc})"


def powershell(script: str, timeout=None):
    return run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        timeout=timeout,
    )


def snapshot_devices(reason=""):
    """Dump all disks and DiskDrive PnP devices to the diagnostic log.

    This is the key diagnostic for the 'stalls at temporary bootloader' issue:
    it shows exactly what Windows presents (FriendlyName, BusType, Size, PnP
    status) so we can see why find_disk() -- which requires FriendlyName in
    DISK_NAMES AND BusType == 'USB' -- may fail to match on a given PC.
    """
    if not DIAG:
        return
    DIAG.section(f"DEVICE SNAPSHOT {('- ' + reason) if reason else ''}")
    # All disks, unfiltered, with the exact properties find_disk() checks.
    powershell(
        "Get-Disk | Select-Object Number, FriendlyName, BusType, "
        "OperationalStatus, Size, PartitionStyle | Format-List | Out-String -Width 200",
        timeout=60,
    )
    # PnP disk drives (catches a device present but not yet a 'disk').
    powershell(
        "Get-PnpDevice -Class DiskDrive -ErrorAction SilentlyContinue | "
        "Select-Object Status, FriendlyName, InstanceId | "
        "Format-List | Out-String -Width 200",
        timeout=60,
    )
    # Active 05AC:1262 devices are especially important here. The Nano 3G
    # bootloader deliberately uses Apple's retail VID/PID, and a host-specific
    # Apple/iTunes driver or filter can therefore bind differently from the
    # generic USBSTOR stack. Capture the exact bound service/driver, PnP state
    # and parent chain rather than inferring it from the friendly name.
    powershell(
        "$targets = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | "
        "Where-Object { $_.InstanceId -like 'USB\\VID_05AC&PID_1262*' -or "
        "$_.InstanceId -like 'USBSTOR\\DISK&VEN_APPLE*' -or "
        "$_.InstanceId -like 'USBSTOR\\DISK&VEN_ROCKBOX*' }); "
        "$chunks = @(); "
        "if ($targets.Count -eq 0) { $chunks += 'No active Nano 3G / Apple mass-storage PnP nodes.' } "
        "else { foreach ($d in $targets) { "
        "$chunks += '===== ACTIVE PNP NODE ====='; "
        "$chunks += ($d | Format-List Status,Class,FriendlyName,InstanceId | Out-String -Width 240); "
        "$chunks += '----- Device properties -----'; "
        "$chunks += (Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue | "
        "Select-Object KeyName,Type,Data | Format-List | Out-String -Width 240); "
        "$chunks += '----- Signed driver -----'; "
        "$chunks += (Get-CimInstance Win32_PnPSignedDriver -ErrorAction SilentlyContinue | "
        "Where-Object { $_.DeviceID -eq $d.InstanceId } | "
        "Select-Object DeviceName,DriverName,DriverVersion,DriverDate,InfName,Manufacturer | "
        "Format-List | Out-String -Width 240) "
        "} }; $chunks -join [Environment]::NewLine",
        timeout=90,
    )
    # Recent system events often contain the decisive USBSTOR/SCSI error (for
    # example a BOT command timeout, a driver bind failure, or a device reset).
    # Keep the query narrow and bounded so it is safe to run repeatedly.
    powershell(
        "$since = (Get-Date).AddMinutes(-3); "
        "Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=$since} "
        "-ErrorAction SilentlyContinue | Where-Object { "
        "$_.ProviderName -match 'Kernel-PnP|USBHUB|USBSTOR|Disk' -or "
        "$_.Message -match 'VID_05AC|PID_1262|USBSTOR|Apple iPod' "
        "} | Select-Object -First 40 TimeCreated,Id,ProviderName,LevelDisplayName,Message | "
        "Format-List | Out-String -Width 240",
        timeout=90,
    )


class InstallError(Exception):
    """A stage failed; the message is already user-facing."""


# --------------------------------------------------------------------------
# the installer itself
# --------------------------------------------------------------------------

class Installer:
    def __init__(self, log, progress, prompt=None):
        self.log = log
        self.progress = progress
        # prompt(title, message) -> bool: shows a modal dialog on the UI thread
        # and blocks this worker thread until the user clicks OK (True) or
        # Cancel (False). Used to pause at every point that needs a physical
        # action, so nothing races past the user.
        self.prompt = prompt or (lambda title, message: True)
        self.mks5lboot = resource("mks5lboot.exe")
        self.bootloader_bin = resource("bootloader.bin")
        self.bootloader_ipod = resource("bootloader-ipodnano3g.ipod")
        self.rockbox_src = resource("rockbox_files", ".rockbox")
        self.zadig = resource("zadig.exe")
        self.eraser_bin = resource("eraser.bin")  # optional; for full uninstall
        self.check_bin = resource("check.bin")     # NAND check image (read-only)
        self._stopped_amds = False

    # ---- preflight -------------------------------------------------------

    def preflight(self):
        for label, path in (
            ("mks5lboot.exe", self.mks5lboot),
            ("bootloader.bin", self.bootloader_bin),
            ("bootloader-ipodnano3g.ipod", self.bootloader_ipod),
        ):
            if not os.path.isfile(path):
                raise InstallError(f"Bundled file missing: {label}")
        if not os.path.isdir(self.rockbox_src):
            raise InstallError("Bundled .rockbox tree is missing")

        if not is_admin():
            raise InstallError(
                "Administrator rights are required to format the device.\n"
                "Right-click the installer and choose 'Run as administrator'."
            )

    # ---- DFU -------------------------------------------------------------

    def clear_dfu_blockers(self):
        """Release anything holding the DFU interface exclusively.

        Both of these were real obstacles during bring-up and both produce the
        same symptom: libusb reports "bad access" (LIBUSB_ERROR_ACCESS).
        """
        run(["taskkill", "/F", "/IM", "wInd3x-win.exe"], timeout=20)

        rc, out = powershell(
            "$s = Get-Service -Name 'Apple Mobile Device Service' "
            "-ErrorAction SilentlyContinue; if ($s -and $s.Status -eq 'Running') "
            "{ Stop-Service -Name 'Apple Mobile Device Service' -Force; 'stopped' } "
            "else { 'not-running' }",
            timeout=60,
        )
        if "stopped" in out:
            self._stopped_amds = True
        time.sleep(2)

    def restore_amds(self):
        if not self._stopped_amds:
            return
        powershell("Start-Service -Name 'Apple Mobile Device Service'", timeout=60)
        self._stopped_amds = False

    def dfu_state(self):
        rc, out = run([self.mks5lboot, "--dfuscan"], timeout=60)
        m = re.search(r"DFU device state:\s*(\d+)", out)
        return int(m.group(1)) if m else None

    def wait_dfu_idle(self, cancel, timeout=240, session=None):
        # Pop-up free flow: the caller logs the DFU instruction, then this polls
        # until the device reaches stock DFU (state 2) and continues on its own.
        if session:
            self.log(session, "step")
        self.log("On the iPod, hold MENU + SELECT until the screen goes black "
                 "and iPod enters DFU mode.", "step")
        self.log("  waiting...")
        deadline = time.time() + timeout
        warned = False
        while time.time() < deadline:
            if cancel.is_set():
                raise InstallError("Cancelled.")
            s = self.dfu_state()
            if s == DFU_IDLE:
                return
            if s is not None and not warned:
                warned = True
            time.sleep(3)
        raise InstallError("The iPod did not reach stock DFU mode (state 2).")

    def send_dfu(self, path, what):
        self.log(f"Sending {what}...")
        rc, out = run([self.mks5lboot, "--dfusend", path], timeout=300)
        if "sent successfully" not in out:
            raise InstallError(f"Failed to send {what}.\n\n{out.strip()[-600:]}")

    # ---- disk ------------------------------------------------------------

    def find_disk(self):
        """Return (number, size, letter) for the iPod disk, or None."""
        names_ps = ",".join("'" + n.replace("'", "''") + "'" for n in DISK_NAMES)
        rc, out = powershell(
            f"$names = @({names_ps}); "
            f"$d = @(Get-Disk | Where-Object {{ $_.FriendlyName -in $names "
            f"-and $_.BusType -eq '{DISK_BUS}' }}); "
            "if ($d.Count -ne 1) { 'NONE'; exit }; "
            "$p = @(Get-Partition -DiskNumber $d[0].Number -ErrorAction SilentlyContinue "
            "| Where-Object { $_.DriveLetter }); "
            "'{0}|{1}|{2}' -f $d[0].Number, $d[0].Size, "
            "$(if ($p.Count -ge 1) { $p[0].DriveLetter } else { '' })",
            timeout=90,
        )
        for line in out.splitlines():
            line = line.strip()
            if "|" not in line:
                continue
            num, size, letter = (line.split("|") + ["", "", ""])[:3]
            try:
                num_i, size_i = int(num), int(size)
            except ValueError:
                continue
            if not (DISK_MIN <= size_i <= DISK_MAX):
                raise InstallError(
                    f"Disk {num_i} is {size_i} bytes, outside the safety range; refusing."
                )
            return num_i, size_i, (letter.strip() or None)
        return None

    def wait_for_disk(self, cancel, timeout=180):
        if DIAG:
            DIAG.write(f"wait_for_disk: polling for up to {timeout}s "
                       f"(match: FriendlyName in {DISK_NAMES} AND BusType == '{DISK_BUS}', "
                       f"size {DISK_MIN}..{DISK_MAX})")
        deadline = time.time() + timeout
        start = time.time()
        next_snapshot = 0.0
        while time.time() < deadline:
            if cancel.is_set():
                raise InstallError("Cancelled.")
            d = self.find_disk()
            if d:
                if DIAG:
                    DIAG.write(f"wait_for_disk: matched disk {d} "
                               f"after {time.time() - start:.1f}s")
                return d
            # Snapshot the enumeration state a few times across the wait so a
            # failing PC records exactly what Windows was presenting while the
            # match kept failing (different BusType, different name, etc.).
            elapsed = time.time() - start
            if elapsed >= next_snapshot:
                snapshot_devices(f"waiting for disk (t+{int(elapsed)}s, no match yet)")
                next_snapshot = elapsed + 15
            time.sleep(1)
        snapshot_devices("wait_for_disk TIMED OUT - final state")
        raise InstallError(
            "The iPod never appeared as a disk. On a blank device the bootloader "
            "enters USB mode by itself; if it is waiting at 'Plug USB cable', "
            "reconnect the cable."
        )

    def format_disk(self, cancel):
        d = self.find_disk()
        if not d:
            raise InstallError("The target disk vanished before formatting.")
        num, _, _ = d
        self.log("Preparing the iPod...", "step")

        # Windows exposes this removable device as a single pseudo-partition at
        # offset 0 spanning the whole disk ("superfloppy"), so there is no free
        # extent and New-Partition fails with "Not enough available capacity".
        # Format that pseudo-partition directly. Rockbox is happy with it:
        # disk_mount() tries fat_mount(drive, 0) at sector 0 first.
        names_ps = ",".join("'" + n.replace("'", "''") + "'" for n in DISK_NAMES)
        script = f"""
$ErrorActionPreference='Stop'
$names = @({names_ps})
$d = @(Get-Disk | Where-Object {{ $_.FriendlyName -in $names -and $_.BusType -eq '{DISK_BUS}' }})
if ($d.Count -ne 1) {{ throw 'target disk not uniquely identified' }}
if ($d[0].Size -lt {DISK_MIN} -or $d[0].Size -gt {DISK_MAX}) {{ throw 'disk size outside safety range' }}
$n = $d[0].Number
Clear-Disk -Number $n -RemoveData -RemoveOEM -Confirm:$false
Start-Sleep -Seconds 3
$p = @(Get-Partition -DiskNumber $n -ErrorAction SilentlyContinue)[0]
if (-not $p) {{ throw 'no partition object appeared' }}
if (-not $p.DriveLetter) {{
    try {{
        Add-PartitionAccessPath -DiskNumber $n -PartitionNumber $p.PartitionNumber -AssignDriveLetter
        Start-Sleep -Seconds 2
        $p = Get-Partition -DiskNumber $n -PartitionNumber $p.PartitionNumber
    }} catch {{ }}
}}
$ok = $false
try {{
    Format-Volume -Partition $p -FileSystem FAT32 -NewFileSystemLabel 'IPOD' -Confirm:$false -Force | Out-Null
    $ok = $true
}} catch {{
    if ($p.DriveLetter) {{
        cmd /c "echo. | format $($p.DriveLetter): /FS:FAT32 /Q /Y /V:IPOD" | Out-Null
        Start-Sleep -Seconds 3
        $v = Get-Volume -DriveLetter $p.DriveLetter -ErrorAction SilentlyContinue
        if ($v -and $v.FileSystem -like 'FAT*') {{ $ok = $true }}
    }}
}}
if (-not $ok) {{ throw 'format failed' }}
$p = Get-Partition -DiskNumber $n -PartitionNumber $p.PartitionNumber
'LETTER=' + $p.DriveLetter
"""
        rc, out = powershell(script, timeout=900)
        m = re.search(r"LETTER=([A-Za-z])", out)
        if not m:
            raise InstallError(f"Could not format the device.\n\n{out.strip()[-800:]}")
        letter = m.group(1).upper()
        return letter

    def copy_files(self, letter, cancel):
        dest = f"{letter}:\\.rockbox"
        self.log("Copying Rockbox (about a minute; progress is shown above)...", "step")
        started = time.time()

        proc = subprocess.Popen(
            ["robocopy", self.rockbox_src, dest, "/E", "/NFL", "/NDL", "/NP", "/R:1", "/W:1"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=CREATE_NO_WINDOW,
        )
        total = sum(
            os.path.getsize(os.path.join(r, f))
            for r, _, fs in os.walk(self.rockbox_src)
            for f in fs
        )
        while proc.poll() is None:
            if cancel.is_set():
                proc.kill()
                raise InstallError("Cancelled during copy; the device is now incomplete.")
            done = 0
            try:
                for r, _, fs in os.walk(dest):
                    for f in fs:
                        try:
                            done += os.path.getsize(os.path.join(r, f))
                        except OSError:
                            pass
            except OSError:
                pass
            if total:
                pct = min(99, int(done * 100 / total))
                self.progress(pct, f"Copying... {done/1024/1024:.1f} / {total/1024/1024:.1f} MB")
            time.sleep(5)

        # robocopy: 0-7 success, >= 8 failure
        if proc.returncode is not None and proc.returncode >= 8:
            raise InstallError(f"Copy failed (robocopy exit code {proc.returncode}).")

        boot = os.path.join(dest, "rockbox.ipod")
        if not os.path.isfile(boot):
            raise InstallError("rockbox.ipod is missing from the device after copying.")

        rc, out = powershell(
            f"$a=(Get-FileHash '{boot}' -Algorithm SHA256).Hash; "
            f"$b=(Get-FileHash '{os.path.join(self.rockbox_src, 'rockbox.ipod')}' "
            "-Algorithm SHA256).Hash; if ($a -eq $b) {'MATCH'} else {'DIFFER'}",
            timeout=600,
        )
        if "MATCH" not in out:
            raise InstallError("rockbox.ipod failed verification on the device.")

    def flush_volume(self, letter):
        """Force the filesystem cache out to NAND before power is removed."""
        GENERIC_RW = 0xC0000000
        SHARE_RW = 0x00000003
        OPEN_EXISTING = 3
        FSCTL_LOCK_VOLUME = 0x00090018
        FSCTL_DISMOUNT_VOLUME = 0x00090020

        k32 = ctypes.windll.kernel32
        h = k32.CreateFileW(
            f"\\\\.\\{letter}:", GENERIC_RW, SHARE_RW, None, OPEN_EXISTING, 0, None
        )
        if h == wintypes.HANDLE(-1).value or h == -1:
            return
        try:
            k32.FlushFileBuffers(h)
            ret = wintypes.DWORD(0)
            k32.DeviceIoControl(h, FSCTL_LOCK_VOLUME, None, 0, None, 0, ctypes.byref(ret), None)
            k32.DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, None, 0, None, 0, ctypes.byref(ret), None)
        finally:
            k32.CloseHandle(h)
        time.sleep(3)

    # ---- stages ----------------------------------------------------------

    def install(self, cancel, skip_format=False, skip_nor=False):
        try:
            self.progress(0, "Checking...")
            self.preflight()

            # 1. Get the device presenting a disk that Rockbox's FTL can use.
            #
            # If a bootloader is already installed the iPod exposes that disk by
            # itself, so the files can be copied straight away with no DFU step
            # at all. Only when no such disk exists does the bootloader need to
            # be run temporarily from DFU to create one. (A stock device's disk
            # does not count: Apple's firmware presents the NAND in Apple's own
            # FTL format, which Rockbox cannot read, so anything copied there
            # would be invisible to it.)
            self.progress(5, "Looking for the iPod...")
            disk = self.find_disk()

            if disk:
                num, size, letter = disk
            else:
                self.clear_dfu_blockers()
                self.wait_dfu_idle(cancel)

                tmp = os.path.join(os.environ.get("TEMP", "."), "nano3g-bl-run.dfu")
                rc, out = run(
                    [self.mks5lboot, "--mkdfu-raw", self.bootloader_bin, tmp], timeout=120
                )
                if not os.path.isfile(tmp):
                    raise InstallError(
                        f"Could not build the temporary bootloader image.\n\n{out[-400:]}"
                    )
                snapshot_devices("before sending temporary bootloader")
                self.send_dfu(tmp, "temporary bootloader")
                num, size, letter = self.wait_for_disk(cancel)

            # 2. filesystem
            self.progress(15, "Preparing the filesystem...")
            if skip_format:
                if not letter:
                    raise InstallError("No drive letter on the device, so files cannot be copied.")
            else:
                letter = self.format_disk(cancel)

            # 3. copy
            self.progress(25, "Copying Rockbox...")
            self.copy_files(letter, cancel)

            # 4. flush
            self.progress(90, "Committing to flash...")
            self.flush_volume(letter)

            # 5. NOR, last. Needs a fresh DFU session (the device just left USB
            # disk mode after the copy). Pop-up free: the log asks for DFU and
            # wait_dfu_idle() detects it and continues on its own.
            if not skip_nor:
                self.progress(93, "Installing the bootloader...")
                self.log("Almost done - one more DFU step to finish.", "step")
                self.clear_dfu_blockers()
                self.wait_dfu_idle(cancel, timeout=300)

                rc, out = run([self.mks5lboot, "--bl-inst", self.bootloader_ipod], timeout=300)
                if "sent successfully" not in out:
                    raise InstallError(
                        "The install failed while writing the bootloader.\n\n"
                        f"{out.strip()[-600:]}"
                    )

            self.progress(100, "Done")
            self.log("")
            self.log("Wait, iPod will reboot into Rockbox shortly.", "ok")
            return True

        except InstallError as exc:
            self.log("")
            self.log(str(exc), "err")
            self.progress(0, "Failed")
            return False
        except Exception as exc:  # noqa: BLE001
            self.log("")
            self.log(f"Unexpected error: {exc}", "err")
            self.progress(0, "Failed")
            return False
        finally:
            self.restore_amds()

    # ---- uninstall -------------------------------------------------------

    def uninstall(self, cancel, erase_nand=True):
        """Remove Rockbox and return the device toward a stock, restorable state.

        Two parts, in order:
          1. Restore Apple's original bootloader to NOR (mks5lboot --bl-uninst),
             so the Rockbox bootloader is no longer what runs at power-on.
          2. Optionally erase the NAND back to blank. This matters because the
             NAND is still in Rockbox's FTL format, which Apple's firmware
             cannot read; it CAN initialise a blank chip (that is the factory
             state), so erasing is what actually lets a subsequent iTunes
             restore succeed. Without this the device would show Apple's
             "connect to iTunes" recovery but the restore is only reliable from
             a truly blank chip.

        After this, the user runs an iTunes restore to reinstall Apple's OS.
        Everything here needs the iPod in stock DFU (state 2).
        """
        try:
            self.progress(0, "Checking...")
            for label, path in (("mks5lboot.exe", self.mks5lboot),):
                if not os.path.isfile(path):
                    raise InstallError(f"Bundled file missing: {label}")
            if not is_admin():
                raise InstallError(
                    "Administrator rights are required.\n"
                    "Right-click the installer and choose 'Run as administrator'."
                )

            have_eraser = os.path.isfile(self.eraser_bin)
            if erase_nand and not have_eraser:
                erase_nand = False

            # ---- Step 1: restore Apple's bootloader to NOR ------------------
            # This is the first of two DFU sessions in a full restore.
            self.progress(10, "Waiting for DFU (step 1 of 2)...")
            self.clear_dfu_blockers()
            self.wait_dfu_idle(
                cancel,
                session="DFU step 1 of 2 — restore Apple's bootloader.",
            )
            self.progress(35, "Restoring Apple's bootloader...")
            rc, out = run([self.mks5lboot, "--bl-uninst", "ipodnano3g"], timeout=300)
            if "sent successfully" not in out:
                raise InstallError(
                    "Could not restore Apple's bootloader.\n\n" + out.strip()[-600:]
                )

            if not erase_nand:
                self.progress(100, "Done")
                self.log("")
                self.log("Done. Rockbox's bootloader is removed. Open iTunes and "
                         "restore the iPod to reinstall Apple's software.", "ok")
                return True

            # ---- Step 2: erase the NAND back to blank -----------------------
            # This needs a second, fresh DFU session after the NOR write.
            self.progress(50, "Waiting for DFU (step 2 of 2)...")
            self.clear_dfu_blockers()
            self.wait_dfu_idle(
                cancel,
                timeout=300,
                session="DFU step 2 of 2 — prepare the NAND erase.",
            )

            self.progress(65, "Sending NAND eraser...")
            tmp = os.path.join(os.environ.get("TEMP", "."), "nano3g-erase.dfu")
            rc, out = run(
                [self.mks5lboot, "--mkdfu-raw", self.eraser_bin, tmp], timeout=120
            )
            if not os.path.isfile(tmp):
                raise InstallError(
                    f"Could not build the eraser image.\n\n{out[-400:]}"
                )
            self.send_dfu(tmp, "NAND eraser")

            # The eraser is now running on the device. It cannot report its
            # progress back over USB, so track its known on-device run time in
            # the app rather than declaring the restore finished after a few
            # seconds. The iPod's own "Hold MENU + SELECT to reboot" screen is
            # still the final authority that the user sees on the device.
            erase_wait_seconds = 40
            self.progress(75, "Erasing on the iPod...")
            self.log("The iPod is erasing its storage. Leave it plugged in and "
                     "do not touch it yet.", "step")
            for elapsed in range(erase_wait_seconds):
                if cancel.is_set():
                    raise InstallError("Cancelled while the iPod continues erasing.")
                pct = 75 + ((elapsed + 1) * 24 // erase_wait_seconds)
                remaining = erase_wait_seconds - elapsed - 1
                self.progress(pct, f"Erasing on the iPod... about {remaining}s remaining")
                time.sleep(1)

            self.progress(100, "Restore complete — reboot the iPod")
            self.log("The erase period is complete.", "ok")
            self.log("On the iPod, confirm it says \"Hold MENU + SELECT to reboot\".",
                     "step")
            self.log("Then press MENU + SELECT. The iPod restarts into recovery "
                     "mode; open iTunes and restore it to reinstall Apple's software.",
                     "step")
            return True

        except InstallError as exc:
            self.log("")
            self.log(str(exc), "err")
            self.progress(0, "Failed")
            return False
        except Exception as exc:  # noqa: BLE001
            self.log("")
            self.log(f"Unexpected error: {exc}", "err")
            self.progress(0, "Failed")
            return False
        finally:
            self.restore_amds()

    # ---- pre-flight compatibility check ---------------------------------

    CHECK_MAGIC = b"nano3g-nandcheck"

    def _read_check_report(self):
        """Find the disk the NAND-check image exposes and return its sector-0
        text report, or None. The check image publishes the report as plain
        ASCII in sector 0 (2048-byte sectors), led by CHECK_MAGIC -- so we
        identify the right disk by that magic rather than by size/name, and
        read nothing else. Read-only.
        """
        # Enumerate USB physical drives and read sector 0 of each, looking for
        # the check image's magic. PowerShell gets us the \\.\PhysicalDriveN
        # numbers for USB disks; the raw read is done with ctypes CreateFile.
        rc, out = powershell(
            "Get-Disk | Where-Object { $_.BusType -eq 'USB' } | "
            "ForEach-Object { $_.Number }",
            timeout=60,
        )
        nums = [int(x) for x in re.findall(r"\d+", out)]
        if DIAG:
            DIAG.write(f"check report: USB disk numbers currently visible: {nums}")
        for n in nums:
            path = rf"\\.\PhysicalDrive{n}"
            if DIAG:
                DIAG.write(f"check report: reading sector 0 from {path}")
            data = self._raw_read_sector0(path)
            if data:
                if DIAG:
                    DIAG.write(f"check report: {path} returned {len(data)} bytes; "
                               f"prefix={data[:32]!r}")
                if data.startswith(self.CHECK_MAGIC):
                    if DIAG:
                        DIAG.write(f"check report: magic matched on {path}")
                    return data.split(b"\0", 1)[0].decode("ascii", "replace")
            elif DIAG:
                DIAG.write(f"check report: {path} returned no data")
        return None

    @staticmethod
    def _raw_read_sector0(path):
        """Read the first 2048 bytes of a physical drive, read-only.

        The diagnostic trace deliberately logs Win32 errors here: on the
        affected PCs the temporary image briefly appears as a disk and then
        drops, so this lets us determine whether the first application read is
        rejected or whether the device had already gone away.
        """
        GENERIC_READ = 0x80000000
        SHARE_RW = 0x00000003
        OPEN_EXISTING = 3
        k32 = ctypes.windll.kernel32
        h = k32.CreateFileW(path, GENERIC_READ, SHARE_RW, None,
                            OPEN_EXISTING, 0, None)
        if h == -1 or h == wintypes.HANDLE(-1).value:
            if DIAG:
                DIAG.write(f"raw read: CreateFileW({path}) failed; "
                           f"GetLastError={k32.GetLastError()}")
            return None
        try:
            buf = ctypes.create_string_buffer(2048)
            got = wintypes.DWORD(0)
            ok = k32.ReadFile(h, buf, 2048, ctypes.byref(got), None)
            if not ok or got.value == 0:
                if DIAG:
                    DIAG.write(f"raw read: ReadFile({path}) ok={bool(ok)} bytes={got.value}; "
                               f"GetLastError={k32.GetLastError()}")
                return None
            if DIAG:
                DIAG.write(f"raw read: ReadFile({path}) succeeded; bytes={got.value}")
            return buf.raw[:got.value]
        finally:
            k32.CloseHandle(h)

    # JEDEC NAND manufacturer codes, mirroring nand_vendor.c so the check
    # report can name the maker without the device having to.
    _MAKERS = {
        0x98: "Toshiba", 0xEC: "Samsung", 0xAD: "Hynix",
        0x89: "Intel", 0x2C: "Micron", 0x45: "SanDisk", 0x20: "ST Micro",
    }

    @classmethod
    def _decode_maker(cls, ids):
        """Return (name, maker_byte) from a report 'ids' string such as
        '0000D5AD ...'. The maker code is the low byte of the first word."""
        if not ids:
            return ("unknown", None)
        first = ids.split()[0]
        try:
            word = int(first, 16)
        except ValueError:
            return ("unknown", None)
        maker = word & 0xFF
        return (cls._MAKERS.get(maker, "unknown"), maker)

    def check_device(self, cancel):
        """Run the read-only NAND-check image and report whether this iPod's
        NAND is supported for a write-enabled Rockbox install. Writes NOTHING
        to the device -- the check image is volatile and runs from DFU.

        The verdict keys off the SAME 'recognized' gate the FTL uses to decide
        read-write vs read-only, so a 'supported' result here is exactly the
        condition under which an install will work rather than leave the device
        read-only. This is the check to run BEFORE installing.
        """
        try:
            self.progress(0, "Checking...")
            if not os.path.isfile(self.mks5lboot):
                raise InstallError("Bundled file missing: mks5lboot.exe")
            if not os.path.isfile(self.check_bin):
                raise InstallError("Bundled file missing: check.bin (NAND check image)")
            if not is_admin():
                raise InstallError(
                    "Administrator rights are required to read the device.\n"
                    "Right-click the installer and choose 'Run as administrator'."
                )

            self.log("Compatibility check - nothing is written to the iPod.", "step")
            self.progress(10, "Waiting for DFU...")
            self.clear_dfu_blockers()
            self.wait_dfu_idle(cancel)

            tmp = os.path.join(os.environ.get("TEMP", "."), "nano3g-check.dfu")
            rc, out = run([self.mks5lboot, "--mkdfu-raw", self.check_bin, tmp],
                          timeout=120)
            if not os.path.isfile(tmp):
                raise InstallError(f"Could not build the check image.\n\n{out[-400:]}")
            self.send_dfu(tmp, "NAND check image")
            snapshot_devices("after sending NAND check image")

            self.progress(50, "Reading the chip report...")
            self.log("Reading the chip report from the device...")
            report = None
            deadline = time.time() + 60
            start = time.time()
            next_snapshot = 0.0
            while time.time() < deadline and report is None:
                if cancel.is_set():
                    raise InstallError("Cancelled.")
                report = self._read_check_report()
                if report is None:
                    elapsed = time.time() - start
                    if elapsed >= next_snapshot:
                        snapshot_devices(
                            f"waiting for NAND check report (t+{int(elapsed)}s, no report yet)"
                        )
                        next_snapshot = elapsed + 15
                    time.sleep(2)
            if report is None:
                snapshot_devices("NAND check report TIMED OUT - final state")
                raise InstallError(
                    "Could not read the check report. If the iPod is not visible "
                    "as a disk, use 'USB driver setup (Zadig)' and try again."
                )

            # Parse the report's key lines.
            rep = {}
            for line in report.splitlines():
                key, _, value = line.partition(" ")
                rep[key.strip()] = value.strip()
            recognized = rep.get("recognized", "0") == "1"
            verdict = rep.get("verdict", "(no verdict)")
            ids = rep.get("ids", rep.get("rawid", ""))
            model = rep.get("model", "")

            # Decode the NAND maker from the first ID word's maker byte. The
            # report prints ids as 8-hex-digit words like "0000D5AD"; the low
            # byte (here AD) is the JEDEC maker code. This is the genuinely
            # useful thing to show/report -- earlier this line mistakenly
            # showed the iPod MODEL number (e.g. MB261) as the "maker".
            maker_name, maker_byte = self._decode_maker(ids)

            self.progress(100, "Done")
            self.log("")
            self.log(f"  chip ids : {ids}", "ok")
            self.log(f"  NAND maker: {maker_name}"
                     + (f" (0x{maker_byte:02X})" if maker_byte is not None else ""),
                     "ok")
            if rep.get("bitspercell"):
                slc = rep.get("bitspercell") == "1"
                self.log(f"  cell type: {'SLC' if slc else 'MLC'} "
                         f"(bits/cell {rep.get('bitspercell')})", "ok")
            if model:
                self.log(f"  iPod model: {model}", "ok")
            self.log(f"  verdict  : {verdict}", "ok")
            self.log("")
            if recognized:
                self.log("SUPPORTED. This iPod's NAND is validated for a "
                         "write-enabled install.", "ok")
                self.log("It is safe to click 'Install Rockbox'.", "ok")
                return ("supported", ids, verdict)
            else:
                self.log("NOT SUPPORTED. This NAND is not validated for writing.",
                         "err")
                self.log("Do NOT install: the device would mount read-only and the "
                         "install would fail (and an unvalidated write could risk "
                         "the flash).", "err")
                self.log("Please report this chip id so it can be validated: "
                         + (ids or "(unknown)"), "step")
                return ("unsupported", ids, verdict)

        except InstallError as exc:
            self.log("")
            self.log(str(exc), "err")
            self.progress(0, "Failed")
            return ("error", "", str(exc))
        except Exception as exc:  # noqa: BLE001
            self.log("")
            self.log(f"Unexpected error: {exc}", "err")
            self.progress(0, "Failed")
            return ("error", "", str(exc))
        finally:
            self.restore_amds()


# --------------------------------------------------------------------------
# GUI
# --------------------------------------------------------------------------

class App(tk.Tk):
    def __init__(self):
        # Set this before creating the first window so Windows associates the
        # title bar/Alt-Tab/taskbar entry with this app and its custom icon.
        if sys.platform == "win32":
            try:
                ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(APP_ID)
            except (AttributeError, OSError):
                pass

        super().__init__()
        self.title(f"{APP_NAME} v{APP_VERSION}"
                   + (f"  [{FTL_VARIANT}]" if FTL_VARIANT else ""))
        try:
            # icon.ico is both bundled into the one-file build and present next
            # to the source build. This replaces Tk/Python's generic feather.
            self.iconbitmap(resource("icon.ico"))
        except tk.TclError:
            pass
        self.geometry("720x500")
        self.minsize(700, 440)

        self.queue: "queue.Queue[tuple]" = queue.Queue()
        self.cancel = threading.Event()
        self.worker = None
        # For worker->UI modal prompts: the worker fills _prompt_result and
        # signals _prompt_done after the main thread has shown the dialog.
        self._prompt_done = threading.Event()
        self._prompt_result = False
        # Use the single diagnostic log file (created in main()) so the on-screen
        # log, raw command output, and device snapshots all land in one place.
        if DIAG and getattr(DIAG, "path", None):
            self.logfile = DIAG.path
        else:
            self.logfile = os.path.join(
                os.environ.get("TEMP", "."),
                f"nano3g-install-{datetime.now():%Y%m%d-%H%M%S}.log",
            )

        self._style()
        self._build()
        self.after(100, self._drain)
        # Tell the user where the diagnostic log lives so it is easy to find
        # and send back after a stall.
        self._log(f"Diagnostic log: {self.logfile}", "step")

    def _style(self):
        """Keep the ttk widgets on the same light background as the tk ones.

        Without this, checkbuttons and frames inherit the theme's own default
        (often grey) and sit visibly apart from the window.
        """
        st = ttk.Style(self)
        try:
            st.theme_use("clam")   # honours background settings on Windows
        except tk.TclError:
            pass
        st.configure("TFrame", background=BG)
        st.configure("TCheckbutton", background=BG, foreground=FG)
        st.map("TCheckbutton", background=[("active", BG)])
        st.configure("TButton", padding=(8, 4))
        st.configure(
            "Horizontal.TProgressbar",
            background="#2f81f7", troughcolor="#e6e8eb",
            bordercolor=BORDER, lightcolor="#2f81f7", darkcolor="#2f81f7",
        )

    def _build(self):
        self.configure(bg=BG)

        head = tk.Frame(self, bg=BG_HEAD)
        head.pack(fill="x")
        tk.Label(
            head, text="Rockbox for iPod Nano 3G", bg=BG_HEAD, fg=FG,
            font=("Segoe UI", 15, "bold"),
        ).pack(anchor="w", padx=10, pady=(2, 0))
        tk.Label(
            head, text="S5L8702 / N46  -  self-contained installer",
            bg=BG_HEAD, fg=FG_MUTED, font=("Segoe UI", 9),
        ).pack(anchor="w", padx=10, pady=(0, 2))
        tk.Frame(self, bg=BORDER, height=1).pack(fill="x")

        warn = tk.Frame(self, bg=BG_WARN, highlightbackground=BORDER_WARN,
                        highlightthickness=1)
        warn.pack(fill="x", padx=10, pady=(5, 5))
        tk.Label(
            warn,
            text=("Only some Nano 3G units have a NAND chip this build supports.\n"
                  "Run 'Check my iPod (safe)' FIRST - it is read-only and tells you\n"
                  "whether installing is safe. Do not install if the check says no."),
            bg=BG_WARN, fg=FG_WARN, justify="left", font=("Segoe UI", 9, "bold"),
        ).pack(anchor="w", padx=10, pady=(5, 2))
        tk.Label(
            warn,
            text=("Installing erases the iPod completely, including Apple's firmware.\n"
                  "Returning to the Apple OS requires an iTunes restore."),
            bg=BG_WARN, fg=FG_WARN2, justify="left", font=("Segoe UI", 9),
        ).pack(anchor="w", padx=10, pady=(0, 5))

        btns = tk.Frame(self, bg=BG)
        btns.pack(fill="x", padx=10, pady=(5, 5))
        # Primary actions: Install and Uninstall. Install always does the full,
        # correct thing (format + copy + NOR); there are deliberately no
        # partial-install checkboxes to get wrong.
        # Recommended first step: a read-only compatibility check.
        self.b_check = ttk.Button(btns, text="Check my iPod (safe)",
                                  command=self._check)
        self.b_check.pack(side="left")
        self.b_install = ttk.Button(btns, text="Install Rockbox", command=self._install)
        self.b_install.pack(side="left", padx=4)
        self.b_uninstall = ttk.Button(btns, text="Uninstall (restore Apple)",
                                       command=self._uninstall)
        self.b_uninstall.pack(side="left", padx=4)
        self.b_cancel = ttk.Button(btns, text="Cancel", command=self._cancel, state="disabled")
        self.b_cancel.pack(side="left", padx=4)
        # Keep Zadig in the same baseline-aligned control group; separating it
        # at the far right created an unnecessary wide, empty button row.
        ttk.Button(btns, text="USB driver setup (Zadig)",
                   command=self._zadig).pack(side="left", padx=(4, 0))

        self.pbar = ttk.Progressbar(self, mode="determinate", maximum=100)
        self.pbar.pack(fill="x", padx=10)
        self.status = tk.Label(self, text="Ready", anchor="w", bg=BG, fg=FG_MUTED)
        self.status.pack(fill="x", padx=10, pady=(2, 4))

        # Plain Text (no scrollbar). Scrolling is done with the mouse wheel /
        # trackpad; the log auto-scrolls to the newest line as it grows.
        self.text = tk.Text(
            self, height=10, bg=BG_PANEL, fg=LOG_FG, insertbackground=LOG_FG,
            font=("Consolas", 9), wrap="word", relief="solid", borderwidth=1,
        )
        self.text.pack(fill="both", expand=True, padx=10, pady=(0, 6))
        self._bind_wheel_scroll(self.text)

        footer = tk.Frame(self, bg=BG)
        footer.pack(fill="x", padx=10, pady=(0, 4))
        tk.Label(footer, text=f"{APP_NAME} v{APP_VERSION}",
                 anchor="w", bg=BG, fg=FG_MUTED, font=("Segoe UI", 8)).pack(side="left")
        tk.Label(footer, text="2026 Created by Ricardo de Koning",
                 anchor="e", bg=BG, fg=FG_MUTED, font=("Segoe UI", 8)).pack(side="right")
        for tag, col in (("ok", LOG_OK), ("warn", LOG_WARN),
                         ("err", LOG_ERR), ("step", LOG_STEP)):
            self.text.tag_config(tag, foreground=col)
        self.text.tag_config("step", foreground=LOG_STEP, font=("Consolas", 9, "bold"))

        self._log("Connect the iPod, then press 'Check my iPod (safe)' first, "
                  "or 'Install Rockbox' to begin.")
        if not is_admin():
            self._log("Please restart with 'Run as administrator'.", "warn")

    # ---- plumbing --------------------------------------------------------

    def _bind_wheel_scroll(self, widget):
        """Let the log scroll with the mouse wheel / trackpad, since it has no
        visible scrollbar. Windows/macOS deliver <MouseWheel> with a signed
        delta; X11 uses Button-4/5. Bindings are attached only while the
        pointer is over the widget so wheel events elsewhere are unaffected."""
        def on_wheel(event):
            if event.num == 4:            # X11 scroll up
                widget.yview_scroll(-3, "units")
            elif event.num == 5:          # X11 scroll down
                widget.yview_scroll(3, "units")
            else:                          # Windows/macOS
                step = -3 if event.delta > 0 else 3
                widget.yview_scroll(step, "units")
            return "break"

        def bind_wheel(_e=None):
            widget.bind_all("<MouseWheel>", on_wheel)
            widget.bind_all("<Button-4>", on_wheel)
            widget.bind_all("<Button-5>", on_wheel)

        def unbind_wheel(_e=None):
            widget.unbind_all("<MouseWheel>")
            widget.unbind_all("<Button-4>")
            widget.unbind_all("<Button-5>")

        widget.bind("<Enter>", bind_wheel)
        widget.bind("<Leave>", unbind_wheel)

    def _log(self, msg, tag=None):
        stamp = datetime.now().strftime("%H:%M:%S")
        line = f"[{stamp}] {msg}\n" if msg else "\n"
        self.text.insert("end", line, tag or "")
        self.text.see("end")
        try:
            with open(self.logfile, "a", encoding="utf-8") as fh:
                fh.write(line)
        except OSError:
            pass
        # Mirror the on-screen narrative into the full diagnostic transcript so
        # the single log file the user sends back has both the UI messages and
        # the raw command output / device snapshots interleaved in order.
        if DIAG and msg:
            DIAG.write(f"UI[{tag or ''}]: {msg}")

    def _drain(self):
        try:
            while True:
                kind, a, b = self.queue.get_nowait()
                if kind == "log":
                    self._log(a, b)
                elif kind == "progress":
                    self.pbar["value"] = a
                    self.status.config(text=b)
                elif kind == "prompt":
                    # a = title, b = message. Show the modal on THIS (main)
                    # thread, then release the waiting worker with the result.
                    self._prompt_result = messagebox.askokcancel(
                        a, b, icon="info", parent=self
                    )
                    self._prompt_done.set()
                elif kind == "done":
                    self.b_check.config(state="normal")
                    self.b_install.config(state="normal")
                    self.b_uninstall.config(state="normal")
                    self.b_cancel.config(state="disabled")
        except queue.Empty:
            pass
        self.after(150, self._drain)

    def _install(self):
        # No confirmation pop-up: what the install does (and that it replaces
        # Apple's firmware, needing an iTunes restore to undo) is already
        # spelled out in the main window. Pressing Install is the confirmation.
        # From here the flow is pop-up free: DFU is requested in the log and
        # auto-detected, and the run ends with a single completion message.
        self._begin_worker(lambda inst: inst.install(self.cancel))

    def _uninstall(self):
        # Likewise no confirmation pop-up here; the main window already
        # describes what Uninstall does. The flow is driven entirely through
        # the log with automatic DFU detection.
        self._begin_worker(lambda inst: inst.uninstall(self.cancel, erase_nand=True))

    def _check(self):
        # Read-only; no confirmation needed. This is the safe, recommended
        # first step.
        self._begin_worker(lambda inst: inst.check_device(self.cancel))

    def _worker_prompt(self, title, message):
        """Called from the worker thread: show a modal dialog on the main
        thread and block until the user answers. Returns True (OK) / False
        (Cancel). If a cancel was already requested, don't prompt -- just
        return False so the flow unwinds."""
        if self.cancel.is_set():
            return False
        self._prompt_done.clear()
        self.queue.put(("prompt", title, message))
        self._prompt_done.wait()
        return self._prompt_result

    def _begin_worker(self, action):
        """Shared setup for Check/Install/Uninstall: disable buttons, clear the
        log, run `action(installer)` on a worker thread."""
        self.b_check.config(state="disabled")
        self.b_install.config(state="disabled")
        self.b_uninstall.config(state="disabled")
        self.b_cancel.config(state="normal")
        self.cancel.clear()
        self.text.delete("1.0", "end")

        inst = Installer(
            log=lambda m, t=None: self.queue.put(("log", m, t)),
            progress=lambda p, s: self.queue.put(("progress", p, s)),
            prompt=self._worker_prompt,
        )

        def work():
            action(inst)
            self.queue.put(("done", None, None))

        self.worker = threading.Thread(target=work, daemon=True)
        self.worker.start()

    def _cancel(self):
        self.cancel.set()
        self._log("Cancelling...", "warn")

    def _zadig(self):
        z = resource("zadig.exe")
        if not os.path.isfile(z):
            messagebox.showerror("Zadig", "Zadig was not bundled with this build.")
            return
        messagebox.showinfo(
            "USB driver setup",
            "Only needed if the installer cannot find the iPod in DFU mode.\n\n"
            "In Zadig:\n"
            "  1. Options -> List All Devices\n"
            "  2. Select the iPod DFU device (VID 05AC, PID 1223)\n"
            "  3. Choose WinUSB and click Replace/Install Driver\n\n"
            "libusb needs Zadig's WinUSB driver specifically; Apple's own driver "
            "uses a GUID libusb cannot open.",
        )
        try:
            subprocess.Popen([z])
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror("Zadig", f"Could not start Zadig: {exc}")

    def _open_log(self):
        try:
            if not os.path.exists(self.logfile):
                open(self.logfile, "a", encoding="utf-8").close()
            os.startfile(self.logfile)  # noqa: S606
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror("Log", str(exc))


def main():
    if os.name != "nt":
        print("This installer is for Windows.")
        return 1
    # Create the diagnostic logger first so every command run during startup
    # and the whole session is captured to one file the user can send back.
    global DIAG
    DIAG = DiagLogger()
    DIAG.write(f"is_admin={is_admin()}  cwd={os.getcwd()}")
    DIAG.write(f"frozen={getattr(sys, 'frozen', False)}  "
               f"meipass={getattr(sys, '_MEIPASS', '')}")
    App().mainloop()
    DIAG.write("App closed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
