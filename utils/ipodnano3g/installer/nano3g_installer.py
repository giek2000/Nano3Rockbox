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

*** AFTER INSTALLING, NEVER BOOT WITH THE HOLD SWITCH ON. ***
The dual bootloader hands off to Apple's bootloader as designed, but on a
Rockbox-formatted device Apple's firmware writes to the NAND trying to repair
storage it cannot read, which wipes the Rockbox install. Confirmed on
hardware. Treat this as a single-boot device.

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
from tkinter import messagebox, scrolledtext, ttk

APP_NAME = "Rockbox Installer for iPod Nano 3G"
APP_VERSION = "1.1.0"

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
DISK_NAME = "Apple Samsung"
DISK_BUS = "USB"
DISK_MIN = 1 * 1024**3
DISK_MAX = 20 * 1024**3

# DFU states. 2 = dfuIDLE, which mks5lboot needs before a download.
# 9 = dfuERROR, which is what wInd3x's "Haxed DFU" leaves behind; --dfureset
# does not clear it, only a power cycle back into stock DFU does.
DFU_IDLE = 2

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


def run(args, timeout=None):
    """Run a command with no console window. Returns (rc, combined output)."""
    try:
        p = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=timeout,
            creationflags=CREATE_NO_WINDOW,
        )
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return -1, "(timed out)"
    except Exception as exc:  # noqa: BLE001
        return -1, f"(failed to run: {exc})"


def powershell(script: str, timeout=None):
    return run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        timeout=timeout,
    )


class InstallError(Exception):
    """A stage failed; the message is already user-facing."""


# --------------------------------------------------------------------------
# the installer itself
# --------------------------------------------------------------------------

class Installer:
    def __init__(self, log, progress):
        self.log = log
        self.progress = progress
        self.mks5lboot = resource("mks5lboot.exe")
        self.bootloader_bin = resource("bootloader.bin")
        self.bootloader_ipod = resource("bootloader-ipodnano3g.ipod")
        self.rockbox_src = resource("rockbox_files", ".rockbox")
        self.zadig = resource("zadig.exe")
        self._stopped_amds = False

    # ---- preflight -------------------------------------------------------

    def preflight(self):
        self.log("Checking bundled components...")
        for label, path in (
            ("mks5lboot.exe", self.mks5lboot),
            ("bootloader.bin", self.bootloader_bin),
            ("bootloader-ipodnano3g.ipod", self.bootloader_ipod),
        ):
            if not os.path.isfile(path):
                raise InstallError(f"Bundled file missing: {label}")
        if not os.path.isdir(self.rockbox_src):
            raise InstallError("Bundled .rockbox tree is missing")

        n = sum(len(f) for _, _, f in os.walk(self.rockbox_src))
        size = sum(
            os.path.getsize(os.path.join(r, f))
            for r, _, fs in os.walk(self.rockbox_src)
            for f in fs
        )
        self.log(f"  .rockbox: {n} files, {size / 1024 / 1024:.1f} MB", "ok")

        if not is_admin():
            raise InstallError(
                "Administrator rights are required to format the device.\n"
                "Right-click the installer and choose 'Run as administrator'."
            )
        self.log("  running elevated", "ok")

    # ---- DFU -------------------------------------------------------------

    def clear_dfu_blockers(self):
        """Release anything holding the DFU interface exclusively.

        Both of these were real obstacles during bring-up and both produce the
        same symptom: libusb reports "bad access" (LIBUSB_ERROR_ACCESS).
        """
        rc, out = run(["taskkill", "/F", "/IM", "wInd3x-win.exe"], timeout=20)
        if rc == 0:
            self.log("  stopped a leftover wInd3x process holding the DFU device", "warn")

        rc, out = powershell(
            "$s = Get-Service -Name 'Apple Mobile Device Service' "
            "-ErrorAction SilentlyContinue; if ($s -and $s.Status -eq 'Running') "
            "{ Stop-Service -Name 'Apple Mobile Device Service' -Force; 'stopped' } "
            "else { 'not-running' }",
            timeout=60,
        )
        if "stopped" in out:
            self._stopped_amds = True
            self.log("  stopped Apple Mobile Device Service (it claims the DFU device)", "warn")
        time.sleep(2)

    def restore_amds(self):
        if not self._stopped_amds:
            return
        powershell("Start-Service -Name 'Apple Mobile Device Service'", timeout=60)
        self.log("Restarted Apple Mobile Device Service", "ok")
        self._stopped_amds = False

    def dfu_state(self):
        rc, out = run([self.mks5lboot, "--dfuscan"], timeout=60)
        m = re.search(r"DFU device state:\s*(\d+)", out)
        return int(m.group(1)) if m else None

    def wait_dfu_idle(self, cancel, timeout=240):
        self.log("Waiting for DFU mode...", "step")
        self.log("  On the iPod: hold SELECT+MENU for ~12 s until the screen stays")
        self.log("  black, and keep the cable connected.")
        deadline = time.time() + timeout
        warned = False
        while time.time() < deadline:
            if cancel.is_set():
                raise InstallError("Cancelled.")
            s = self.dfu_state()
            if s == DFU_IDLE:
                self.log("  device is in stock DFU (state 2)", "ok")
                return
            if s is not None and not warned:
                self.log(
                    f"  found DFU but state is {s}, need 2. If wInd3x was used, "
                    "power-cycle the iPod and re-enter DFU.",
                    "warn",
                )
                warned = True
            time.sleep(3)
        raise InstallError("The iPod did not reach stock DFU mode (state 2).")

    def send_dfu(self, path, what):
        self.log(f"Sending {what}...")
        rc, out = run([self.mks5lboot, "--dfusend", path], timeout=300)
        if "sent successfully" not in out:
            raise InstallError(f"Failed to send {what}.\n\n{out.strip()[-600:]}")
        self.log(f"  {what} sent", "ok")

    # ---- disk ------------------------------------------------------------

    def find_disk(self):
        """Return (number, size, letter) for the iPod disk, or None."""
        rc, out = powershell(
            f"$d = @(Get-Disk | Where-Object {{ $_.FriendlyName -eq '{DISK_NAME}' "
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
        self.log("Waiting for the iPod to appear as a disk...", "step")
        deadline = time.time() + timeout
        while time.time() < deadline:
            if cancel.is_set():
                raise InstallError("Cancelled.")
            d = self.find_disk()
            if d:
                num, size, letter = d
                self.log(f"  disk {num}, {size / 1024**3:.2f} GB, drive {letter or '(none)'}", "ok")
                return d
            time.sleep(1)
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
        self.log(f"Formatting disk {num} as FAT32 (this erases everything)", "step")

        # Windows exposes this removable device as a single pseudo-partition at
        # offset 0 spanning the whole disk ("superfloppy"), so there is no free
        # extent and New-Partition fails with "Not enough available capacity".
        # Format that pseudo-partition directly. Rockbox is happy with it:
        # disk_mount() tries fat_mount(drive, 0) at sector 0 first.
        script = f"""
$ErrorActionPreference='Stop'
$d = @(Get-Disk | Where-Object {{ $_.FriendlyName -eq '{DISK_NAME}' -and $_.BusType -eq '{DISK_BUS}' }})
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
        self.log(f"  formatted FAT32 as {letter}:", "ok")
        return letter

    def copy_files(self, letter, cancel):
        dest = f"{letter}:\\.rockbox"
        self.log("Copying Rockbox to the device", "step")
        self.log(
            "  Slow by design: each write rewrites a full 512 KB erase block, "
            "so expect tens of minutes. This is not a hang.",
            "warn",
        )
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

        mins = (time.time() - started) / 60
        self.log(f"  copied in {mins:.1f} min", "ok")

        boot = os.path.join(dest, "rockbox.ipod")
        if not os.path.isfile(boot):
            raise InstallError("rockbox.ipod is missing from the device after copying.")

        self.log("Verifying rockbox.ipod...")
        rc, out = powershell(
            f"$a=(Get-FileHash '{boot}' -Algorithm SHA256).Hash; "
            f"$b=(Get-FileHash '{os.path.join(self.rockbox_src, 'rockbox.ipod')}' "
            "-Algorithm SHA256).Hash; if ($a -eq $b) {'MATCH'} else {'DIFFER'}",
            timeout=600,
        )
        if "MATCH" not in out:
            raise InstallError("rockbox.ipod failed verification on the device.")
        self.log("  verified (SHA256 match)", "ok")

    def flush_volume(self, letter):
        """Force the filesystem cache out to NAND before power is removed."""
        self.log("Committing everything to flash...")
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
            self.log("  could not open the volume to flush it", "warn")
            return
        try:
            k32.FlushFileBuffers(h)
            ret = wintypes.DWORD(0)
            k32.DeviceIoControl(h, FSCTL_LOCK_VOLUME, None, 0, None, 0, ctypes.byref(ret), None)
            k32.DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, None, 0, None, 0, ctypes.byref(ret), None)
            self.log("  flushed and dismounted", "ok")
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
            self.log("Step 1 of 5 - find the iPod's storage", "step")
            disk = self.find_disk()

            if disk:
                num, size, letter = disk
                self.log(
                    f"  already visible as disk {num} ({size / 1024**3:.2f} GB, "
                    f"drive {letter or 'none'}) - no DFU needed to copy files",
                    "ok",
                )
            else:
                self.log("  not visible yet; running the bootloader from DFU to expose it")
                self.log("  (volatile - nothing is written to the device by this step)")
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
                self.send_dfu(tmp, "temporary bootloader")
                num, size, letter = self.wait_for_disk(cancel)

            # 2. filesystem
            self.progress(15, "Preparing the filesystem...")
            self.log("Step 2 of 5 - prepare the filesystem", "step")
            if skip_format:
                self.log("  skipping format as requested", "warn")
                if not letter:
                    raise InstallError("No drive letter on the device, so files cannot be copied.")
            else:
                letter = self.format_disk(cancel)

            # 3. copy
            self.progress(25, "Copying Rockbox...")
            self.log("Step 3 of 5 - copy Rockbox", "step")
            self.copy_files(letter, cancel)

            # 4. flush
            self.progress(90, "Committing to flash...")
            self.log("Step 4 of 5 - commit to flash", "step")
            self.flush_volume(letter)

            # 5. NOR, last
            if skip_nor:
                self.log("Step 5 of 5 - skipping the NOR patch as requested", "warn")
            else:
                self.progress(93, "Installing the bootloader...")
                self.log("Step 5 of 5 - install the bootloader into NOR", "step")
                self.log("Power-cycle the iPod and put it back into DFU mode.", "step")
                self.clear_dfu_blockers()
                self.wait_dfu_idle(cancel, timeout=300)

                rc, out = run([self.mks5lboot, "--bl-inst", self.bootloader_ipod], timeout=300)
                if "sent successfully" not in out:
                    raise InstallError(
                        "The NOR install failed. Nothing was written, and the files "
                        f"are already on the device.\n\n{out.strip()[-600:]}"
                    )
                self.log("  installer sent. Listen to the iPod:", "ok")
                self.log("    dual beep then reboot  = installed", "ok")
                self.log("    one 330 Hz tone        = failed, NOR still intact", "warn")
                self.log("    three 330 Hz tones     = NOR corrupted, restore with iTunes", "err")

            self.progress(100, "Done")
            self.log("")
            self.log("Finished. The iPod should now boot Rockbox on its own.", "ok")
            self.log("NEVER boot with the HOLD switch on - it wipes the install.", "warn")
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


# --------------------------------------------------------------------------
# GUI
# --------------------------------------------------------------------------

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(f"{APP_NAME} v{APP_VERSION}")
        self.geometry("820x640")
        self.minsize(720, 560)

        self.queue: "queue.Queue[tuple]" = queue.Queue()
        self.cancel = threading.Event()
        self.worker = None
        self.logfile = os.path.join(
            os.environ.get("TEMP", "."),
            f"nano3g-install-{datetime.now():%Y%m%d-%H%M%S}.log",
        )

        self._style()
        self._build()
        self.after(100, self._drain)

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
        st.configure("TButton", padding=(10, 5))
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
        ).pack(anchor="w", padx=14, pady=(12, 0))
        tk.Label(
            head, text="S5L8702 / N46  -  self-contained installer",
            bg=BG_HEAD, fg=FG_MUTED, font=("Segoe UI", 9),
        ).pack(anchor="w", padx=14, pady=(0, 12))
        tk.Frame(self, bg=BORDER, height=1).pack(fill="x")

        warn = tk.Frame(self, bg=BG_WARN, highlightbackground=BORDER_WARN,
                        highlightthickness=1)
        warn.pack(fill="x", padx=12, pady=10)
        tk.Label(
            warn,
            text=("This erases the iPod completely, including Apple's firmware.\n"
                  "Rockbox's flash format is incompatible with Apple's, so the Apple OS\n"
                  "cannot survive. Returning to it requires an iTunes restore."),
            bg=BG_WARN, fg=FG_WARN, justify="left", font=("Segoe UI", 9, "bold"),
        ).pack(anchor="w", padx=10, pady=(8, 4))
        tk.Label(
            warn,
            text=("After installing, NEVER boot with the HOLD switch on. Apple's bootloader\n"
                  "writes to the NAND and wipes the Rockbox install. This is a single-boot device."),
            bg=BG_WARN, fg=FG_WARN2, justify="left", font=("Segoe UI", 9),
        ).pack(anchor="w", padx=10, pady=(0, 8))

        opts = tk.Frame(self, bg=BG)
        opts.pack(fill="x", padx=12)
        self.skip_format = tk.BooleanVar(value=False)
        self.skip_nor = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            opts, text="Keep the existing filesystem (only refresh files)",
            variable=self.skip_format,
        ).pack(anchor="w")
        ttk.Checkbutton(
            opts, text="Do not write NOR (bootloader already installed)",
            variable=self.skip_nor,
        ).pack(anchor="w")

        btns = tk.Frame(self, bg=BG)
        btns.pack(fill="x", padx=12, pady=10)
        self.b_install = ttk.Button(btns, text="Install Rockbox", command=self._install)
        self.b_install.pack(side="left")
        self.b_cancel = ttk.Button(btns, text="Cancel", command=self._cancel, state="disabled")
        self.b_cancel.pack(side="left", padx=6)
        ttk.Button(btns, text="USB driver setup (Zadig)", command=self._zadig).pack(side="left", padx=6)
        ttk.Button(btns, text="Open log", command=self._open_log).pack(side="right")

        self.pbar = ttk.Progressbar(self, mode="determinate", maximum=100)
        self.pbar.pack(fill="x", padx=12)
        self.status = tk.Label(self, text="Ready", anchor="w", bg=BG, fg=FG_MUTED)
        self.status.pack(fill="x", padx=14, pady=(2, 6))

        self.text = scrolledtext.ScrolledText(
            self, height=18, bg=BG_PANEL, fg=LOG_FG, insertbackground=LOG_FG,
            font=("Consolas", 9), wrap="word", relief="solid", borderwidth=1,
        )
        self.text.pack(fill="both", expand=True, padx=12, pady=(0, 12))
        for tag, col in (("ok", LOG_OK), ("warn", LOG_WARN),
                         ("err", LOG_ERR), ("step", LOG_STEP)):
            self.text.tag_config(tag, foreground=col)
        self.text.tag_config("step", foreground=LOG_STEP, font=("Consolas", 9, "bold"))

        self._log("Ready. Connect the iPod and press 'Install Rockbox'.")
        if not is_admin():
            self._log("Not running as administrator - formatting will fail. "
                      "Restart with 'Run as administrator'.", "warn")

    # ---- plumbing --------------------------------------------------------

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

    def _drain(self):
        try:
            while True:
                kind, a, b = self.queue.get_nowait()
                if kind == "log":
                    self._log(a, b)
                elif kind == "progress":
                    self.pbar["value"] = a
                    self.status.config(text=b)
                elif kind == "done":
                    self.b_install.config(state="normal")
                    self.b_cancel.config(state="disabled")
        except queue.Empty:
            pass
        self.after(150, self._drain)

    def _install(self):
        if not messagebox.askyesno(
            "Confirm",
            "This erases the iPod completely, including Apple's firmware.\n\n"
            "Returning to the Apple OS requires an iTunes restore.\n\n"
            "Continue?",
            icon="warning",
        ):
            return
        self.b_install.config(state="disabled")
        self.b_cancel.config(state="normal")
        self.cancel.clear()
        self.text.delete("1.0", "end")

        inst = Installer(
            log=lambda m, t=None: self.queue.put(("log", m, t)),
            progress=lambda p, s: self.queue.put(("progress", p, s)),
        )
        sf, sn = self.skip_format.get(), self.skip_nor.get()

        def work():
            inst.install(self.cancel, skip_format=sf, skip_nor=sn)
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
    App().mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
