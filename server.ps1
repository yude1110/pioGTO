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
    catch { $listener = New-Object System.Net.HttpListener }
}
if (-not $port) { Write-Host 'No free port in 8765-8785.'; Read-Host 'Press Enter to exit'; exit 1 }
$url = "http://localhost:$port/"
Write-Host "Solver UI running at $url  (close this window to stop)"
Start-Process $url

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
    $r.OutputStream.Write($bytes, 0, $bytes.Length); $r.OutputStream.Close()
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
            $json = '{"running":' + $run.ToString().ToLower() + ',"exit":' + $code + ',"log":' + (ConvertTo-Json $log) + '}'
            SendText $ctx 200 'application/json' $json
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
                Send $ctx 200 $t ([IO.File]::ReadAllBytes($file))
            } else { SendText $ctx 404 'text/plain' 'not found' }
        }
    } catch {
        try { SendText $ctx 500 'text/plain' $_.Exception.Message } catch {}
    }
}
