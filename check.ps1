# Diagnostics for machines where presolve/start do not work. Writes check_result.txt next to this script.
# Kept compatible with old PowerShell (2.0) so it runs everywhere.
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir
$out = Join-Path $dir 'check_result.txt'
$lines = New-Object System.Collections.ArrayList
function Say($t) { [void]$lines.Add([string]$t); Write-Host $t }
function Try-Step($name, [scriptblock]$b) { try { Say ("[OK]   {0}: {1}" -f $name, (& $b)) } catch { Say ("[FAIL] {0}: {1}" -f $name, $_.Exception.Message) } }

Say ('==== check ' + (Get-Date).ToString('yyyy-MM-dd HH:mm') + ' ====')
Try-Step 'Windows' { $o = Get-WmiObject Win32_OperatingSystem; '{0} {1} ({2})' -f $o.Caption, $o.Version, $o.OSArchitecture }
Try-Step '64-bit Windows' { [Environment]::Is64BitOperatingSystem }
Try-Step 'PowerShell' { $PSVersionTable.PSVersion.ToString() }
Try-Step '.NET 4.5+' { $r = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\NET Framework Setup\NDP\v4\Full' -ErrorAction Stop).Release; "release $r " + $(if ($r -ge 378389) { '(ok)' } else { '(too old)' }) }
Try-Step 'CPU' { $c = @(Get-WmiObject Win32_Processor)[0]; '{0}, {1} cores / {2} threads' -f $c.Name.Trim(), $c.NumberOfCores, $c.NumberOfLogicalProcessors }
Try-Step 'RAM' { $o = Get-WmiObject Win32_OperatingSystem; '{0:N1} GB total, {1:N1} GB free' -f ($o.TotalVisibleMemorySize / 1MB), ($o.FreePhysicalMemory / 1MB) }
Try-Step 'Disk free here' { $d = Get-WmiObject Win32_LogicalDisk -Filter ("DeviceID='" + $dir.Substring(0, 2) + "'"); '{0:N1} GB' -f ($d.FreeSpace / 1GB) }
foreach ($f in 'solver.exe', 'ranges_6max.txt', 'presolve.txt', 'presolve.ps1', 'config.txt', 'viewer.html', 'server.ps1') { Try-Step "file $f" { if (Test-Path (Join-Path $dir $f)) { 'found' } else { throw 'MISSING' } } }

Try-Step 'C# helper (Add-Type)' { Add-Type -TypeDefinition 'public static class ChkT { public static int Two() { return 2; } }'; [ChkT]::Two() }
Try-Step 'gzip' { $m = New-Object IO.MemoryStream; $g = New-Object IO.Compression.GZipStream($m, [IO.Compression.CompressionLevel]::Optimal); $g.Close(); 'ok' }

# a tiny river solve; exit code -1073741795 (0xC000001D) = CPU lacks an instruction, -1073741515 = missing DLL
$tmp = Join-Path $dir '_check'
[IO.File]::WriteAllText("$tmp.txt", "board = Qs Jh 2h 7c 3d`r`npot = 10`r`nstack = 50`r`nrange_oop = 22+, AJs+, KQs`r`nrange_ip = 22+, ATs+, KJs+`r`noop_river_bet = 66`r`nip_river_bet = 66`r`nraise_cap = 1`r`niterations = 20`r`nthreads = 0`r`noutput = _check.js`r`n")
Try-Step 'solver.exe test run' {
    $p = Start-Process -FilePath (Join-Path $dir 'solver.exe') -ArgumentList '_check.txt' -WorkingDirectory $dir -NoNewWindow -PassThru -RedirectStandardOutput "$tmp.log" -RedirectStandardError "$tmp.err"
    $null = $p.Handle
    if (-not $p.WaitForExit(120000)) { $p.Kill(); throw 'timed out after 120s' }
    $msg = (Get-Content "$tmp.log", "$tmp.err" -ErrorAction SilentlyContinue) -join ' | '
    if ($p.ExitCode -ne 0) { throw ('exit code {0}: {1}' -f $p.ExitCode, $msg) }
    'ok - ' + (($msg -split '\|') | Where-Object { $_ -match 'Total' })
}
Remove-Item "$tmp.txt", "$tmp.log", "$tmp.err", "$tmp.js" -ErrorAction SilentlyContinue

Try-Step 'local web server (start.bat)' {
    $l = New-Object System.Net.HttpListener; $l.Prefixes.Add('http://localhost:8799/'); $l.Start()
    try {
        $ar = $l.BeginGetContext($null, $null)
        $wc = New-Object System.Net.WebClient; $req = $wc.DownloadStringAsync((New-Object Uri 'http://localhost:8799/'))
        if (-not $ar.AsyncWaitHandle.WaitOne(10000)) { throw 'server started but the request never arrived (firewall / proxy / security software?)' }
        $ctx = $l.EndGetContext($ar); $b = [Text.Encoding]::ASCII.GetBytes('ok'); $ctx.Response.OutputStream.Write($b, 0, 2); $ctx.Response.Close()
        'ok'
    } finally { $l.Stop() }
}
Try-Step 'proxy for localhost' { $p = [Net.WebRequest]::GetSystemWebProxy(); $u = New-Object Uri 'http://localhost:8765/'; if ($p.IsBypassed($u)) { 'not proxied (ok)' } else { 'PROXIED via ' + $p.GetProxy($u) + ' - browser may not reach localhost' } }
Try-Step 'ports 8765-8785 in use' { $n = @(netstat -ano | Select-String ':87(6[5-9]|7[0-9]|8[0-5])\s.*LISTENING'); if ($n.Count) { ($n | ForEach-Object { $_.Line.Trim() }) -join ' / ' } else { 'none' } }
Try-Step 'last presolve error' { $e = @(Get-ChildItem (Join-Path $dir 'library') -Recurse -Filter '_error.txt' -ErrorAction SilentlyContinue); if ($e.Count) { ($e | ForEach-Object { $_.Directory.Name + ': ' + (Get-Content $_.FullName) }) -join ' / ' } else { 'none' } }
[IO.File]::WriteAllLines($out, [string[]]$lines.ToArray())
Write-Host ''
Write-Host "Saved to $out  - send this file (or a screenshot of this window)." -ForegroundColor Cyan
