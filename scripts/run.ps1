# Starts the built port on Windows from the repo root or from an unpacked release folder (paths in src/main.c
# are relative to it).
# Windows counterpart of scripts/run.sh (same switches and defaults).
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1              # start with the saved settings
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 --settings   # settings screen first; saved in
#                                                                         # %APPDATA%\ssx3-recomp\settings.ini
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 *> logs\run.log   # for doctor.sh / seed_from_log
# Looks for build-win\ssx3_recomp.exe (scripts/build_win.sh, MinGW), then build\ssx3_recomp.exe and
# build\Release\ssx3_recomp.exe (MSVC, untested). Works in Windows PowerShell 5.1 and PowerShell 7.
# Runtime switches (details: scripts/run.sh, docs/notes.md, xboxrecomp/docs):
#   RECOMP_TITLE_KEVENTS / RECOMP_VBLANK / RECOMP_AC97_READY / RECOMP_PB_EXEC   on by default, as in run.sh
#   The SSX3_* variables override the saved settings for one run:
#   SSX3_RENDERER=vulkan    GPU renderer (native Win32 window, needs a Vulkan driver) - default; without a driver
#                           the exe falls back to cpu by itself
#   SSX3_RENDERER=cpu       the software rasteriser, kept as the reference
#   SSX3_LANGUAGE=<en|fr|de|es>  the game's language (default: the Windows display language, if the disc has it)
#   SSX3_INTERP=0 / SSX3_FPS_LIMIT=<n> / SSX3_VSYNC=1 / SSX3_RES_SCALE=<1..5> / SSX3_ASPECT=<16:9|16:10|21:9|auto>
#                           / SSX3_WINDOW=<windowed|borderless|fullscreen> / SSX3_MONITOR=<primary|number|name>
#                           Vulkan settings, as in run.sh
#   SSX3_PAD_TRACE=1        log every controller button change
#   RECOMP_USB_PADS=2       more emulated pads on the USB bus (split screen), default 1
#   SSX3_INSTALL=0          without game_files\default.xbe: just stop instead of starting install_game.ps1
$ErrorActionPreference = 'Stop'
$Exe = $null
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'ssx3_recomp.exe')) {
    # Release package: the exe, this script and game_files\ side by side
    $Root = $PSScriptRoot
    $Exe = Join-Path $Root 'ssx3_recomp.exe'
} else {
    $Root = Split-Path -Parent $PSScriptRoot
    foreach ($p in 'build-win\ssx3_recomp.exe', 'build\ssx3_recomp.exe', 'build\Release\ssx3_recomp.exe') {
        if (Test-Path -LiteralPath (Join-Path $Root $p)) { $Exe = Join-Path $Root $p; break }
    }
}
if (-not $Exe) {
    [Console]::Error.WriteLine('No Windows build. First run: scripts/build_win.sh (MinGW cross build)')
    exit 1
}
Set-Location -LiteralPath $Root
if (-not (Test-Path -LiteralPath (Join-Path $Root 'game_files\default.xbe'))) {
    [Console]::Error.WriteLine("game_files\default.xbe missing: copy the extracted contents of your SSX 3 Xbox disc (PAL) into $(Join-Path $Root 'game_files')")
    $installer = Join-Path $PSScriptRoot 'install_game.ps1'
    if ($env:SSX3_INSTALL -eq '0' -or -not (Test-Path -LiteralPath $installer)) { exit 1 }
    [Console]::Error.WriteLine('run.ps1: starting the game data installer (install_game.ps1; SSX3_INSTALL=0 skips it)')
    $env:SSX3_INSTALL_FROM_RUN = '1'
    & $installer
    $rc = $LASTEXITCODE
    Remove-Item Env:SSX3_INSTALL_FROM_RUN -ErrorAction SilentlyContinue
    if ($rc -eq 10) { exit 0 }                  # installed, but the player chose not to start now
    if ($rc -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $Root 'game_files\default.xbe'))) { exit $(if ($rc) { $rc } else { 1 }) }
    Set-Location -LiteralPath $Root
}

function Set-EnvDefault([string]$Name, [string]$Value) {
    if (-not [Environment]::GetEnvironmentVariable($Name)) { [Environment]::SetEnvironmentVariable($Name, $Value) }
}
function Get-EnvValue([string]$Name) { [Environment]::GetEnvironmentVariable($Name) }

Set-EnvDefault RECOMP_TITLE_KEVENTS 1
Set-EnvDefault RECOMP_VBLANK 1
Set-EnvDefault RECOMP_AC97_READY 1
Set-EnvDefault RECOMP_PB_EXEC 1             # decode/execute the pushbuffer - otherwise no picture
Set-EnvDefault RECOMP_USB 1                 # emulated USB controller (XPP/OHCI)

# Renderer, window, resolution, language, keyboard, logos/intro: the saved settings (ssx3_recomp --settings).
if ($args -contains '--settings') {
    # The settings screen runs in this console: wait for the exe, so PowerShell does not read the same keys
    # (the exe is a GUI program, which PowerShell would otherwise not wait for).
    $p = Start-Process -FilePath $Exe -ArgumentList $args -NoNewWindow -Wait -PassThru
    exit $p.ExitCode
}
& $Exe @args
exit $LASTEXITCODE
