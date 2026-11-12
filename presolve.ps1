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
$own = @('spots', 'flops', 'library', 'priority', 'keep_awake', 'parallel_jobs', 'gzip')
$spots = New-Object System.Collections.Generic.List[string]
$opt = @{ flops = 'all'; library = 'library'; priority = 'BelowNormal'; keep_awake = 'true'; parallel_jobs = '0'; gzip = 'true' }
$base = New-Object System.Text.StringBuilder
$threads = 0
foreach ($line in [IO.File]::ReadAllLines((Join-Path $dir 'presolve.txt'))) {
    $l = $line.TrimStart([char]0xFEFF); $h = $l.IndexOf('#'); if ($h -ge 0) { $l = $l.Substring(0, $h) }
    $e = $l.IndexOf('='); if ($e -lt 0) { continue }
    $k = $l.Substring(0, $e).Trim().ToLower(); $v = $l.Substring($e + 1).Trim()
    if ($k -eq 'spots') { foreach ($s in $v.Split(';')) { if ($s.Trim()) { $spots.Add($s.Trim()) } } }
    elseif ($k -eq 'threads') { $threads = [int]$v }
    elseif ($own -contains $k) { $opt[$k] = $v }
    else { [void]$base.AppendLine("$k = $v") }
}
$lib = Join-Path $dir $opt.library
New-Item -ItemType Directory -Force $lib | Out-Null
$cpu = [Environment]::ProcessorCount
if ($threads -le 0) { $threads = $cpu }
$nJobs = [int]$opt.parallel_jobs
if ($nJobs -le 0) { $nJobs = [Math]::Max(1, [Math]::Floor($threads / 8)) }  # measured: a few smaller solves side by side beat one big one
$perJob = [Math]::Max(1, [Math]::Floor($threads / $nJobs))
$useGz = $opt.gzip -eq 'true'

if ($opt.flops -eq 'all') {
    $cache = Join-Path $lib 'flops.txt'
    if (Test-Path $cache) { $flops = @([IO.File]::ReadAllLines($cache)) }
    else { Write-Host 'Building flop list...'; $flops = [Flop]::All(); [IO.File]::WriteAllLines($cache, $flops) }
} else {
    $flops = foreach ($f in $opt.flops.Split(',')) {
        $t = ($f -replace '\s', ''); if ($t.Length -ne 6) { continue }
        [Flop]::Name([Flop]::Canon([Flop]::Parse($t.Substring(0, 2)), [Flop]::Parse($t.Substring(2, 2)), [Flop]::Parse($t.Substring(4, 2))))
    }
    $flops = @($flops | Select-Object -Unique)
}

$spotList = @(foreach ($s in $spots) {
    if ($s -notmatch '^\s*(\w+)\s+vs\s+(\w+)\s+(srp|3bp|4bp)\s*$') { Write-Host "Skipping bad spot: $s" -ForegroundColor Yellow; continue }
    [pscustomobject]@{ A = $Matches[1].ToUpper(); B = $Matches[2].ToUpper(); T = $Matches[3]; Dir = ('{0}_vs_{1}_{2}' -f $Matches[1].ToUpper(), $Matches[2].ToUpper(), $Matches[3]) }
})
$total = $spotList.Count * $flops.Count
if ($opt.keep_awake -eq 'true') { [Flop]::KeepAwake($true) }

Write-Host ("Spots: {0}   flops per spot: {1}   total solves: {2}" -f $spotList.Count, $flops.Count, $total)
Write-Host ("Running {0} solves at a time, {1} threads each.  Library: {2}" -f $nJobs, $perJob, $lib)
Write-Host 'You can close this window at any time; it resumes where it stopped.' -ForegroundColor Cyan
Write-Host ''

function OutPath($sd, $f) { if ($useGz) { Join-Path $sd "$f.js.gz" } else { Join-Path $sd "$f.js" } }
function IsDone($sd, $f) { (Test-Path (Join-Path $sd "$f.js.gz")) -or (Test-Path (Join-Path $sd "$f.js")) }
function Gzip($src, $dst) {
    $tmp = "$dst.part"
    $in = [IO.File]::OpenRead($src); $out = [IO.File]::Create($tmp)
    $gz = New-Object IO.Compression.GZipStream($out, [IO.Compression.CompressionLevel]::Optimal)
    try { $in.CopyTo($gz) } finally { $gz.Close(); $out.Close(); $in.Close() }
    Move-Item $tmp $dst -Force; Remove-Item $src
}

# queue of (spot, flop) still to do
$queue = New-Object System.Collections.Generic.Queue[object]
$done = 0; $bad = @{}
foreach ($j in $spotList) {
    $sd = Join-Path $lib $j.Dir; New-Item -ItemType Directory -Force $sd | Out-Null
    if (Test-Path (Join-Path $sd '_error.txt')) { Write-Host "$($j.Dir): skipped (see _error.txt)" -ForegroundColor Yellow; $bad[$j.Dir] = $true; $done += $flops.Count; continue }
    $have = 0
    foreach ($f in $flops) { if (IsDone $sd $f) { $have++ } else { $queue.Enqueue([pscustomobject]@{ J = $j; F = $f; Sd = $sd }) } }
    $done += $have
    Write-Host ("  {0,-16} {1,5}/{2} done" -f $j.Dir, $have, $flops.Count)
}
Write-Host ''

$slots = @{}; $solved = 0; $start = Get-Date
while ($queue.Count -gt 0 -or $slots.Count -gt 0) {
    # start new solves in free slots
    for ($i = 0; $i -lt $nJobs -and $queue.Count -gt 0; $i++) {
        if ($slots.ContainsKey($i)) { continue }
        $it = $queue.Dequeue()
        if ($bad[$it.J.Dir]) { $done++; $i--; continue }
        $tmp = Join-Path $lib "_job$i"
        $board = '{0} {1} {2}' -f $it.F.Substring(0, 2), $it.F.Substring(2, 2), $it.F.Substring(4, 2)
        $cfg = $base.ToString() + "threads = $perJob`r`nboard = $board`r`nmatchup = $($it.J.A) vs $($it.J.B)`r`npot_type = $($it.J.T)`r`noutput = $tmp.js`r`n"
        [IO.File]::WriteAllText("$tmp.txt", $cfg)
        Remove-Item "$tmp.js" -ErrorAction SilentlyContinue
        $p = Start-Process -FilePath $exe -ArgumentList "`"$tmp.txt`"" -WorkingDirectory $dir -NoNewWindow -PassThru -RedirectStandardOutput "$tmp.log" -RedirectStandardError "$tmp.err"
        $null = $p.Handle
        try { $p.PriorityClass = $opt.priority } catch {}
        $slots[$i] = [pscustomobject]@{ P = $p; It = $it; Tmp = $tmp; T0 = Get-Date }
    }
    Start-Sleep -Milliseconds 300
    foreach ($i in @($slots.Keys)) {
        $s = $slots[$i]; if (-not $s.P.HasExited) { continue }
        $slots.Remove($i)
        $it = $s.It; $tmp = $s.Tmp; $sec = ((Get-Date) - $s.T0).TotalSeconds
        $err = [IO.File]::ReadAllText("$tmp.err")
        $done++
        if ($s.P.ExitCode -ne 0 -or -not (Test-Path "$tmp.js")) {
            if ($err -match 'no entry for|too small|must act before|unknown position') {
                if (-not $bad[$it.J.Dir]) {
                    [IO.File]::WriteAllText((Join-Path $it.Sd '_error.txt'), $err)
                    Write-Host "  $($it.J.Dir) cannot be solved: $($err.Trim())  -> skipping this spot" -ForegroundColor Red
                }
                $bad[$it.J.Dir] = $true
            } else { Write-Host "  $($it.J.Dir) $($it.F) failed: $($err.Trim())" -ForegroundColor Red }
            continue
        }
        if ($useGz) { Gzip "$tmp.js" (OutPath $it.Sd $it.F) } else { Move-Item "$tmp.js" (OutPath $it.Sd $it.F) -Force }
        $solved++
        $ex = ([regex]::Matches([IO.File]::ReadAllText("$tmp.log"), '\(([\d.]+)% pot\)') | Select-Object -Last 1).Groups[1].Value
        $rate = $solved / ((Get-Date) - $start).TotalSeconds
        $eta = [TimeSpan]::FromSeconds(($total - $done) / $rate)
        Write-Host ('[{0}/{1} {2:P1}] {3} {4}  {5:N0}s  expl {6}%  ETA {7}d {8:hh\:mm}' -f $done, $total, ($done / $total), $it.J.Dir, $it.F, $sec, $ex, [int]$eta.TotalDays, $eta)
        $Host.UI.RawUI.WindowTitle = "Presolve $done/$total  ETA $([int]$eta.TotalDays)d $($eta.ToString('hh\:mm'))"
    }
}
Get-ChildItem $lib -Filter '_job*' | Remove-Item -ErrorAction SilentlyContinue
[Flop]::KeepAwake($false)
Write-Host ''
Write-Host 'All done.' -ForegroundColor Green
