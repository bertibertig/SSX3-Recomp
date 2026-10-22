# Game data installer for SSX3-Recomp on Windows: puts the contents of YOUR OWN SSX 3 Xbox disc into game_files\
# next to the port's exe, so a release folder is ready to play. Windows counterpart of scripts/install_game.sh
# (same steps, same look). Sources:
#   - a disc image: a full Redump-style .iso (video + game partition) or a trimmed/rebuilt .xiso
#     (extract-xiso -c, xdvdfs pack). The Xbox file system (XDVDFS) is read here, no extra tools needed.
#   - an already extracted folder (the one with default.xbe in it): copied.
# Usage (install_game.bat does the first line on a double-click):
#   powershell -ExecutionPolicy Bypass -File install_game.ps1                     # full-screen menu
#   powershell -ExecutionPolicy Bypass -File install_game.ps1 "SSX 3.iso"         # this source, still asks
#   powershell -ExecutionPolicy Bypass -File install_game.ps1 -Yes "SSX 3.iso"    # no questions
#   ... install_game.ps1 -Check [SOURCE]      only identify the source (or the installed data), write nothing
#   ... install_game.ps1 -Dest DIR SOURCE     another target folder instead of game_files\
# run.ps1 starts this on its own when game_files\default.xbe is missing (SSX3_INSTALL_FROM_RUN=1): "Ride now"
# then exits 0 and run.ps1 goes on with the launch; "Quit" after a finished install exits 10.
#   NO_COLOR=1 no colours; SSX3_INSTALL_PLAIN=1 line-by-line output instead of the full-screen menu.
# Works in Windows PowerShell 5.1 and PowerShell 7. This file is ASCII only on purpose (5.1 reads BOM-less
# scripts in the ANSI code page): every non-ASCII character is built with [char].
param(
    [Parameter(Position = 0)][string]$Source = '',
    [switch]$Yes,
    [switch]$Check,
    [string]$Dest = ''
)
$ErrorActionPreference = 'Stop'

if ((Test-Path -LiteralPath (Join-Path $PSScriptRoot 'ssx3_recomp.exe')) -or
    (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'ssx3_recomp'))) {
    $Root = $PSScriptRoot                                   # release package: exe, scripts and game_files\ side by side
} elseif ((Test-Path -LiteralPath (Join-Path $PSScriptRoot '_common.sh')) -and
          (Test-Path -LiteralPath (Join-Path $PSScriptRoot '..\src'))) {
    $Root = Split-Path -Parent $PSScriptRoot                # repo checkout: scripts\install_game.ps1
} else {
    $Root = $PSScriptRoot
}
if (-not $Dest) { $Dest = Join-Path $Root 'game_files' }
$Dest = [IO.Path]::GetFullPath($Dest)

# The build the port was made from (docs/notes.md "Game version"): SSX 3 PAL/EU, title ID 0x4541003C
$TitleId = '4541003C'
$Sha1Orig = 'ba876cc11ad38650eac9831b1c7f8c4dbf7c5ba9'       # default.xbe as on the PAL disc
$Sha1Patched = '409141774025fce02ab4743d1e669d35bb1d190e'    # same build, region/media-free softmod patch
$Chunk = 32MB
$OnWindows = $env:OS -eq 'Windows_NT'

# ---- XDVDFS reader, copying, hashing (C# 5, so Windows PowerShell 5.1 compiles it) ----
if (-not ('Ssx3Install' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

public class Ssx3Entry {
    public string Path; public string Src; public long Offset; public long Size;
}

public static class Ssx3Install {
    // rebuilt .xiso, then Redump images of XGD1 (SSX 3), XGD2, XGD3 discs
    static readonly long[] Partitions = { 0L, 0x18300000L, 0x0FD90000L, 0x02080000L };
    const string Magic = "MICROSOFT*XBOX*MEDIA";

    public static byte[] Read(string path, long off, int n) {
        using (FileStream f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            if (off + n > f.Length) return null;
            f.Seek(off, SeekOrigin.Begin);
            byte[] b = new byte[n]; int got = 0;
            while (got < n) { int r = f.Read(b, got, n - got); if (r <= 0) return null; got += r; }
            return b;
        }
    }

    // byte offset of the game partition, -1 if this is no Xbox image; root directory sector/size
    public static long Probe(string path, out uint rootSect, out uint rootSize) {
        rootSect = 0; rootSize = 0;
        foreach (long p in Partitions) {
            byte[] b = Read(path, p + 0x10000, 2048);
            if (b == null || Encoding.ASCII.GetString(b, 0, 20) != Magic) continue;
            rootSect = BitConverter.ToUInt32(b, 20); rootSize = BitConverter.ToUInt32(b, 24);
            return p;
        }
        return -1;
    }

    // walks the whole tree; throws with a readable message on a broken image
    public static void Scan(string img, long part, uint sect, uint size, string rel, int depth,
                            List<Ssx3Entry> files, List<string> dirs) {
        if (size == 0) return;
        if (size > (16 << 20)) throw new Exception("directory table too large in /" + rel);
        if (depth > 32) throw new Exception("directories nested too deep");
        long imgSize = new FileInfo(img).Length;
        byte[] d = Read(img, part + (long)sect * 2048, (int)size);
        if (d == null) throw new Exception("directory /" + rel + " lies outside the image");
        Stack<int> stack = new Stack<int>(); stack.Push(0);
        HashSet<int> seen = new HashSet<int>();
        List<object[]> subs = new List<object[]>();
        while (stack.Count > 0) {
            int o = stack.Pop();
            if (!seen.Add(o) || o + 14 > d.Length) continue;
            int l = BitConverter.ToUInt16(d, o), r = BitConverter.ToUInt16(d, o + 2);
            if (l == 0xFFFF && r == 0xFFFF) continue;                    // sector padding
            uint es = BitConverter.ToUInt32(d, o + 4), fs = BitConverter.ToUInt32(d, o + 8);
            int at = d[o + 12], nl = d[o + 13];
            if (nl == 0 || o + 14 + nl > d.Length) throw new Exception("broken entry in /" + rel);
            StringBuilder sb = new StringBuilder();
            for (int i = o + 14; i < o + 14 + nl; i++) {
                byte c = d[i];
                if (c < 32 || c > 126 || c == '/' || c == '\\' || c == ':' ) throw new Exception("bad file name in /" + rel);
                sb.Append((char)c);
            }
            string name = sb.ToString();
            if (name == "." || name == "..") throw new Exception("bad file name in /" + rel);
            if ((at & 0x10) != 0) {
                subs.Add(new object[] { es, fs, name }); dirs.Add(rel + name);
            } else {
                long off = part + (long)es * 2048;
                if (off + fs > imgSize) throw new Exception("/" + rel + name + " lies outside the image (incomplete download?)");
                Ssx3Entry e = new Ssx3Entry(); e.Path = rel + name; e.Src = img; e.Offset = off; e.Size = fs;
                files.Add(e);
            }
            if (l != 0) stack.Push(l * 4);
            if (r != 0) stack.Push(r * 4);
        }
        foreach (object[] s in subs)
            Scan(img, part, (uint)s[0], (uint)s[1], rel + (string)s[2] + "/", depth + 1, files, dirs);
    }

    public static void CopyChunk(string src, long srcOff, string dst, long dstOff, int n) {
        byte[] buf = new byte[Math.Min(n, 4 << 20)];
        using (FileStream i = new FileStream(src, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (FileStream o = new FileStream(dst, FileMode.OpenOrCreate, FileAccess.Write, FileShare.Read)) {
            i.Seek(srcOff, SeekOrigin.Begin); o.Seek(dstOff, SeekOrigin.Begin);
            int left = n;
            while (left > 0) {
                int r = i.Read(buf, 0, Math.Min(buf.Length, left));
                if (r <= 0) throw new IOException("unexpected end of " + src);
                o.Write(buf, 0, r); left -= r;
            }
        }
    }

    public static string Sha1(string path, long off, long len) {
        using (FileStream f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (SHA1 h = SHA1.Create()) {
            f.Seek(off, SeekOrigin.Begin);
            byte[] buf = new byte[1 << 20]; long left = len;
            while (left > 0) {
                int r = f.Read(buf, 0, (int)Math.Min(buf.Length, left));
                if (r <= 0) break;
                h.TransformBlock(buf, 0, r, null, 0); left -= r;
            }
            h.TransformFinalBlock(buf, 0, 0);
            StringBuilder sb = new StringBuilder();
            foreach (byte b in h.Hash) sb.Append(b.ToString("x2"));
            return sb.ToString();
        }
    }

    [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int n);
    [DllImport("kernel32.dll")] static extern bool GetConsoleMode(IntPtr h, out uint m);
    [DllImport("kernel32.dll")] static extern bool SetConsoleMode(IntPtr h, uint m);
    // ANSI escape sequences in the classic console (Windows 10 1511+); false if not possible
    public static bool EnableVT() {
        try {
            IntPtr h = GetStdHandle(-11); uint m;
            if (!GetConsoleMode(h, out m)) return false;
            return SetConsoleMode(h, m | 0x0004);
        } catch { return false; }
    }
}
'@
}

# ---- terminal ----
$Tui = $true
try { if ([Console]::IsInputRedirected -or [Console]::IsOutputRedirected) { $Tui = $false } } catch { $Tui = $false }
if ($env:SSX3_INSTALL_PLAIN -or $Check -or ($Yes -and $Source)) { $Tui = $false }
$Color = -not $env:NO_COLOR
if ($OnWindows) {
    try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
    if (-not [Ssx3Install]::EnableVT()) { $Tui = $false; $Color = $false }
}
try { if ([Console]::IsOutputRedirected) { $Color = $false } } catch { }

$E = [char]27
function Fg([int]$r, [int]$g, [int]$b) { if ($Color) { "$E[38;2;$r;$g;${b}m" } else { '' } }
function Bg([int]$r, [int]$g, [int]$b) { if ($Color) { "$E[48;2;$r;$g;${b}m" } else { '' } }
# SSX 3's menus: orange and sun-yellow on dark, ice-blue and snow-white for the mountain
$C_OR = Fg 255 122 0; $C_YE = Fg 255 204 0; $C_IC = Fg 120 210 255; $C_DP = Fg 70 120 170
$C_SN = Fg 245 250 255; $C_DIM = Fg 130 140 155; $C_RED = Fg 255 80 60; $C_GRN = Fg 120 230 120
$B_OR = Bg 255 122 0; $C_INK = Fg 20 20 30
if ($Color) { $Bo = "$E[1m"; $It = "$E[3m"; $Rst = "$E[0m" } else { $Bo = ''; $It = ''; $Rst = '' }
$LogoCol = @((Fg 255 84 0), (Fg 255 112 0), (Fg 255 140 0), (Fg 255 170 0), (Fg 255 200 20))

$Ch = @{
    Full = [string][char]0x2588; Up = [string][char]0x2580; Lo = [string][char]0x2584; Ell = [string][char]0x2026
    Tri = [string][char]0x25B6; Dia = [string][char]0x25C6; DiaO = [string][char]0x25C7; Hz = [string][char]0x2500
    HzB = [string][char]0x2501; Star = [string][char]0x2726; Check = [string][char]0x2713; Dot = [string][char]0x00B7
    Mid = [string][char]0x00B7; Arr = ([string][char]0x2191) + [char]0x2193; Ret = [string][char]0x23CE
}
# logo: # = full block, ^ = upper half, _ = lower half (see install_game.sh)
$Logo = @(
    '       __#####_   __#####_  ^###_   _###^      _#####__',
    '      ###^^  ^^  ###^^  ^^    ^###_###^        ^^  _##^',
    '      ^^#####_   ^^#####_       _###_            ^^##_',
    '    __    _##^ __    _##^     _###^###_      __    _##^',
    '    ^######^^  ^######^^    _###^   ^###_    ^######^^'
) | ForEach-Object { $_.Replace('#', $Ch.Full).Replace('^', $Ch.Up).Replace('_', $Ch.Lo) }
$PeakName = @('', 'PICK YOUR LINE', 'CHECK THE GEAR', 'DROP IN')
$W = 64; $M = 2; $Rows = 24; $Cols = 80

function Measure-Screen {
    try { $script:Cols = [Console]::WindowWidth; $script:Rows = [Console]::WindowHeight } catch { }
    $script:W = [Math]::Max(40, [Math]::Min(80, $script:Cols - 6))
    $script:M = [Math]::Max(1, [int](($script:Cols - $script:W) / 2))
}
function Out-Line([string]$s) { [Console]::Write((' ' * $M) + $s + "$E[K`n") }
function Out-Center([int]$vis, [string]$s) { [Console]::Write((" " * [Math]::Max(0, $M + [int](($W - $vis) / 2))) + $s + "$E[K`n") }
function Fit([int]$w, [string]$s) {
    if ($s.Length -le $w) { return $s }
    $a = [int]($w / 2) - 1
    return $s.Substring(0, $a) + $Ch.Ell + $s.Substring($s.Length - ($w - $a - 1))
}
function Human([long]$b) {
    if ($b -ge 1GB) { return ('{0:N2} GB' -f ($b / 1GB)) }
    if ($b -ge 1MB) { return ('{0:N1} MB' -f ($b / 1MB)) }
    return ('{0} KB' -f [long]($b / 1KB))
}

$RidgeHeight = @(0, 10, 13, 16)
function Get-Ridge([int]$row, [int]$lit) {  # one of the two rows of the three-peak mountain
    $blocks = @(' ') + (0..7 | ForEach-Object { [string][char](0x2581 + $_) })
    $centers = @(0, [int]($W * 18 / 100), [int]($W * 47 / 100), [int]($W * 78 / 100))
    $sb = New-Object Text.StringBuilder; $cur = ''
    for ($x = 0; $x -lt $W; $x++) {
        $best = 0; $owner = 0
        for ($p = 1; $p -le 3; $p++) {
            $h = $RidgeHeight[$p] - [Math]::Floor(([Math]::Abs($x - $centers[$p]) * 5 + 2) / 4)
            if ($h -gt $best) { $best = $h; $owner = $p }
        }
        if ($best -lt 1) { $best = 1 }
        $lvl = if ($row -eq 0) { $best - 8 } else { $best }
        $lvl = [Math]::Max(0, [Math]::Min(8, $lvl))
        $col = if ($owner -eq $lit) { $C_OR } elseif ($row -eq 0) { $C_SN } else { $C_DP }
        if ($col -ne $cur) { [void]$sb.Append($col); $cur = $col }
        [void]$sb.Append($blocks[$lvl])
    }
    return $sb.ToString() + $Rst
}

function Show-Header([int]$step) {
    Measure-Screen
    [Console]::Write("$E[H")
    Out-Line ''
    $big = ($Cols -ge ($W + 2)) -and ($Rows -ge 30)
    if ($big) {
        for ($i = 0; $i -lt 5; $i++) { Out-Line ($LogoCol[$i] + $Bo + $It + $Logo[$i] + $Rst) }
        $lit = if ($step -ge 4) { 0 } else { $step }
        Out-Line (Get-Ridge 0 $lit); Out-Line (Get-Ridge 1 $lit)
    } else {
        Out-Line ($LogoCol[1] + $Bo + $It + 'SSX 3' + $Rst)
    }
    Out-Center 33 "${C_IC}R E C O M P$Rst   $C_DIM$($Ch.Mid)$Rst   ${C_SN}game data setup$Rst"
    Out-Line ''
    $trail = ''; $tw = 3 * 8 + 2 * 7
    for ($i = 1; $i -le 3; $i++) {
        if ($i -lt $step) { $mark = $Ch.Dia; $col = $C_GRN }
        elseif ($i -eq $step) { $mark = $Ch.Dia; $col = $C_OR + $Bo }
        else { $mark = $Ch.DiaO; $col = $C_DIM }
        $trail += "$col$mark PEAK $i$Rst"
        if ($i -eq $step) { $trail += " $C_SN$Bo$It$($PeakName[$i])$Rst"; $tw += $PeakName[$i].Length + 1 }
        if ($i -lt 3) { $trail += "  $C_DIM$($Ch.HzB * 3)$Rst  " }
    }
    Out-Center $tw $trail
    Out-Line ''
    $script:HeaderRows = if ($big) { 12 } else { 6 }
}
function Show-Footer([string]$hint) {
    [Console]::Write("$E[J$E[$Rows;1H")
    [Console]::Write((' ' * $M) + $C_DIM + $hint + $Rst + "$E[K")
}
function Show-Panel([string]$title) {
    Out-Line ("$C_OR$Bo$It$($Ch.Tri)$($Ch.Tri) $title$Rst $C_DIM" + ($Ch.Hz * [Math]::Max(0, $W - $title.Length - 4)) + $Rst)
}
function Out-KV([string]$k, [string]$v) { Out-Line ("  $C_DIM" + $k.PadRight(12) + "$Rst $C_SN$v$Rst") }

function Read-MenuKey {
    $k = [Console]::ReadKey($true)
    if ($k.Key -eq 'UpArrow' -or $k.KeyChar -eq 'k') { return 'up' }
    if ($k.Key -eq 'DownArrow' -or $k.KeyChar -eq 'j') { return 'down' }
    if ($k.Key -eq 'Enter') { return 'enter' }
    if ($k.Key -eq 'Escape' -or $k.KeyChar -eq 'q' -or $k.KeyChar -eq 'Q') { return 'esc' }
    return [string]$k.KeyChar
}

# Show-Menu <peak> <title> <default tag> <items: @(tag, label, detail)> <scriptblock drawn above the list>
# -> chosen tag, $null on Esc / q
function Show-Menu([int]$Peak, [string]$Title, [string]$Default, $Items, [scriptblock]$Pre) {
    $n = $Items.Count; $sel = 0; $lw = 0
    for ($i = 0; $i -lt $n; $i++) {
        if ($Items[$i][0] -eq $Default) { $sel = $i }
        if ($Items[$i][1].Length -gt $lw) { $lw = $Items[$i][1].Length }
    }
    while ($true) {
        Show-Header $Peak
        Show-Panel $Title
        if ($Pre) { & $Pre }
        Out-Line ''
        for ($i = 0; $i -lt $n; $i++) {
            $det = Fit ($W - $lw - 7) $Items[$i][2]
            $lab = $Items[$i][1].PadRight($lw)
            if ($i -eq $sel) { Out-Line ("$C_OR$Bo$($Ch.Tri)$Rst $B_OR$C_INK$Bo$It $lab $Rst $C_YE$det$Rst") }
            else { Out-Line ("   $C_SN$lab $Rst  $C_DIM$det$Rst") }
        }
        Show-Footer "$($Ch.Arr) choose    $($Ch.Ret) select    esc quit"
        $k = Read-MenuKey
        switch -regex ($k) {
            '^up$'    { $sel = ($sel + $n - 1) % $n }
            '^down$'  { $sel = ($sel + 1) % $n }
            '^[1-9]$' { if ([int]$k -le $n) { $sel = [int]$k - 1 } }
            '^enter$' { return $Items[$sel][0] }
            '^esc$'   { return $null }
        }
    }
}

function Show-Message([int]$Peak, [string]$Title, [string]$Col, [string[]]$Lines) {
    Show-Header $Peak; Show-Panel $Title; Out-Line ''
    foreach ($l in $Lines) { Out-Line "  $Col$l$Rst" }
    Show-Footer "$($Ch.Ret) continue"
    [void][Console]::ReadKey($true)
}

function Enter-Tui { [Console]::Write("$E[?1049h$E[?25l$E[H$E[2J") }
function Exit-Tui { [Console]::Write("$E[?25h$E[?1049l") }

function Say([string]$s) { if (-not $script:TuiActive) { Write-Host $s } }
function Fail([string]$msg) {
    if ($script:TuiActive) { Show-Message 0 'WIPEOUT' $C_RED @($msg); Exit-Tui; $script:TuiActive = $false }
    [Console]::Error.WriteLine("install_game: $msg")
    exit 1
}

# ---- reading the source -> $Files (Ssx3Entry list), $Dirs, $Total, $SrcDesc, $SrcPath ----
function Get-Source([string]$p) {
    $script:Files = New-Object 'System.Collections.Generic.List[Ssx3Entry]'
    $script:Dirs = New-Object 'System.Collections.Generic.List[string]'
    $script:Total = 0L
    if (-not (Test-Path -LiteralPath $p)) { throw "not found: $p" }
    $p = (Resolve-Path -LiteralPath $p).ProviderPath
    if (Test-Path -LiteralPath $p -PathType Container) {
        if (-not (Test-Path -LiteralPath (Join-Path $p 'default.xbe'))) {     # a parent folder with one game folder in it
            $sub = @(Get-ChildItem -LiteralPath $p -Directory -ErrorAction SilentlyContinue |
                     Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'default.xbe') })
            if ($sub.Count -eq 1) { $p = $sub[0].FullName }
        }
        if (-not (Test-Path -LiteralPath (Join-Path $p 'default.xbe'))) {
            throw 'no default.xbe in this folder - pick the folder of the extracted disc'
        }
        $pn = $p.TrimEnd('\', '/'); $dn = $Dest.TrimEnd('\', '/')
        if (-not $Check) {
            if ($pn -eq $dn) { throw 'this is already the target folder' }
            if ($dn.StartsWith($pn + [IO.Path]::DirectorySeparatorChar)) { throw 'the target folder lies inside the source folder' }
        }
        $script:SrcDesc = 'extracted folder'; $script:SrcPath = $p
        foreach ($item in Get-ChildItem -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue) {
            $rel = $item.FullName.Substring($pn.Length + 1).Replace('\', '/')
            if ($item.PSIsContainer) { $script:Dirs.Add($rel) }
            else {
                $ent = New-Object Ssx3Entry; $ent.Path = $rel; $ent.Src = $item.FullName; $ent.Offset = 0; $ent.Size = $item.Length
                $script:Files.Add($ent); $script:Total += $item.Length
            }
        }
    } else {
        $script:SrcPath = $p
        $rs = [uint32]0; $rz = [uint32]0
        $part = [Ssx3Install]::Probe($p, [ref]$rs, [ref]$rz)
        if ($part -lt 0) { throw 'not an Xbox disc image (no XDVDFS file system found)' }
        $script:SrcDesc = if ($part -eq 0) { 'Xbox image (.xiso)' } else { 'full disc image (Redump .iso)' }
        try { [Ssx3Install]::Scan($p, $part, $rs, $rz, '', 0, $script:Files, $script:Dirs) }
        catch { $x = $_.Exception; if ($x.InnerException) { $x = $x.InnerException }; throw $x.Message }
        foreach ($f in $script:Files) { $script:Total += $f.Size }
    }
    if ($script:Files.Count -eq 0) { throw 'the source is empty' }
}

# default.xbe of the scanned source -> $Xbe (Title, Name, Region, Sha1) and $Verdict:
# ok (known build) | pal (SSX 3 PAL, modified) | region (SSX 3, other region) | other | bad
function Get-XbeInfo {
    $script:Xbe = @{ Title = '?'; Name = '?'; Region = '?'; Sha1 = '?' }; $script:Verdict = 'bad'
    $f = $script:Files | Where-Object { $_.Path -ieq 'default.xbe' } | Select-Object -First 1
    if (-not $f) { return }
    $b = [Ssx3Install]::Read($f.Src, $f.Offset, 8192)
    if (-not $b -or [Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'XBEH') { return }
    $cert = [BitConverter]::ToUInt32($b, 0x118) - [BitConverter]::ToUInt32($b, 0x104)
    if ($cert -lt 0 -or $cert + 0xA4 -gt 8192) { return }
    $script:Xbe.Title = '{0:X8}' -f [BitConverter]::ToUInt32($b, $cert + 8)
    $name = [Text.Encoding]::Unicode.GetString($b, $cert + 12, 80)
    $script:Xbe.Name = ($name -split [char]0)[0]
    $reg = [BitConverter]::ToUInt32($b, $cert + 0xA0)
    $parts = @()
    if ($reg -band 1) { $parts += 'NTSC-U' }; if ($reg -band 2) { $parts += 'NTSC-J' }; if ($reg -band 4) { $parts += 'PAL' }
    $script:Xbe.Region = if ($reg -eq 7) { 'region-free' } else { $parts -join ' ' }
    $script:Xbe.Sha1 = [Ssx3Install]::Sha1($f.Src, $f.Offset, $f.Size)
    if ($script:Xbe.Sha1 -eq $Sha1Orig -or $script:Xbe.Sha1 -eq $Sha1Patched) { $script:Verdict = 'ok' }
    elseif ($script:Xbe.Title -ne $TitleId) { $script:Verdict = 'other' }
    elseif ($reg -band 4) { $script:Verdict = 'pal' }
    else { $script:Verdict = 'region' }
}
function Get-VerdictText {  # -> @(text, colour)
    switch ($script:Verdict) {
        'ok'     { return @("SSX 3 $($Ch.Mid) PAL $($Ch.Mid) the exact build this port was made from $($Ch.Check)", $C_GRN) }
        'pal'    { return @("SSX 3 $($Ch.Mid) PAL $($Ch.Mid) modified default.xbe (e.g. media patch), should work", $C_YE) }
        'region' { return @("SSX 3 $($Ch.Mid) $($script:Xbe.Region) - only the PAL version is supported", $C_RED) }
        'other'  { return @("not SSX 3: $($script:Xbe.Name) (title ID $($script:Xbe.Title))", $C_RED) }
        default  { return @('no readable default.xbe', $C_RED) }
    }
}
function Test-Good { return ($script:Verdict -eq 'ok' -or $script:Verdict -eq 'pal') }

function Get-FreeSpace {
    $d = $Dest; while (-not (Test-Path -LiteralPath $d)) { $d = Split-Path -Parent $d; if (-not $d) { return [long]::MaxValue } }
    try { return (New-Object IO.DriveInfo ([IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $d).ProviderPath))).AvailableFreeSpace }
    catch { return [long]::MaxValue }
}

function Get-ExistingInfo {
    $x = Join-Path $Dest 'default.xbe'
    if (-not (Test-Path -LiteralPath $x)) { return '' }
    $h = [Ssx3Install]::Sha1($x, 0, (Get-Item -LiteralPath $x).Length)
    if ($h -eq $Sha1Orig -or $h -eq $Sha1Patched) { return "SSX 3 PAL is already installed $($Ch.Check)" }
    return 'game_files already holds a default.xbe (unknown build)'
}

# disc images (.iso/.xiso > 100 MB, Xbox only) and folders with default.xbe in the usual places
function Find-Candidates {
    $roots = @($PSScriptRoot, $Root, (Split-Path -Parent $Root))
    $up = [Environment]::GetFolderPath('UserProfile')
    foreach ($s in 'Downloads', 'Desktop', 'Documents', 'Games', '') { $roots += (Join-Path $up $s) }
    $seen = @{}; $out = @()
    foreach ($r in $roots) {
        if (-not $r -or -not (Test-Path -LiteralPath $r -PathType Container)) { continue }
        $items = @(Get-ChildItem -LiteralPath $r -File -ErrorAction SilentlyContinue) +
                 @(Get-ChildItem -LiteralPath $r -Directory -ErrorAction SilentlyContinue |
                   ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -File -ErrorAction SilentlyContinue })
        foreach ($f in $items) {
            if ($f.Extension -notmatch '^\.(x?iso)$' -or $f.Length -lt 100MB -or $seen[$f.FullName]) { continue }
            $seen[$f.FullName] = 1
            $a = [uint32]0; $b = [uint32]0
            try { if ([Ssx3Install]::Probe($f.FullName, [ref]$a, [ref]$b) -ge 0) { $out += $f.FullName } } catch { }
        }
        foreach ($f in $items) {
            if ($f.Name -ne 'default.xbe' -or $f.Length -lt 1MB) { continue }
            $d = $f.DirectoryName
            if ($d.TrimEnd('\') -eq $Dest.TrimEnd('\') -or $seen[$d]) { continue }
            $seen[$d] = 1; $out += $d
        }
        if ($out.Count -ge 12) { break }
    }
    return ,$out
}

# ---- copying with the boost meter ----
function Show-Progress([long]$got, [int]$idx) {
    $pct = if ($script:Total -gt 0) { [int][Math]::Floor($got * 100 / $script:Total) } else { 100 }
    if (-not $script:TuiActive) {
        if ($pct -ge $script:NextPct) { Write-Host ('  {0,3}%  {1}' -f $pct, $script:Files[$idx].Path); $script:NextPct = $pct + 10 }
        return
    }
    $bw = $W - 12
    $cells = if ($script:Total -gt 0) { [long][Math]::Floor($got * $bw * 8 / $script:Total) } else { $bw * 8 }
    $sb = New-Object Text.StringBuilder
    for ($i = 0; $i -lt $bw; $i++) {
        if ([Math]::Floor($cells / 8) -gt $i) {
            $c = if ($i * 3 -lt $bw) { $C_OR } elseif ($i * 3 -lt $bw * 2) { $LogoCol[3] } else { $C_YE }
            [void]$sb.Append($c + $Ch.Full)
        } elseif ([Math]::Floor($cells / 8) -eq $i -and $cells % 8 -gt 0) {
            [void]$sb.Append($C_YE + [string][char](0x2590 - ($cells % 8)))     # left 1/8 .. 7/8 block
        } else { [void]$sb.Append($C_DIM + $Ch.Mid) }
    }
    $el = [Math]::Max(1.0, ((Get-Date) - $script:T0).TotalSeconds)
    $speed = [long]($got / $el); $eta = '--:--'
    if ($speed -gt 0) { $t = [int](($script:Total - $got) / $speed); $eta = '{0}:{1:D2}' -f [int][Math]::Floor($t / 60), ($t % 60) }
    [Console]::Write("$E[$($script:ProgRow);1H")
    Out-Line ("  ${C_DIM}BOOST$Rst " + $sb.ToString() + "$Rst $C_SN$Bo" + ('{0,3}%' -f $pct) + $Rst)
    Out-Line ''
    Out-KV 'carving' (Fit ($W - 16) $script:Files[$idx].Path)
    Out-KV 'distance' ("$(Human $got) of $(Human $script:Total)")
    Out-KV 'speed' ("$(Human $speed)/s    ${C_DIM}ETA$Rst $C_SN$eta")
}

function Install-Files {
    $script:T0 = Get-Date; $script:NextPct = 0
    if ($script:TuiActive) {
        Show-Header 3; Show-Panel 'DROP IN'; Out-Line ''
        Out-Line ("  ${C_SN}Copying $($script:Files.Count) files to$Rst $C_IC$(Fit ($W - 24) $Dest)$Rst")
        Out-Line ''
        $script:ProgRow = $script:HeaderRows + 5
        Show-Footer 'ctrl+c abort (run it again to finish)'
    } else {
        Say "Copying $($script:Files.Count) files ($(Human $script:Total)) to $Dest"
    }
    try {
        [void](New-Item -ItemType Directory -Force -Path $Dest)
        foreach ($d in $script:Dirs) { [void](New-Item -ItemType Directory -Force -Path (Join-Path $Dest $d)) }
        $got = 0L; $last = [DateTime]::MinValue
        for ($i = 0; $i -lt $script:Files.Count; $i++) {
            $f = $script:Files[$i]
            $dst = Join-Path $Dest $f.Path
            [void](New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst))
            [IO.File]::WriteAllBytes($dst, [byte[]]@())                # create / truncate
            $pos = 0L
            while ($pos -lt $f.Size) {
                $n = [int][Math]::Min($Chunk, $f.Size - $pos)
                [Ssx3Install]::CopyChunk($f.Src, $f.Offset + $pos, $dst, $pos, $n)
                $pos += $n; $got += $n
                if (((Get-Date) - $last).TotalMilliseconds -ge 100) { Show-Progress $got $i; $last = Get-Date }
            }
            if ((Get-Item -LiteralPath $dst).Length -ne $f.Size) { throw "size mismatch after copying $($f.Path) (disk full?)" }
        }
    } catch {
        $choice = $_.Exception.Message; if ($_.Exception.InnerException) { $choice = $_.Exception.InnerException.Message }
        Fail "copy failed: $choice"
    }
    $script:NextPct = 0; Show-Progress $script:Total ($script:Files.Count - 1)
    $script:Elapsed = [int]((Get-Date) - $script:T0).TotalSeconds
}

# ---- steps ----
function Read-SourcePath {
    Show-Header 1; Show-Panel 'PICK YOUR LINE'; Out-Line ''
    Out-Line "  ${C_SN}Path to the disc image (.iso / .xiso) or to the extracted folder:$Rst"
    Out-Line "  ${C_DIM}drag & drop a file onto this window to paste its path $($Ch.Mid) empty = back$Rst"
    Out-Line ''; [Console]::Write("$E[J" + (' ' * $M) + "  $C_OR$Bo$($Ch.Tri)$Rst ")
    [Console]::Write("$E[?25h")
    $p = [Console]::ReadLine()
    [Console]::Write("$E[?25l")
    if ($null -eq $p) { return '' }
    $p = $p.Trim().Trim('"').Trim("'")
    if ($p.StartsWith('~')) { $p = [Environment]::GetFolderPath('UserProfile') + $p.Substring(1) }
    return $p
}

function Select-ByDialog([bool]$folder) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        if ($folder) {
            $d = New-Object System.Windows.Forms.FolderBrowserDialog
            $d.Description = 'Extracted SSX 3 disc folder (the one with default.xbe)'
            if ($d.ShowDialog() -eq 'OK') { return $d.SelectedPath }
        } else {
            $d = New-Object System.Windows.Forms.OpenFileDialog
            $d.Title = 'SSX 3 disc image'; $d.Filter = 'Xbox disc images (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*'
            if ($d.ShowDialog() -eq 'OK') { return $d.FileName }
        }
    } catch { }
    return ''
}

function Select-Source {  # -> path, '' to show the menu again, $null to quit
    $cands = Find-Candidates; $script:Cands = $cands
    $items = @()
    for ($i = 0; $i -lt $cands.Count; $i++) {
        $c = $cands[$i]
        if (Test-Path -LiteralPath $c -PathType Container) { $kind = 'folder' }
        else {
            $a = [uint32]0; $b = [uint32]0
            $kind = (Human (Get-Item -LiteralPath $c).Length) + ' ' +
                    $(if ([Ssx3Install]::Probe($c, [ref]$a, [ref]$b) -eq 0) { 'xiso' } else { 'disc image' })
        }
        $dir = Split-Path -Parent $c
        $up = [Environment]::GetFolderPath('UserProfile')
        if ($dir.StartsWith($up)) { $dir = '~' + $dir.Substring($up.Length) }
        $items += ,@("c$i", (Fit 26 (Split-Path -Leaf $c)), "$kind  in $dir")
    }
    $items += ,@('path', "Type a path$($Ch.Ell)", 'drag & drop works')
    if ($OnWindows) {
        $items += ,@('bimg', "Browse for an image$($Ch.Ell)", 'file dialog: .iso / .xiso')
        $items += ,@('bdir', "Browse for a folder$($Ch.Ell)", 'file dialog: the extracted disc folder')
    }
    $items += ,@('quit', 'Quit', '')
    $pre = {
        Out-Line ''
        Out-Line "  ${C_SN}Where is your SSX 3 disc? A disc image or an extracted folder.$Rst"
        Out-Line "  ${C_DIM}target  $(Fit ($W - 12) $Dest)$Rst"
        if ($script:ExistText) { Out-Line "  $C_GRN$($script:ExistText)$Rst" }
        if ($script:Cands.Count -eq 0) { Out-Line "  ${C_DIM}(nothing found in the usual places - type a path or browse)$Rst" }
    }
    $def = if ($script:LastPick) { $script:LastPick } else { 'c0' }
    $choice = Show-Menu 1 'PICK YOUR LINE' $def $items $pre
    if (-not $choice -or $choice -eq 'quit') { return $null }
    $script:LastPick = $choice
    switch ($choice) {
        'path' { return Read-SourcePath }
        'bimg' { return Select-ByDialog $false }
        'bdir' { return Select-ByDialog $true }
        default { return $cands[[int]$choice.Substring(1)] }
    }
}

function Invoke-Plain {
    $src = $Source
    if ($Check -and -not $src) { $src = $Dest }
    if (-not $src) { Fail 'no source given. Usage: install_game.ps1 [-Yes] <SSX 3 .iso | .xiso | extracted folder>' }
    Say 'SSX3-Recomp game data setup'
    Say "[1/3] Source: $src"
    try { Get-Source $src } catch { Fail $_.Exception.Message }
    Get-XbeInfo
    Say "[2/3] $($script:SrcDesc), $($script:Files.Count) files, $(Human $script:Total)"
    Say "      game: $($script:Xbe.Name) (title ID $($script:Xbe.Title), $($script:Xbe.Region)), default.xbe SHA-1 $($script:Xbe.Sha1)"
    Say "      check: $((Get-VerdictText)[0])"
    if ($Check) { if (Test-Good) { exit 0 } else { exit 1 } }
    if (-not (Test-Good) -and -not $Yes) { Fail 'not the supported game version (see above); -Yes copies anyway' }
    $free = Get-FreeSpace
    if ($free -lt $script:Total) { Fail "not enough free space at $Dest ($(Human $free) free)" }
    $ex = Get-ExistingInfo; if ($ex) { Say "      target: $ex - overwriting" }
    if (-not $Yes) {
        $a = Read-Host "Copy to $Dest? [Y/n]"
        if ($a -and $a -notmatch '^(y|yes)$') { exit 1 }
    }
    Say '[3/3] Installing'
    Install-Files
    Say "Done in $($script:Elapsed)s. Start the game with run.bat"
    exit 0
}

function Invoke-Tui {
    Enter-Tui; $script:TuiActive = $true
    try {
        $script:ExistText = Get-ExistingInfo
        $pick = $Source
        while ($true) {
            if (-not $pick) {
                $pick = Select-Source
                if ($null -eq $pick) { $script:ExitCode = 1; return }
                if (-not $pick) { continue }
            }
            Show-Header 2; Show-Panel 'CHECK THE GEAR'; Out-Line ''
            Out-Line "  ${C_SN}Reading $(Fit ($W - 12) $pick) $($Ch.Ell)$Rst"; [Console]::Write("$E[J")
            try { Get-Source $pick } catch {
                Show-Message 1 'WIPEOUT' $C_RED @($_.Exception.Message, '', "${C_DIM}Pick another source.")
                $pick = ''; continue
            }
            $pick = ''
            Get-XbeInfo
            $free = Get-FreeSpace; $script:Free = $free
            $script:Vt = Get-VerdictText
            $pre = {
                Out-Line ''
                Out-KV 'source' (Fit ($W - 16) $script:SrcPath)
                Out-KV 'type' $script:SrcDesc
                Out-KV 'contents' ("$($script:Files.Count) files $($Ch.Mid) $(Human $script:Total)")
                Out-KV 'game' ("$($script:Xbe.Name)  ${C_DIM}title ID $($script:Xbe.Title) $($Ch.Mid) $($script:Xbe.Region)")
                Out-Line ("  $C_DIM" + 'check'.PadRight(12) + "$Rst $($script:Vt[1])$($script:Vt[0])$Rst")
                Out-KV 'target' (Fit ($W - 16) $Dest)
                if ($script:Free -lt $script:Total) { Out-KV 'free space' "$C_RED$(Human $script:Free) - not enough room" }
                else { Out-KV 'free space' (Human $script:Free) }
                if ($script:ExistText) { Out-Line "  $C_YE$($script:ExistText) - its files get overwritten$Rst" }
            }
            $items = @()
            if (Test-Good) { $items += ,@('go', 'Drop in', 'copy the game data') }
            else { $items += ,@('go', 'Copy anyway', 'this is not the supported SSX 3 PAL disc') }
            $items += ,@('back', 'Pick another source', '')
            $items += ,@('quit', 'Quit', '')
            $choice = Show-Menu 2 'CHECK THE GEAR' $(if (Test-Good) { 'go' } else { 'back' }) $items $pre
            if (-not $choice -or $choice -eq 'quit') { $script:ExitCode = 1; return }
            if ($choice -eq 'back') { continue }
            if ($free -lt $script:Total) {
                Show-Message 2 'WIPEOUT' $C_RED @("Not enough free space at the target ($(Human $free) free).")
                continue
            }
            Install-Files
            $script:FinT = '{0}:{1:D2}' -f [int][Math]::Floor($script:Elapsed / 60), ($script:Elapsed % 60)
            $runBat = Join-Path $PSScriptRoot 'run.bat'
            $items = @()
            if ($env:SSX3_INSTALL_FROM_RUN -or (Test-Path -LiteralPath $runBat)) { $items += ,@('run', 'Ride now', 'start SSX 3') }
            $items += ,@('quit', 'Quit', 'start it later with run.bat')
            $pre = {
                Out-Line ''
                Out-Center 43 "$C_YE$Bo$It$($Ch.Star)  G A M E   D A T A   I N S T A L L E D  $($Ch.Star)$Rst"
                Out-Line ''
                Out-KV 'copied' ("$($script:Files.Count) files $($Ch.Mid) $(Human $script:Total) in $($script:FinT)")
                Out-KV 'to' (Fit ($W - 16) $Dest)
            }
            $choice = Show-Menu 4 'PEAK CONQUERED' 'run' $items $pre
            if ($env:SSX3_INSTALL_FROM_RUN) {                  # run.ps1 waits for us and launches the game itself
                $script:ExitCode = $(if ($choice -eq 'run') { 0 } else { 10 }); return
            }
            $script:ExitCode = 0
            if ($choice -eq 'run') { $script:RunAfter = $runBat }
            return
        }
    } finally {
        if ($script:TuiActive) { Exit-Tui; $script:TuiActive = $false }
    }
}

$script:TuiActive = $false; $script:ExitCode = 0; $script:RunAfter = $null
if (-not $Tui) { Invoke-Plain }
Invoke-Tui
if ($script:RunAfter) { & cmd.exe /c "`"$($script:RunAfter)`""; exit $LASTEXITCODE }
exit $script:ExitCode
