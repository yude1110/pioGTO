# Pre-solver: runs solver.exe over every (spot x flop) listed in presolve.txt and stores results in a library folder.
# Started by presolve.bat. Safe to close at any time; finished flops are skipped on the next run.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir
$exe = Join-Path $dir 'solver.exe'
if (-not (Test-Path $exe)) { Write-Host 'solver.exe not found - run build.bat first'; exit 1 }

Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class Flop {
    const string R = "23456789TJQKA", S = "cdhs";
    static readonly List<int[]> P = new List<int[]>();
    static Flop() { for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 4; c++) for (int d = 0; d < 4; d++)
        if (a != b && a != c && a != d && b != c && b != d && c != d) P.Add(new[] { a, b, c, d }); }
    static int Key(int x, int y, int z, int[] p) {
        x = (x & ~3) | p[x & 3]; y = (y & ~3) | p[y & 3]; z = (z & ~3) | p[z & 3];
        int t; if (x < y) { t = x; x = y; y = t; } if (y < z) { t = y; y = z; z = t; } if (x < y) { t = x; x = y; y = t; }
        return x * 2704 + y * 52 + z;
    }
    // smallest key over all suit permutations; flops with the same key are strategically identical
    public static int Canon(int x, int y, int z) { int m = int.MaxValue; foreach (var p in P) m = Math.Min(m, Key(x, y, z, p)); return m; }
    public static string Name(int key) { return Card(key / 2704) + Card(key / 52 % 52) + Card(key % 52); }
    public static string Card(int c) { return R[c >> 2].ToString() + S[c & 3]; }
    public static int Parse(string s) { return R.IndexOf(char.ToUpper(s[0])) * 4 + S.IndexOf(char.ToLower(s[1])); }
    public static List<string> All() {
        var r = new List<string>();
        for (int a = 51; a >= 0; a--) for (int b = a - 1; b >= 0; b--) for (int c = b - 1; c >= 0; c--) {
            int k = a * 2704 + b * 52 + c; if (Canon(a, b, c) == k) r.Add(Name(k)); }
        return r;
    }
    [DllImport("kernel32.dll")] static extern uint SetThreadExecutionState(uint f);
    public static void KeepAwake(bool on) { SetThreadExecutionState(on ? 0x80000001u : 0x80000000u); } // ES_CONTINUOUS | ES_SYSTEM_REQUIRED
}
'@

# ---- read presolve.txt
$own = @('spots', 'flops', 'library', 'priority', 'keep_awake')
$spots = New-Object System.Collections.Generic.List[string]
$opt = @{ flops = 'all'; library = 'library'; priority = 'BelowNormal'; keep_awake = 'true' }
$base = New-Object System.Text.StringBuilder
foreach ($line in [IO.File]::ReadAllLines((Join-Path $dir 'presolve.txt'))) {
    $l = $line.TrimStart([char]0xFEFF); $h = $l.IndexOf('#'); if ($h -ge 0) { $l = $l.Substring(0, $h) }
    $e = $l.IndexOf('='); if ($e -lt 0) { continue }
    $k = $l.Substring(0, $e).Trim().ToLower(); $v = $l.Substring($e + 1).Trim()
    if ($k -eq 'spots') { foreach ($s in $v.Split(';')) { if ($s.Trim()) { $spots.Add($s.Trim()) } } }
    elseif ($own -contains $k) { $opt[$k] = $v }
    else { [void]$base.AppendLine("$k = $v") }
}
$lib = Join-Path $dir $opt.library
New-Item -ItemType Directory -Force $lib | Out-Null

if ($opt.flops -eq 'all') {
    $cache = Join-Path $lib 'flops.txt'
    if (Test-Path $cache) { $flops = [IO.File]::ReadAllLines($cache) }
    else { Write-Host 'Building flop list...'; $flops = [Flop]::All(); [IO.File]::WriteAllLines($cache, $flops) }
} else {
    $flops = foreach ($f in $opt.flops.Split(',')) {
        $t = ($f -replace '\s', ''); if ($t.Length -ne 6) { continue }
        [Flop]::Name([Flop]::Canon([Flop]::Parse($t.Substring(0, 2)), [Flop]::Parse($t.Substring(2, 2)), [Flop]::Parse($t.Substring(4, 2))))
    }
    $flops = @($flops | Select-Object -Unique)
}

$jobs = foreach ($s in $spots) {
    if ($s -notmatch '^\s*(\w+)\s+vs\s+(\w+)\s+(srp|3bp|4bp)\s*$') { Write-Host "Skipping bad spot: $s" -ForegroundColor Yellow; continue }
    [pscustomobject]@{ A = $Matches[1].ToUpper(); B = $Matches[2].ToUpper(); T = $Matches[3]; Dir = ('{0}_vs_{1}_{2}' -f $Matches[1].ToUpper(), $Matches[2].ToUpper(), $Matches[3]) }
}
$total = $jobs.Count * $flops.Count
if ($opt.keep_awake -eq 'true') { [Flop]::KeepAwake($true) }

Write-Host ("Spots: {0}   flops per spot: {1}   total solves: {2}" -f $jobs.Count, $flops.Count, $total)
Write-Host "Library: $lib"
Write-Host 'You can close this window at any time; it resumes where it stopped.' -ForegroundColor Cyan
Write-Host ''

$tmpCfg = Join-Path $lib '_job.txt'; $tmpOut = Join-Path $lib '_job.js'; $tmpLog = Join-Path $lib '_job.log'; $tmpErr = Join-Path $lib '_job.err'
$done = 0; $solved = 0; $spent = 0.0
foreach ($j in $jobs) {
    $sd = Join-Path $lib $j.Dir
    New-Item -ItemType Directory -Force $sd | Out-Null
    if (Test-Path (Join-Path $sd '_error.txt')) { Write-Host "$($j.Dir): skipped (see _error.txt)" -ForegroundColor Yellow; $done += $flops.Count; continue }
    $have = @(Get-ChildItem $sd -Filter '*.js' -ErrorAction SilentlyContinue).Count
    Write-Host ("== {0}  ({1}/{2} already done)" -f $j.Dir, [Math]::Min($have, $flops.Count), $flops.Count) -ForegroundColor Green
    foreach ($f in $flops) {
        $out = Join-Path $sd "$f.js"
        if (Test-Path $out) { $done++; continue }
        $board = '{0} {1} {2}' -f $f.Substring(0, 2), $f.Substring(2, 2), $f.Substring(4, 2)
        $cfg = $base.ToString() + "board = $board`r`nmatchup = $($j.A) vs $($j.B)`r`npot_type = $($j.T)`r`noutput = $tmpOut`r`n"
        [IO.File]::WriteAllText($tmpCfg, $cfg)
        Remove-Item $tmpOut -ErrorAction SilentlyContinue
        $t0 = Get-Date
        $p = Start-Process -FilePath $exe -ArgumentList "`"$tmpCfg`"" -WorkingDirectory $dir -NoNewWindow -PassThru -RedirectStandardOutput $tmpLog -RedirectStandardError $tmpErr
        $null = $p.Handle
        try { $p.PriorityClass = $opt.priority } catch {}
        $p.WaitForExit()
        $sec = ((Get-Date) - $t0).TotalSeconds
        $err = [IO.File]::ReadAllText($tmpErr)
        if ($p.ExitCode -ne 0 -or -not (Test-Path $tmpOut)) {
            if ($err -match 'no entry for|too small|must act before|unknown position') {
                [IO.File]::WriteAllText((Join-Path $sd '_error.txt'), $err)
                Write-Host "  $($j.Dir) cannot be solved: $($err.Trim())  -> skipping this spot" -ForegroundColor Red
                $done += $flops.Count - [Array]::IndexOf($flops, $f); break
            }
            Write-Host "  $f failed: $($err.Trim())" -ForegroundColor Red; $done++; continue
        }
        Move-Item $tmpOut $out -Force
        $done++; $solved++; $spent += $sec
        $ex = ([regex]::Matches([IO.File]::ReadAllText($tmpLog), '\(([\d.]+)% pot\)') | Select-Object -Last 1).Groups[1].Value
        $eta = [TimeSpan]::FromSeconds($spent / $solved * ($total - $done))
        $msg = '[{0}/{1} {2:P1}] {3} {4}  {5:N0}s  expl {6}%  ETA {7}d {8:hh\:mm}' -f $done, $total, ($done / $total), $j.Dir, $f, $sec, $ex, [int]$eta.TotalDays, $eta
        Write-Host $msg
        $Host.UI.RawUI.WindowTitle = "Presolve $done/$total  ETA $([int]$eta.TotalDays)d $($eta.ToString('hh\:mm'))"
    }
}
Remove-Item $tmpCfg, $tmpLog, $tmpErr -ErrorAction SilentlyContinue
[Flop]::KeepAwake($false)
Write-Host ''
Write-Host 'All done.' -ForegroundColor Green
