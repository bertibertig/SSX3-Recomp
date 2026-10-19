# Starts the built port on Windows from the repo root or from an unpacked release folder (paths in src/main.c
# are relative to it).
# Windows counterpart of scripts/run.sh (same switches and defaults, same start-up menu).
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 *> logs\run.log   # for doctor.sh / seed_from_log
# Looks for build-win\ssx3_recomp.exe (scripts/build_win.sh, MinGW), then build\ssx3_recomp.exe and
# build\Release\ssx3_recomp.exe (MSVC, untested). Works in Windows PowerShell 5.1 and PowerShell 7.
# Runtime switches (details: scripts/run.sh, docs/notes.md, xboxrecomp/docs):
#   RECOMP_TITLE_KEVENTS / RECOMP_VBLANK / RECOMP_AC97_READY / RECOMP_PB_EXEC   on by default, as in run.sh
#   SSX3_RENDERER=cpu       default on Windows: the Vulkan back end has no Win32 surface yet (docs/TODO.md, M4)
#   SSX3_RENDERER=vulkan    GPU renderer, once a build includes it; then also chosen in the start-up menu below
#   SSX3_INTERP=0 / SSX3_FPS_LIMIT=<n> / SSX3_VSYNC=1 / SSX3_RES_SCALE=<1..5> / SSX3_ASPECT=<16:9|16:10|21:9|auto>
#                           / SSX3_WINDOW=<windowed|borderless|fullscreen> / SSX3_MONITOR=<primary|number|name>
#                           Vulkan settings, as in run.sh
#   SSX3_MENU=0 / 1         start-up menu for renderer, monitor, display mode, resolution, aspect ratio, frame rate, v-sync: never / always (default: when in a
#                           console and not set above). The choice is saved in %APPDATA%\ssx3-recomp\run.conf
#   SSX3_PAD_TRACE=1        log every controller button change
#   RECOMP_USB_PADS=2       more emulated pads on the USB bus (split screen), default 1
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
    exit 1
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
Set-EnvDefault RECOMP_KEYBOARD 1            # keyboard = controller 1 (Enter=Start, Z=A, X=B, arrows=D-pad ...)
Set-EnvDefault SSX3_SKIP_LOGOS 1            # skip the EA BIG and THX logos (SSX3_SKIP_INTRO=1: the intro too)

# ---- start-up menu (port of scripts/_launch_menu.sh) ----

$ConfDir = if ($env:APPDATA) { Join-Path $env:APPDATA 'ssx3-recomp' } else { Join-Path $HOME '.config\ssx3-recomp' }
$LaunchConf = Join-Path $ConfDir 'run.conf'

$RendererText = @{
    'vulkan' = 'Vulkan    GPU: higher resolution, widescreen, over 60 fps, full screen'
    'cpu'    = 'Software  CPU reference renderer: 640x480, 4:3, 60 fps, window only, slow'
}
$WindowItems = @(
    @('windowed',   'Windowed    resizable window'),
    @('borderless', 'Borderless  full screen as a borderless window (fast Alt+Tab)'),
    @('fullscreen', 'Fullscreen  exclusive full screen at the desktop resolution')
)
$ResItems = @(
    @('1', '1x  640x480    native, fastest'),
    @('2', '2x  1280x960'),
    @('3', '3x  1920x1440'),
    @('4', '4x  2560x1920'),
    @('5', '5x  3200x2400  sharpest, heaviest')
)
$AspectItems = @(
    @('4:3',   '4:3    original, no widescreen'),
    @('16:9',  '16:9   widescreen (most monitors and TVs)'),
    @('16:10', '16:10  widescreen (many laptops)'),
    @('21:9',  '21:9   ultrawide'),
    @('auto',  'Auto   the desktop''s shape')
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
        if ($line -match "^$Key=([][A-Za-z0-9:.,()_ -]*)$") { $v = $Matches[1] }
    }
    return $v
}

function Get-MonitorItems {  # from `ssx3_recomp --list-displays`; empty with one monitor or none listed
    $items = @(); $main = $null
    try { $lines = & $Exe --list-displays 2>$null } catch { return @() }
    foreach ($l in @($lines)) {
        $f = "$l" -split "`t"
        if ($f.Count -lt 5 -or -not $f[1]) { continue }
        if ($f[4] -eq '1') { $main = $f[1] }
        $items += ,@($f[1], ('{0,-36} {1} @ {2} Hz' -f $f[1], $f[2], $f[3]))
    }
    if ($items.Count -lt 2) { return @() }
    $label = if ($main) { "Main monitor ($main)" } else { 'Main monitor' }
    return ,(@(,@('primary', $label)) + $items)
}

function Get-Renderers {  # from `ssx3_recomp --renderers`, preferred first; "cpu" when the exe lists none
    $list = @()
    try { $lines = & $Exe --renderers 2>$null } catch { $lines = @() }
    foreach ($l in @($lines)) { if ("$l".Trim() -match '^(vulkan|cpu)$') { $list += $Matches[1] } }
    if ($list.Count -eq 0) { $list = @('cpu') }   # no Vulkan on Windows yet
    return ,$list
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
    $renderers = Get-Renderers
    $renItems = @()
    if ($renderers.Count -ge 2) { foreach ($r in $renderers) { $renItems += ,@($r, $RendererText[$r]) } }
    $askRen = -not (Get-EnvValue SSX3_RENDERER)
    $askMon = -not (Get-EnvValue SSX3_MONITOR)
    $askWin = -not (Get-EnvValue SSX3_WINDOW)
    $askRes = -not (Get-EnvValue SSX3_RES_SCALE)
    $askAspect = -not (Get-EnvValue SSX3_ASPECT)
    $askFps = -not ((Get-EnvValue SSX3_FPS_LIMIT) -or (Get-EnvValue SSX3_INTERP))
    $askVsync = -not (Get-EnvValue SSX3_VSYNC)
    if ($env:SSX3_MENU -eq '1') { $askRen = $true; $askMon = $true; $askWin = $true; $askRes = $true; $askAspect = $true; $askFps = $true; $askVsync = $true }

    $ren = Get-EnvValue SSX3_RENDERER
    if ($askRen) {
        $ren = Get-ConfValue RENDERER
        if ($renderers -notcontains $ren) { $ren = $renderers[0] }   # unknown or not in this build
    }
    if ($renItems.Count -eq 0) { $askRen = $false }
    $mon = Get-ConfValue MONITOR;     if (-not $mon)   { $mon = if ($env:SSX3_MONITOR) { $env:SSX3_MONITOR } else { 'primary' } }
    $win = Get-ConfValue WINDOW;      if (-not $win)   { $win = if ($env:SSX3_WINDOW) { $env:SSX3_WINDOW } else { 'windowed' } }
    $res = Get-ConfValue RES_SCALE;   if (-not $res)   { $res = if ($env:SSX3_RES_SCALE) { $env:SSX3_RES_SCALE } else { '1' } }
    $aspect = Get-ConfValue ASPECT;   if (-not $aspect) { $aspect = if ($env:SSX3_ASPECT) { $env:SSX3_ASPECT } else { '16:9' } }
    $fps = Get-ConfValue FPS;         if (-not $fps)   { $fps = 'display' }
    $vsync = Get-ConfValue VSYNC;     if (-not $vsync) { $vsync = if ($env:SSX3_VSYNC) { $env:SSX3_VSYNC } else { '0' } }

    $interactive = ($env:SSX3_MENU -ne '0') -and -not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected
    if ($interactive -and -not $askRen -and $ren -ne 'vulkan') { $interactive = $false }   # software renderer: nothing to ask
    if ($interactive -and ($askRen -or $askMon -or $askWin -or $askRes -or $askAspect -or $askFps -or $askVsync)) {
        if ($askRen) {
            $ren = Show-Menu 'Renderer (the software renderer has no further options)' $ren $renItems
            if ($null -eq $ren) { [Console]::Error.WriteLine('Cancelled.'); exit 0 }
        }
        if ($ren -ne 'vulkan') {   # the options below only exist in the Vulkan back end
            $askMon = $false; $askWin = $false; $askRes = $false; $askAspect = $false; $askFps = $false; $askVsync = $false
        }
        if ($askMon) {
            $monItems = Get-MonitorItems
            if ($monItems.Count -gt 0) {
                $mon = Show-Menu 'Monitor (which one the game opens on)' $mon $monItems
                if ($null -eq $mon) { [Console]::Error.WriteLine('Cancelled.'); exit 0 }
            }
        }
        if ($askWin) {
            $win = Show-Menu 'Display mode (Alt+Enter / F11 toggles in game)' $win $WindowItems
            if ($null -eq $win) { [Console]::Error.WriteLine('Cancelled.'); exit 0 }
        }
        if ($askRes) {
            $res = Show-Menu 'Internal resolution (F10 cycles it in game)' $res $ResItems
            if ($null -eq $res) { [Console]::Error.WriteLine('Cancelled.'); exit 0 }
        }
        if ($askAspect) {
            $aspect = Show-Menu 'Aspect ratio (widescreen in races; menus and movies stay 4:3)' $aspect $AspectItems
            if ($null -eq $aspect) { [Console]::Error.WriteLine('Cancelled.'); exit 0 }
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
            '# scripts/run.ps1 - last start-up choice', "RENDERER=$ren", "MONITOR=$mon", "WINDOW=$win", "RES_SCALE=$res", "ASPECT=$aspect", "FPS=$fps", "VSYNC=$vsync")
    }

    $env:SSX3_RENDERER = $ren
    if ($ren -ne 'vulkan') {
        [Console]::Error.WriteLine('run.ps1: software renderer (640x480, 4:3, 60 fps, windowed)')
        return
    }
    if ($askMon) {
        if (-not $mon -or $mon -eq 'primary') { $env:SSX3_MONITOR = $null } else { $env:SSX3_MONITOR = $mon }
    }
    if ($askWin) {
        if ($win -match '^(windowed|borderless|fullscreen)$') { $env:SSX3_WINDOW = $win }
        else { [Console]::Error.WriteLine("run.ps1: ignoring saved display mode '$win'"); $env:SSX3_WINDOW = $null }
    }
    if ($askRes) { $env:SSX3_RES_SCALE = $res }
    if ($askAspect) {
        if ($aspect -match '^(auto|[0-9]+:[0-9]+)$') { $env:SSX3_ASPECT = $aspect }
        else { [Console]::Error.WriteLine("run.ps1: ignoring saved aspect ratio '$aspect'"); $env:SSX3_ASPECT = $null }
    }
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
    $aspectText = if ($env:SSX3_ASPECT) { $env:SSX3_ASPECT } else { '4:3' }
    $winText = if ($env:SSX3_WINDOW) { $env:SSX3_WINDOW } else { 'windowed' }
    $monText = if ($env:SSX3_MONITOR) { $env:SSX3_MONITOR } else { 'primary' }
    [Console]::Error.WriteLine("run.ps1: Vulkan, monitor $monText, $winText, internal resolution ${scale}x, aspect $aspectText, frame rate $fpsText, v-sync $vsyncText")
}

Invoke-LaunchMenu                       # renderer; Vulkan: monitor, resolution, frame rate ...
Set-EnvDefault SSX3_RENDERER cpu            # see above: no Vulkan on Windows yet
if ((Get-EnvValue SSX3_RENDERER) -eq 'cpu') {
    Set-EnvDefault RECOMP_FB_WINDOW 1       # the CPU rasteriser's window
} else {
    [Environment]::SetEnvironmentVariable('RECOMP_FB_WINDOW', $null)   # the Vulkan back end opens its own window
}
& $Exe @args
exit $LASTEXITCODE
