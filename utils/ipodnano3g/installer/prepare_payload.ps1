# Collects everything the standalone installer needs to embed, so the built
# EXE has no external dependencies at all.
#
# Bundled:
#   mks5lboot.exe               - DFU + NOR tool (libusb linked statically, so
#                                 it needs no DLL; verified with objdump: it
#                                 imports only KERNEL32/msvcrt/SETUPAPI/USER32)
#   bootloader.bin              - run temporarily from DFU
#   bootloader-ipodnano3g.ipod  - written to NOR
#   rockbox_files/.rockbox      - the firmware and support files
#   zadig.exe                   - USB driver setup, if the DFU device is not
#                                 visible to libusb
$ErrorActionPreference = 'Stop'

$here   = $PSScriptRoot
$repo   = 'C:\KIRO\Nano3Rockbox\rockbox-master'
$stage  = 'C:\KIRO\TEMP\rbstage'
$payload = Join-Path $here 'payload'

Write-Host "Preparing installer payload" -ForegroundColor Cyan

if (Test-Path $payload) { Remove-Item $payload -Recurse -Force }
New-Item -ItemType Directory -Path $payload -Force | Out-Null

function Need([string]$path, [string]$what) {
    if (-not (Test-Path $path)) { throw "Missing $what : $path" }
    return $path
}

# --- tool + bootloader images ----------------------------------------------
Copy-Item (Need "$repo\utils\mks5lboot\mks5lboot.exe" 'mks5lboot.exe') $payload
Copy-Item (Need "$repo\build-nano3g-boot\bootloader.bin" 'bootloader.bin') $payload
Copy-Item (Need "$repo\build-nano3g-boot\bootloader-ipodnano3g.ipod" 'scrambled bootloader') $payload

# --- the .rockbox tree -----------------------------------------------------
$src = Need "$stage\.rockbox" '.rockbox tree'
$dstRoot = Join-Path $payload 'rockbox_files'
New-Item -ItemType Directory -Path $dstRoot -Force | Out-Null
# robocopy, not Copy-Item: the tree has ~400 files and Copy-Item -Recurse is
# noticeably slower and less predictable with nested empty dirs.
& robocopy $src (Join-Path $dstRoot '.rockbox') /E /NFL /NDL /NP /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed staging .rockbox (code $LASTEXITCODE)" }

# --- Zadig (from the decrypter project, already bundled there) -------------
$zadig = 'C:\KIRO\iPodUniversalDecrypt\zadig.exe'
if (Test-Path $zadig) {
    Copy-Item $zadig $payload
} else {
    Write-Host "  WARNING: zadig.exe not found; driver-setup button will be inert" -ForegroundColor Yellow
}

# --- icon ------------------------------------------------------------------
$icon = 'C:\KIRO\iPodUniversalDecrypt\icon.ico'
if (Test-Path $icon) { Copy-Item $icon (Join-Path $here 'icon.ico') }

# --- report ----------------------------------------------------------------
$files = Get-ChildItem $payload -Recurse -File
$mb = [math]::Round(($files | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
Write-Host ""
Write-Host "Payload ready: $($files.Count) files, $mb MB" -ForegroundColor Green
Get-ChildItem $payload | ForEach-Object {
    if ($_.PSIsContainer) {
        $n = (Get-ChildItem $_.FullName -Recurse -File).Count
        Write-Host ("  {0,-30} {1} files" -f $_.Name, $n)
    } else {
        Write-Host ("  {0,-30} {1:n0} bytes" -f $_.Name, $_.Length)
    }
}
