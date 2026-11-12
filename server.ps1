# Local web UI for the solver (Windows PowerShell 5.1+, no extra installs).
# Started by start.bat. Serves viewer.html and runs solver.exe on request.
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $dir
$exe = Join-Path $dir 'solver.exe'
$cfgPath = Join-Path $dir 'config.txt'
$logPath = Join-Path $dir 'solve.log'
$errPath = Join-Path $dir 'solve.err'
$utf8 = New-Object System.Text.UTF8Encoding($true)
$proc = $null

$listener = New-Object System.Net.HttpListener
$port = 0
foreach ($p in 8765..8785) {
    try { $listener.Prefixes.Clear(); $listener.Prefixes.Add("http://localhost:$p/"); $listener.Start(); $port = $p; break }
    catch { $lastErr = $_.Exception.Message; $listener = New-Object System.Net.HttpListener }
}
if (-not $port) { Write-Host "Could not start the local web server on ports 8765-8785: $lastErr" -ForegroundColor Red; exit 1 }
$url = "http://localhost:$port/"
Write-Host "Solver UI running at $url  (keep this window open; close it to stop)" -ForegroundColor Green
try { Start-Process $url } catch { Write-Host "Could not open a browser automatically - open $url yourself." -ForegroundColor Yellow }

function JsonStr([string]$s) {  # ConvertTo-Json is missing on old PowerShell
    $sb = New-Object System.Text.StringBuilder; [void]$sb.Append('"')
    foreach ($ch in $s.ToCharArray()) {
        $c = [int]$ch
        if ($c -eq 34) { [void]$sb.Append('\"') } elseif ($c -eq 92) { [void]$sb.Append('\\') }
        elseif ($c -lt 32) { [void]$sb.Append(('\u{0:x4}' -f $c)) } else { [void]$sb.Append($ch) }
    }
    [void]$sb.Append('"'); $sb.ToString()
}
function ReadShared($path) {
    if (-not (Test-Path $path)) { return '' }
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try { (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
}
function Send($ctx, [int]$code, [string]$type, [byte[]]$bytes) {
    $r = $ctx.Response
    $r.StatusCode = $code; $r.ContentType = $type
    $r.Headers.Add('Cache-Control', 'no-store')
    $r.ContentLength64 = $bytes.Length
    if ($ctx.Request.HttpMethod -ne 'HEAD') { $r.OutputStream.Write($bytes, 0, $bytes.Length) }
    $r.OutputStream.Close()
}
function SendText($ctx, [int]$code, [string]$type, [string]$text) {
    Send $ctx $code "$type; charset=utf-8" ([Text.Encoding]::UTF8.GetBytes($text))
}
function Running { return ($null -ne $proc) -and -not $proc.HasExited }

$types = @{ '.html' = 'text/html; charset=utf-8'; '.js' = 'application/javascript; charset=utf-8'; '.txt' = 'text/plain; charset=utf-8'; '.css' = 'text/css' }

while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    try {
        $path = $ctx.Request.Url.AbsolutePath
        $method = $ctx.Request.HttpMethod
        if ($path -eq '/api/config' -and $method -eq 'GET') {
            SendText $ctx 200 'text/plain' ([IO.File]::ReadAllText($cfgPath))
        }
        elseif ($path -eq '/api/solve' -and $method -eq 'POST') {
            if (Running) { SendText $ctx 409 'text/plain' 'already running'; continue }
            $body = (New-Object IO.StreamReader($ctx.Request.InputStream, [Text.Encoding]::UTF8)).ReadToEnd()
            [IO.File]::WriteAllText($cfgPath, $body, $utf8)
            if (-not (Test-Path $exe)) { SendText $ctx 500 'text/plain' 'solver.exe not found - run build.bat first'; continue }
            $proc = Start-Process -FilePath $exe -ArgumentList 'config.txt' -WorkingDirectory $dir -NoNewWindow -PassThru `
                -RedirectStandardOutput $logPath -RedirectStandardError $errPath
            $null = $proc.Handle  # keep the handle so ExitCode is available later
            SendText $ctx 200 'text/plain' 'started'
        }
        elseif ($path -eq '/api/status') {
            $run = Running
            $code = if ($run -or $null -eq $proc -or $null -eq $proc.ExitCode) { 'null' } else { [string]$proc.ExitCode }
            $log = (ReadShared $logPath) + (ReadShared $errPath)
            $json = '{"running":' + $run.ToString().ToLower() + ',"exit":' + $code + ',"log":' + (JsonStr $log) + '}'
            SendText $ctx 200 'application/json' $json
        }
        elseif ($path -eq '/api/ranges' -and $method -eq 'GET') {
            SendText $ctx 200 'text/plain' ([IO.File]::ReadAllText((Join-Path $dir 'ranges_6max.txt')))
        }
        elseif ($path -eq '/api/ranges' -and $method -eq 'POST') {
            $rf = Join-Path $dir 'ranges_6max.txt'; $bak = Join-Path $dir 'ranges_6max.original.txt'
            if (-not (Test-Path $bak)) { Copy-Item $rf $bak }  # keep the shipped ranges once, for "restore"
            $body = (New-Object IO.StreamReader($ctx.Request.InputStream, [Text.Encoding]::UTF8)).ReadToEnd()
            [IO.File]::WriteAllText($rf, $body, $utf8)
            SendText $ctx 200 'text/plain' 'saved'
        }
        elseif ($path -eq '/api/library') {
            $lib = Join-Path $dir 'library'; $parts = @()
            if (Test-Path $lib) { foreach ($d in Get-ChildItem $lib -Directory) { $parts += ('"{0}":{1}' -f $d.Name, @(Get-ChildItem $d.FullName -File | Where-Object { $_.Name -match '\.js(\.gz)?$' -and $_.Name -notlike '_*' }).Count) } }
            SendText $ctx 200 'application/json' ('{' + ($parts -join ',') + '}')
        }
        elseif ($path -eq '/api/subsolve' -and $method -eq 'POST') {
            # solve the next street from a node of the current result (river is usually under a second)
            $body = (New-Object IO.StreamReader($ctx.Request.InputStream, [Text.Encoding]::UTF8)).ReadToEnd()
            $subCfg = Join-Path $dir 'sub_config.txt'; $subOut = Join-Path $dir 'sub_result.js'
            [IO.File]::WriteAllText($subCfg, $body + "`r`noutput = sub_result.js`r`n", $utf8)
            Remove-Item $subOut -ErrorAction SilentlyContinue
            $sp = Start-Process -FilePath $exe -ArgumentList 'sub_config.txt' -WorkingDirectory $dir -NoNewWindow -PassThru `
                -RedirectStandardOutput (Join-Path $dir 'sub.log') -RedirectStandardError (Join-Path $dir 'sub.err')
            $null = $sp.Handle
            if (-not $sp.WaitForExit(600000)) { $sp.Kill() }
            if ($sp.ExitCode -eq 0 -and (Test-Path $subOut)) { Send $ctx 200 'application/javascript; charset=utf-8' ([IO.File]::ReadAllBytes($subOut)) }
            else { SendText $ctx 500 'text/plain' ((ReadShared (Join-Path $dir 'sub.err')) + (ReadShared (Join-Path $dir 'sub.log'))) }
        }
        elseif ($path -eq '/api/stop' -and $method -eq 'POST') {
            if (Running) { $proc.Kill() }
            SendText $ctx 200 'text/plain' 'stopped'
        }
        else {
            $name = if ($path -eq '/') { 'viewer.html' } else { [Uri]::UnescapeDataString($path.TrimStart('/')) }
            $file = [IO.Path]::GetFullPath((Join-Path $dir $name))
            if ($file.StartsWith($dir, 'OrdinalIgnoreCase') -and (Test-Path $file -PathType Leaf)) {
                $t = $types[[IO.Path]::GetExtension($file).ToLower()]; if (-not $t) { $t = 'application/octet-stream' }
                $len = (Get-Item $file).Length
                if ($t -like 'application/javascript*' -and $len -gt 200MB) {
                    # a browser cannot load a result this large; tell the page instead of hanging on it
                    SendText $ctx 200 'application/javascript' ('window.RESULT=null;window.RESULT_TOO_BIG=' + [Math]::Round($len / 1MB) + ';')
                } else { Send $ctx 200 $t ([IO.File]::ReadAllBytes($file)) }
            } else { SendText $ctx 404 'text/plain' 'not found' }
        }
    } catch {
        try { SendText $ctx 500 'text/plain' $_.Exception.Message } catch {}
    }
}
