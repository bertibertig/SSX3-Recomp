# Starts the built port on Windows from the repo root (paths in src/main.c are relative to it).
# Windows counterpart of scripts/run.sh (same switches and defaults, same start-up menu).
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 *> logs\run.log   # for doctor.sh / seed_from_log
# Looks for build-win\ssx3_recomp.exe (scripts/build_win.sh, MinGW), then build\ssx3_recomp.exe and
# build\Release\ssx3_recomp.exe (MSVC, untested). Works in Windows PowerShell 5.1 and PowerShell 7.
# Runtime switches (details: scripts/run.sh, docs/notes.md, xboxrecomp/docs):
#   RECOMP_TITLE_KEVENTS / RECOMP_VBLANK / RECOMP_AC97_READY / RECOMP_PB_EXEC   on by default, as in run.sh
#   SSX3_RENDERER=cpu       default on Windows: the Vulkan back end has no Win32 surface yet (docs/TODO.md, M4)
#   SSX3_RENDERER=vulkan    GPU renderer, once a build includes it; enables the start-up menu below
#   SSX3_INTERP=0 / SSX3_FPS_LIMIT=<n> / SSX3_VSYNC=1 / SSX3_RES_SCALE=<1..5>   Vulkan settings, as in run.sh
#   SSX3_MENU=0 / 1         start-up menu for resolution, frame rate, v-sync: never / always (default: when in a
#                           console and not set above). The choice is saved in %APPDATA%\ssx3-recomp\run.conf
#   SSX3_PAD_TRACE=1        log every controller button change
#   RECOMP_USB_PADS=2       more emulated pads on the USB bus (split screen), default 1
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

$Exe = $null
foreach ($p in 'build-win\ssx3_recomp.exe', 'build\ssx3_recomp.exe', 'build\Release\ssx3_recomp.exe') {
    if (Test-Path -LiteralPath (Join-Path $Root $p)) { $Exe = Join-Path $Root $p; break }
}
if (-not $Exe) {
    [Console]::Error.WriteLine('No Windows build. First run: scripts/build_win.sh (MinGW cross build)')
    exit 1
}
Set-Location -LiteralPath $Root

function Set-EnvDefault([string]$Name, [string]$Value) {
    if (-not [Environment]::GetEnvironmentVariable($Name)) { [Environment]::SetEnvironmentVariable($Name, $Value) }
}
function Get-EnvValue([string]$Name) { [Environment]::GetEnvironmentVariable($Name) }

Set-EnvDefault RECOMP_TITLE_KEVENTS 1
Set-EnvDefault RECOMP_VBLANK 1
Set-EnvDefault RECOMP_AC97_READY 1
Set-EnvDefault RECOMP_PB_EXEC 1             # decode/execute the pushbuffer - otherwise no picture
Set-EnvDefault SSX3_RENDERER cpu            # see above: no Vulkan on Windows yet
if ((Get-EnvValue SSX3_RENDERER) -eq 'cpu') {
    Set-EnvDefault RECOMP_FB_WINDOW 1       # the CPU rasteriser's window
} else {
    [Environment]::SetEnvironmentVariable('RECOMP_FB_WINDOW', $null)   # the Vulkan back end opens its own window
}
Set-EnvDefault RECOMP_USB 1                 # emulated USB controller (XPP/OHCI)
Set-EnvDefault RECOMP_KEYBOARD 1            # keyboard = controller 1 (Enter=Start, Z=A, X=B, arrows=D-pad ...)
Set-EnvDefault SSX3_SKIP_LOGOS 1            # skip the EA BIG and THX logos (SSX3_SKIP_INTRO=1: the intro too)

# ---- start-up menu (port of scripts/_launch_menu.sh) ----

$ConfDir = if ($env:APPDATA) { Join-Path $env:APPDATA 'ssx3-recomp' } else { Join-Path $HOME '.config\ssx3-recomp' }
$LaunchConf = Join-Path $ConfDir 'run.conf'

$ResItems = @(
    @('1', '1x  640x480    native, fastest'),
    @('2', '2x  1280x960'),
    @('3', '3x  1920x1440'),
    @('4', '4x  2560x1920'),
    @('5', '5x  3200x2400  sharpest, heaviest')
)
$FpsItems = @(
    @('display',  "Display refresh  interpolated up to the monitor's rate"),
    @('60',       "60 fps           the game's own frames, no interpolation"),
    @('120',      '120 fps          interpolated'),
    @('144',      '144 fps          interpolated'),
    @('165',      '165 fps          interpolated'),
    @('240',      '240 fps          interpolated'),
    @('uncapped', 'Uncapped         interpolated, as fast as the GPU can')
)
$VsyncItems = @(
    @('0', 'Off  lowest latency (mailbox; immediate may tear)'),
    @('1', 'On   FIFO: waits for the display refresh, more latency')
)

# Show-Menu <title> <default tag> <items> -> chosen tag, $null on cancel. Arrow keys / j k / 1-9, Enter, Esc / q.
function Show-Menu([string]$Title, [string]$Default, $Items) {
    $n = $Items.Count
    $sel = 0
    $width = 0
    for ($i = 0; $i -lt $n; $i++) {
        if ($Items[$i][0] -eq $Default) { $sel = $i }
        if ($Items[$i][1].Length -gt $width) { $width = $Items[$i][1].Length }
    }
    $prompt = "Up/Down or 1-$n, Enter = select, Esc/q = quit"
    if ($prompt.Length -gt $width) { $width = $prompt.Length }
    $width += 4
    $hr = ' ' + ('-' * $width)
    $lines = $n + 4

    # Reserve the lines first, so the top row stays valid even if the console scrolls.
    for ($i = 0; $i -lt $lines; $i++) { [Console]::WriteLine() }
    $top = [Console]::CursorTop - $lines
    $cursor = $true
    try { $cursor = [Console]::CursorVisible; [Console]::CursorVisible = $false } catch { }
    try {
        while ($true) {
            [Console]::SetCursorPosition(0, $top)
            Write-Host (' ' + $Title).PadRight($width + 1)
            Write-Host $hr
            for ($i = 0; $i -lt $n; $i++) {
                if ($i -eq $sel) {
                    Write-Host -NoNewline ' '
                    Write-Host (' > ' + $Items[$i][1]).PadRight($width) -ForegroundColor Black -BackgroundColor Gray
                } else {
                    Write-Host ('    ' + $Items[$i][1]).PadRight($width + 1)
                }
            }
            Write-Host $hr
            Write-Host (' ' + $prompt).PadRight($width + 1) -ForegroundColor DarkGray

            $k = [Console]::ReadKey($true)
            if ($k.Key -eq 'UpArrow' -or $k.KeyChar -eq 'k') { $sel = ($sel + $n - 1) % $n }
            elseif ($k.Key -eq 'DownArrow' -or $k.KeyChar -eq 'j') { $sel = ($sel + 1) % $n }
            elseif ($k.KeyChar -ge '1' -and $k.KeyChar -le '9') {
                $d = [int][string]$k.KeyChar
                if ($d -le $n) { $sel = $d - 1 }
            }
            elseif ($k.Key -eq 'Enter') { return $Items[$sel][0] }
            elseif ($k.Key -eq 'Escape' -or $k.KeyChar -eq 'q' -or $k.KeyChar -eq 'Q') { return $null }
        }
    } finally {
        try { [Console]::CursorVisible = $cursor } catch { }
        Write-Host ''
    }
}

function Get-ConfValue([string]$Key) {  # value from the saved choice (parsed as data, never executed)
    if (-not (Test-Path -LiteralPath $LaunchConf)) { return $null }
    $v = $null
    foreach ($line in Get-Content -LiteralPath $LaunchConf) {
        if ($line -match "^$Key=([A-Za-z0-9]*)$") { $v = $Matches[1] }
    }
    return $v
}

function Set-FpsEnv([string]$Fps) {
    switch -regex ($Fps) {
        '^display$'  { break }                                    # the back end's default: fastest display's refresh
        '^60$'       { $env:SSX3_INTERP = '0'; break }
        '^uncapped$' { $env:SSX3_FPS_LIMIT = '0'; break }
        '^[0-9]+$'   { $env:SSX3_FPS_LIMIT = $Fps; break }
        default      { [Console]::Error.WriteLine("run.ps1: ignoring saved frame rate '$Fps'") }
    }
}

function Invoke-LaunchMenu {
    if ((Get-EnvValue SSX3_RENDERER) -ne 'vulkan') { return }   # these settings only exist in the Vulkan back end

    $askRes = -not (Get-EnvValue SSX3_RES_SCALE)
    $askFps = -not ((Get-EnvValue SSX3_FPS_LIMIT) -or (Get-EnvValue SSX3_INTERP))
    $askVsync = -not (Get-EnvValue SSX3_VSYNC)
    if ($env:SSX3_MENU -eq '1') { $askRes = $true; $askFps = $true; $askVsync = $true }

    $res = Get-ConfValue RES_SCALE;   if (-not $res)   { $res = if ($env:SSX3_RES_SCALE) { $env:SSX3_RES_SCALE } else { '1' } }
    $fps = Get-ConfValue FPS;         if (-not $fps)   { $fps = 'display' }
    $vsync = Get-ConfValue VSYNC;     if (-not $vsync) { $vsync = if ($env:SSX3_VSYNC) { $env:SSX3_VSYNC } else { '0' } }

    $interactive = ($env:SSX3_MENU -ne '0') -and -not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected
    if ($interactive -and ($askRes -or $askFps -or $askVsync)) {
        if ($askRes) {
            $res = Show-Menu 'Internal resolution (F10 cycles it in game)' $res $ResItems
            if ($null -eq $res) { [Console]::Error.WriteLine('Cancelled.'); exit 0 }
        }
        if ($askFps) {
            $fps = Show-Menu 'Frame rate (the game runs at 60; higher rates are interpolated)' $fps $FpsItems
            if ($null -eq $fps) { [Console]::Error.WriteLine('Cancelled.'); exit 0 }
        }
        if ($askVsync) {
            $vsync = Show-Menu 'V-sync (sync presentation to the display refresh)' $vsync $VsyncItems
            if ($null -eq $vsync) { [Console]::Error.WriteLine('Cancelled.'); exit 0 }
        }
        New-Item -ItemType Directory -Force -Path $ConfDir | Out-Null
        Set-Content -LiteralPath $LaunchConf -Encoding ASCII -Value @(
            '# scripts/run.ps1 - last start-up choice', "RES_SCALE=$res", "FPS=$fps", "VSYNC=$vsync")
    }

    if ($askRes) { $env:SSX3_RES_SCALE = $res }
    if ($askFps) { $env:SSX3_FPS_LIMIT = $null; $env:SSX3_INTERP = $null; Set-FpsEnv $fps }
    if ($askVsync) {
        if ($vsync -eq '0' -or $vsync -eq '1') { $env:SSX3_VSYNC = $vsync }
        else { [Console]::Error.WriteLine("run.ps1: ignoring saved v-sync '$vsync'"); $env:SSX3_VSYNC = $null }
    }
    $fpsText = 'display refresh'
    if ($env:SSX3_INTERP -eq '0') { $fpsText = '60' }
    elseif ($env:SSX3_FPS_LIMIT -eq '0') { $fpsText = 'uncapped' }
    elseif ($env:SSX3_FPS_LIMIT) { $fpsText = $env:SSX3_FPS_LIMIT }
    $vsyncText = if ($env:SSX3_VSYNC -eq '1') { 'on' } else { 'off' }
    $scale = if ($env:SSX3_RES_SCALE) { $env:SSX3_RES_SCALE } else { '1' }
    [Console]::Error.WriteLine("run.ps1: internal resolution ${scale}x, frame rate $fpsText, v-sync $vsyncText")
}

Invoke-LaunchMenu
& $Exe @args
exit $LASTEXITCODE
