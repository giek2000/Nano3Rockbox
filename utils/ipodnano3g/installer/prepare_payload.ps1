# Collects the installer payload for the FTL v2 build (page-level log-
# structured FTL). Identical layout to prepare_payload.ps1, but sourced from
# the v2 build directories so every embedded artifact is internally
# consistent: the bootloader written to NOR and the rockbox.ipod on the NAND
# are BOTH v2, which is mandatory -- a v1 NOR bootloader cannot mount a v2
# NAND and vice versa.
#
# Bundled:
#   mks5lboot.exe               - DFU + NOR tool (static libusb, no DLL)
#   bootloader.bin              - v2 bootloader, run temporarily from DFU
#   bootloader-ipodnano3g.ipod  - v2 bootloader, written to NOR
#   rockbox_files/.rockbox      - v2 main firmware + support files
#   zadig.exe                   - USB driver setup fallback
$ErrorActionPreference = 'Stop'

$here    = $PSScriptRoot
$repo    = 'C:\KIRO\Nano3Rockbox\rockbox-master'
$bootdir = "$repo\build-nano3g-boot-v2"      # v2 bootloader build
$stage   = 'C:\KIRO\TEMP\rbstage-v2'         # v2 staged .rockbox tree
$payload = Join-Path $here 'payload'

Write-Host "Preparing installer payload (FTL v2)" -ForegroundColor Cyan

if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
New-Item -ItemType Directory -Path $payload -Force | Out-Null

function Need([string]$path, [string]$what) {
    if (-not (Test-Path $path)) { throw "Missing $what : $path" }
    return $path
}

# --- tool + v2 bootloader images -------------------------------------------
Copy-Item (Need "$repo\utils\mks5lboot\mks5lboot.exe" 'mks5lboot.exe') $payload
Copy-Item (Need "$bootdir\bootloader.bin" 'v2 bootloader.bin') $payload
Copy-Item (Need "$bootdir\bootloader-ipodnano3g.ipod" 'v2 scrambled bootloader') $payload

# --- NAND eraser (for full uninstall: blanks the chip so iTunes can restore)
# This is a bootloader built with -DNANO3G_ERASE_ALL. Bundled as eraser.bin.
$eraser = "$repo\build-nano3g-erase\bootloader.bin"
if (Test-Path $eraser) {
    Copy-Item $eraser (Join-Path $payload 'eraser.bin')
} else {
    Write-Host "  WARNING: eraser build not found; Uninstall will remove the" -ForegroundColor Yellow
    Write-Host "           bootloader only (no NAND erase)." -ForegroundColor Yellow
}

# --- NAND check image (read-only compatibility probe, run before installing).
# Bootloader built with -DNAND_CHECK; identifies the chip and reports whether
# it is recognized/validated for a write-enabled install. Bundled as check.bin.
$check = "$repo\build-nano3g-check\bootloader.bin"
if (Test-Path $check) {
    Copy-Item $check (Join-Path $payload 'check.bin')
} else {
    Write-Host "  WARNING: NAND check build not found; 'Check my iPod' will be" -ForegroundColor Yellow
    Write-Host "           unavailable." -ForegroundColor Yellow
}

# --- the v2 .rockbox tree --------------------------------------------------
$src = Need "$stage\.rockbox" 'v2 .rockbox tree'
# Guard the consistency invariant: the NAND firmware must be v2.
Need "$src\rockbox.ipod" 'v2 rockbox.ipod in .rockbox' | Out-Null
$dstRoot = Join-Path $payload 'rockbox_files'
New-Item -ItemType Directory -Path $dstRoot -Force | Out-Null
& robocopy $src (Join-Path $dstRoot '.rockbox') /E /NFL /NDL /NP /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed staging .rockbox (code $LASTEXITCODE)" }

# --- Zadig -----------------------------------------------------------------
$zadig = 'C:\KIRO\iPodUniversalDecrypt\zadig.exe'
if (Test-Path $zadig) { Copy-Item $zadig $payload }
else { Write-Host "  WARNING: zadig.exe not found; driver-setup button will be inert" -ForegroundColor Yellow }

# --- icon ------------------------------------------------------------------
$icon = 'C:\KIRO\iPodUniversalDecrypt\icon.ico'
if (Test-Path $icon) { Copy-Item $icon (Join-Path $here 'icon.ico') }

# --- report ----------------------------------------------------------------
$files = Get-ChildItem $payload -Recurse -File
$mb = [math]::Round(($files | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
Write-Host ""
Write-Host "Payload ready (v2): $($files.Count) files, $mb MB" -ForegroundColor Green
Get-ChildItem $payload | ForEach-Object {
    if ($_.PSIsContainer) {
        $n = (Get-ChildItem $_.FullName -Recurse -File).Count
        Write-Host ("  {0,-30} {1} files" -f $_.Name, $n)
    } else {
        Write-Host ("  {0,-30} {1:n0} bytes" -f $_.Name, $_.Length)
    }
}
