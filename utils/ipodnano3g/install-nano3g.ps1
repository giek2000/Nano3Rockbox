<#
.SYNOPSIS
    Installs Rockbox on an iPod Nano 3G (N46 / S5L8702).

.DESCRIPTION
    Runs the whole installation in the order that keeps NOR safe for as long
    as possible:

      1. Run the Rockbox bootloader TEMPORARILY from DFU. This is volatile --
         nothing is written to the device -- but it exposes the raw NAND to
         the host as a USB mass-storage disk through Rockbox's own FTL.
      2. Format that disk FAT32 and copy the .rockbox tree onto it.
      3. Only then patch NOR with the persistent dual bootloader.

    Doing the NOR write last means that if any earlier step fails, NOR has
    never been touched and the device is exactly as it was. It also means the
    device auto-boots into a working Rockbox the moment the patch lands,
    instead of coming up to "Can't load rockbox.ipod: File not found".

.NOTES
    DESTRUCTIVE AND NOT REVERSIBLE WITHOUT iTUNES.

    Rockbox's FTL uses its own on-flash format, incompatible with Apple's.
    Installing it reformats the NAND and therefore destroys Apple's OS, which
    lives on the NAND (NOR is only 1 MB and holds just Apple's 128 KB
    bootloader). Returning to the Apple firmware requires an iTunes restore.

    *** AFTER INSTALLING, DO NOT USE THE HOLD SWITCH AT BOOT. ***
    The dual bootloader hands off to Apple's bootloader as designed, but on a
    Rockbox-formatted device Apple's firmware writes to the NAND trying to
    repair storage it cannot read, which wipes the Rockbox install. Confirmed
    on hardware. Treat this as a single-boot device.

    Lessons encoded here, all hit during bring-up:
      - Apple Mobile Device Service claims the DFU device (libusb "bad access").
      - A leftover wInd3x process also holds it exclusively.
      - wInd3x's "Haxed DFU" is not interchangeable with mks5lboot's: after
        running wInd3x, mks5lboot sees DFU state 9 (dfuERROR) instead of the
        state 2 (dfuIDLE) it needs, and --dfureset does not clear it. So this
        script uses mks5lboot for everything and verifies state 2 before
        writing NOR.
      - Writes go through a full 512 KB erase-block rewrite per call, so
        copying ~20 MB takes tens of minutes. That is expected, not a hang.

.PARAMETER RockboxDir
    Directory containing the '.rockbox' folder to install (as produced by
    'make zip' or tools/buildzip.pl --install=<dir>).

.PARAMETER Bootloader
    Raw bootloader binary (build-nano3g-boot/bootloader.bin), run from DFU.

.PARAMETER BootloaderIpod
    Scrambled bootloader (build-nano3g-boot/bootloader-ipodnano3g.ipod),
    written to NOR.

.PARAMETER Mks5lboot
    Path to mks5lboot.exe.

.PARAMETER SkipNorPatch
    Do everything except the NOR write. Useful for refreshing the files on a
    device whose bootloader is already installed.

.PARAMETER SkipFormat
    Copy files without reformatting. Only valid if the device already carries
    a Rockbox-formatted FAT volume.

.EXAMPLE
    .\install-nano3g.ps1 -RockboxDir C:\KIRO\TEMP\rbstage
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $RockboxDir,
    [string] $Bootloader      = "",
    [string] $BootloaderIpod  = "",
    [string] $Mks5lboot       = "",
    [switch] $SkipNorPatch,
    [switch] $SkipFormat,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
$script:LogFile = Join-Path $env:TEMP ("nano3g-install-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))

# Identity of the disk this script is allowed to touch. Every destructive
# operation re-checks all three, so a shifting disk number can never cause it
# to format the wrong device.
$DISK_NAME   = 'Apple Samsung'
$DISK_BUS    = 'USB'
$DISK_MIN    = 1GB
$DISK_MAX    = 20GB

function Write-Log {
    param([string] $Message, [string] $Level = 'INFO')
    $line = "[{0}] [{1,-5}] {2}" -f (Get-Date -Format 'HH:mm:ss'), $Level, $Message
    $colour = switch ($Level) {
        'OK'    { 'Green' }
        'WARN'  { 'Yellow' }
        'ERR'   { 'Red' }
        'STEP'  { 'Cyan' }
        default { 'Gray' }
    }
    Write-Host $line -ForegroundColor $colour
    $line | Out-File $script:LogFile -Append -Encoding utf8
}

function Fail { param([string] $Message) Write-Log $Message 'ERR'; throw $Message }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

# --- target disk resolution -------------------------------------------------

function Get-IpodDisk {
    $d = @(Get-Disk -ErrorAction SilentlyContinue | Where-Object {
        $_.FriendlyName -eq $DISK_NAME -and $_.BusType -eq $DISK_BUS
    })
    if ($d.Count -ne 1) { return $null }
    if ($d[0].Size -lt $DISK_MIN -or $d[0].Size -gt $DISK_MAX) {
        Fail "Disk $($d[0].Number) is $($d[0].Size) bytes, outside the $DISK_MIN..$DISK_MAX safety range; refusing."
    }
    return $d[0]
}

function Wait-IpodDisk {
    param([int] $TimeoutSec = 120)
    Write-Log "Waiting for the iPod to appear as a USB disk (up to ${TimeoutSec}s)..."
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $d = Get-IpodDisk
        if ($d) {
            Write-Log "Disk $($d.Number) '$($d.FriendlyName)' $($d.Size) bytes, sector $($d.LogicalSectorSize)" 'OK'
            return $d
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

# --- DFU handling -----------------------------------------------------------

function Clear-DfuBlockers {
    # Anything holding the DFU interface exclusively makes libusb fail with
    # LIBUSB_ERROR_ACCESS (-3). Two known culprits.
    Get-Process wInd3x-win -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Log "Stopping leftover wInd3x (PID $($_.Id)) which holds the DFU device" 'WARN'
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
    $svc = Get-Service -Name 'Apple Mobile Device Service' -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -eq 'Running') {
        Write-Log "Apple Mobile Device Service is running and will claim the DFU device" 'WARN'
        if (Test-Admin) {
            try {
                Stop-Service -Name 'Apple Mobile Device Service' -Force
                $script:StoppedAmds = $true
                Write-Log "Stopped Apple Mobile Device Service (will restart at the end)" 'OK'
            } catch {
                Write-Log "Could not stop it: $_" 'WARN'
            }
        } else {
            Fail "Apple Mobile Device Service must be stopped first. Run in an elevated shell, or: Stop-Service -Name 'Apple Mobile Device Service' -Force"
        }
    }
    Start-Sleep -Seconds 2
}

function Get-DfuState {
    # Returns the numeric DFU state, or $null if no device / unparseable.
    # 2 = dfuIDLE (what a download needs). 9 = dfuERROR, which is what a
    # previous wInd3x "Haxed DFU" session leaves behind.
    $out = & $script:Mks5l --dfuscan 2>&1 | Out-String
    $out | Out-File $script:LogFile -Append -Encoding utf8
    if ($out -match 'DFU device state:\s*(\d+)') { return [int]$Matches[1] }
    return $null
}

function Wait-DfuIdle {
    param([int] $TimeoutSec = 180)
    Write-Log "Put the iPod into DFU mode: hold SELECT+MENU ~12s until the screen stays black, keep it connected." 'STEP'
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $s = Get-DfuState
        if ($s -eq 2) { Write-Log "Device is in stock DFU (state 2, dfuIDLE)" 'OK'; return $true }
        if ($null -ne $s) {
            Write-Log "DFU present but state $s (need 2). If you ran wInd3x, power-cycle and re-enter DFU." 'WARN'
        }
        Start-Sleep -Seconds 3
    }
    return $false
}

function Send-Dfu {
    param([string] $File, [string] $What)
    Write-Log "Sending $What ..."
    $out = & $script:Mks5l --dfusend $File 2>&1 | Out-String
    $out | Out-File $script:LogFile -Append -Encoding utf8
    if ($out -notmatch 'sent successfully') { Fail "Failed to send ${What}: $out" }
    Write-Log "$What sent" 'OK'
}

# --- disk operations --------------------------------------------------------

function Format-IpodDisk {
    $d = Get-IpodDisk
    if (-not $d) { Fail "Target disk disappeared before formatting." }
    Write-Log "Formatting disk $($d.Number) as FAT32 (ERASES EVERYTHING)" 'STEP'

    Clear-Disk -Number $d.Number -RemoveData -RemoveOEM -Confirm:$false
    Start-Sleep -Seconds 3

    # Windows presents this removable device as a single pseudo-partition at
    # offset 0 spanning the whole disk ("superfloppy"), so there is no free
    # extent for New-Partition -- it fails with "Not enough available
    # capacity". Format that pseudo-partition directly instead. Rockbox is
    # happy with it: disk_mount() tries fat_mount(drive, 0) at sector 0 before
    # looking at any partition table.
    $p = @(Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue)[0]
    if (-not $p) { Fail "No partition object appeared on disk $($d.Number)." }

    if (-not $p.DriveLetter) {
        try {
            Add-PartitionAccessPath -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber -AssignDriveLetter
            Start-Sleep -Seconds 2
            $p = Get-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber
        } catch { Write-Log "Could not assign a drive letter: $_" 'WARN' }
    }

    $ok = $false
    try {
        Format-Volume -Partition $p -FileSystem FAT32 -NewFileSystemLabel 'IPOD' -Confirm:$false -Force | Out-Null
        $ok = $true
    } catch {
        Write-Log "Format-Volume failed ($_); trying format.com" 'WARN'
        if ($p.DriveLetter) {
            & cmd /c "echo. | format $($p.DriveLetter): /FS:FAT32 /Q /Y /V:IPOD" 2>&1 |
                Out-File $script:LogFile -Append -Encoding utf8
            Start-Sleep -Seconds 3
            $v = Get-Volume -DriveLetter $p.DriveLetter -ErrorAction SilentlyContinue
            $ok = ($v -and $v.FileSystem -like 'FAT*')
        }
    }
    if (-not $ok) { Fail "Could not format the device." }

    $p = Get-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber
    Write-Log "Formatted FAT32 as $($p.DriveLetter):" 'OK'
    return $p.DriveLetter
}

function Get-IpodDriveLetter {
    $d = Get-IpodDisk
    if (-not $d) { return $null }
    $p = @(Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue |
           Where-Object { $_.DriveLetter })
    if ($p.Count -ge 1) { return $p[0].DriveLetter }
    return $null
}

function Copy-RockboxTree {
    param([char] $Drive, [string] $Source)

    $src = Join-Path $Source '.rockbox'
    if (-not (Test-Path $src)) { Fail "No '.rockbox' directory in $Source" }
    $files = @(Get-ChildItem $src -Recurse -File)
    $bytes = ($files | Measure-Object -Property Length -Sum).Sum
    Write-Log "Copying $($files.Count) files ($([math]::Round($bytes/1MB,1)) MB) to ${Drive}:\.rockbox" 'STEP'
    Write-Log "This is slow by design: every write rewrites a full 512 KB erase block. Expect tens of minutes." 'WARN'

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & robocopy $src "${Drive}:\.rockbox" /E /NFL /NDL /NP /R:1 /W:1 |
        Out-File $script:LogFile -Append -Encoding utf8
    $code = $LASTEXITCODE
    $sw.Stop()
    # robocopy: 0-7 success, >=8 failure
    if ($code -ge 8) { Fail "robocopy failed with exit code $code" }
    Write-Log ("Copy finished in {0:n1} min (robocopy code {1})" -f $sw.Elapsed.TotalMinutes, $code) 'OK'

    $boot = "${Drive}:\.rockbox\rockbox.ipod"
    if (-not (Test-Path $boot)) { Fail "rockbox.ipod is missing from the device after copying." }

    $a = (Get-FileHash $boot -Algorithm SHA256).Hash
    $b = (Get-FileHash (Join-Path $src 'rockbox.ipod') -Algorithm SHA256).Hash
    if ($a -ne $b) { Fail "rockbox.ipod failed verification (hash mismatch)." }
    Write-Log "rockbox.ipod verified on device (SHA256 match)" 'OK'
}

function Flush-IpodVolume {
    param([char] $Drive)
    # Force the filesystem cache out to NAND before power is removed.
    Write-Log "Flushing and dismounting ${Drive}: ..."
    Add-Type -ErrorAction SilentlyContinue -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public class N3GFlush {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sec,
        uint d, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr ib,
        uint isz, IntPtr ob, uint osz, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FlushFileBuffers(SafeFileHandle h);
    public static string Flush(char letter) {
        using (var h = CreateFile(@"\\.\" + letter + ":", 0xC0000000u, 3u,
                   IntPtr.Zero, 3u, 0u, IntPtr.Zero)) {
            if (h.IsInvalid) return "open failed " + Marshal.GetLastWin32Error();
            uint r;
            bool f = FlushFileBuffers(h);
            bool l = DeviceIoControl(h, 0x00090018u, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);
            bool u = DeviceIoControl(h, 0x00090020u, IntPtr.Zero, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);
            return "flush=" + f + " lock=" + l + " dismount=" + u;
        }
    }
}
'@
    try { Write-Log ([N3GFlush]::Flush($Drive)) 'OK' }
    catch { Write-Log "Flush failed: $_" 'WARN' }
    Start-Sleep -Seconds 3
}

# ===========================================================================
#  main
# ===========================================================================

Write-Host ""
Write-Host "  Rockbox installer for iPod Nano 3G (S5L8702)" -ForegroundColor White
Write-Host "  log: $script:LogFile" -ForegroundColor DarkGray
Write-Host ""

# --- resolve inputs ---------------------------------------------------------

$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent   # utils/ipodnano3g -> repo root
if (-not $Bootloader)     { $Bootloader     = Join-Path $repo 'build-nano3g-boot\bootloader.bin' }
if (-not $BootloaderIpod) { $BootloaderIpod = Join-Path $repo 'build-nano3g-boot\bootloader-ipodnano3g.ipod' }
if (-not $Mks5lboot)      { $Mks5lboot      = Join-Path $repo 'utils\mks5lboot\mks5lboot.exe' }
$script:Mks5l = $Mks5lboot

foreach ($p in @(
        @{ n = 'bootloader.bin';             v = $Bootloader },
        @{ n = 'bootloader-ipodnano3g.ipod'; v = $BootloaderIpod },
        @{ n = 'mks5lboot.exe';              v = $Mks5lboot })) {
    if (-not (Test-Path $p.v)) { Fail "Missing $($p.n): $($p.v)" }
}
if (-not (Test-Path (Join-Path $RockboxDir '.rockbox'))) {
    Fail "No '.rockbox' directory inside $RockboxDir"
}
Write-Log "mks5lboot     : $Mks5lboot"
Write-Log "bootloader    : $Bootloader"
Write-Log "NOR image     : $BootloaderIpod"
Write-Log "rockbox files : $RockboxDir\.rockbox"

if (-not (Test-Admin)) {
    Fail "Administrator rights are required (formatting and raw disk access). Re-run from an elevated shell."
}

# --- consent ----------------------------------------------------------------

if (-not $Force) {
    Write-Host ""
    Write-Host "  THIS ERASES THE IPOD COMPLETELY, INCLUDING APPLE'S FIRMWARE." -ForegroundColor Red
    Write-Host "  Rockbox's flash format is incompatible with Apple's, so the Apple OS" -ForegroundColor Yellow
    Write-Host "  cannot survive. Going back requires an iTunes restore." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  After installing, DO NOT boot with the HOLD switch on: Apple's" -ForegroundColor Yellow
    Write-Host "  bootloader will write to the NAND and wipe the Rockbox install." -ForegroundColor Yellow
    Write-Host ""
    $a = Read-Host "  Type INSTALL to continue"
    if ($a -ne 'INSTALL') { Write-Log "Aborted by user." 'WARN'; return }
}

$script:StoppedAmds = $false

try {
    # --- 1. temporary bootloader from DFU (nothing written yet) -------------
    Write-Log "Step 1/5 - run the bootloader from DFU (volatile; NOR untouched)" 'STEP'
    Clear-DfuBlockers
    if (-not (Wait-DfuIdle)) { Fail "Device did not reach stock DFU (state 2)." }

    $runDfu = Join-Path $env:TEMP 'nano3g-bl-run.dfu'
    & $Mks5lboot --mkdfu-raw $Bootloader $runDfu 2>&1 |
        Out-File $script:LogFile -Append -Encoding utf8
    if (-not (Test-Path $runDfu)) { Fail "Could not build the temporary bootloader DFU image." }
    Send-Dfu -File $runDfu -What 'temporary bootloader'

    # --- 2. format ---------------------------------------------------------
    Write-Log "Step 2/5 - prepare the filesystem" 'STEP'
    $disk = Wait-IpodDisk -TimeoutSec 120
    if (-not $disk) {
        Fail "The iPod did not appear as a disk. On a blank device the bootloader enters USB mode by itself; if it is waiting at 'Plug USB cable', reconnect it."
    }

    if ($SkipFormat) {
        Write-Log "Skipping format at your request" 'WARN'
        $drive = Get-IpodDriveLetter
        if (-not $drive) { Fail "No drive letter on the device and -SkipFormat was given." }
    } else {
        $drive = Format-IpodDisk
    }

    # --- 3. copy -----------------------------------------------------------
    Write-Log "Step 3/5 - copy Rockbox" 'STEP'
    Copy-RockboxTree -Drive $drive -Source $RockboxDir

    # --- 4. flush ----------------------------------------------------------
    Write-Log "Step 4/5 - commit everything to flash" 'STEP'
    Flush-IpodVolume -Drive $drive

    # --- 5. NOR patch, last ------------------------------------------------
    if ($SkipNorPatch) {
        Write-Log "Step 5/5 - skipping the NOR patch at your request" 'WARN'
        Write-Log "Files are installed. The device still needs its bootloader run from DFU to boot." 'WARN'
    } else {
        Write-Log "Step 5/5 - install the persistent bootloader into NOR" 'STEP'
        Write-Log "Power-cycle the iPod and put it back into DFU mode." 'STEP'
        Clear-DfuBlockers
        if (-not (Wait-DfuIdle -TimeoutSec 240)) {
            Fail "Device did not reach stock DFU. Nothing was written to NOR; the files are already installed, so re-run with -SkipFormat once it is in DFU."
        }

        $out = & $Mks5lboot --bl-inst $BootloaderIpod 2>&1 | Out-String
        $out | Out-File $script:LogFile -Append -Encoding utf8
        if ($out -notmatch 'sent successfully') { Fail "NOR install failed: $out" }
        Write-Log "Installer sent. Listen to the iPod:" 'OK'
        Write-Log "  dual beep (1000Hz+2000Hz) then reboot = installed" 'OK'
        Write-Log "  one 330Hz tone  = failed, NOR still intact" 'WARN'
        Write-Log "  three 330Hz tones = NOR corrupted, restore with iTunes" 'ERR'
    }

    Write-Host ""
    Write-Log "Done. The iPod should now boot Rockbox on its own, with no DFU." 'OK'
    Write-Log "Remember: never boot with the HOLD switch on." 'WARN'
}
finally {
    if ($script:StoppedAmds) {
        try {
            Start-Service -Name 'Apple Mobile Device Service'
            Write-Log "Restarted Apple Mobile Device Service" 'OK'
        } catch {
            Write-Log "Could not restart Apple Mobile Device Service: $_" 'WARN'
        }
    }
    Write-Log "Log saved to $script:LogFile"
}
