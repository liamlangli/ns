# Install Nano Script into %USERPROFILE%\.ns.
#
#   irm https://liamlangli.com/ns/install.ps1 | iex
#
# Downloads the prebuilt Windows x86_64 release published beside this script,
# verifies its SHA-256, and adds %USERPROFILE%\.ns\bin to the user PATH.
#
# Environment:
#   NS_VERSION         release to install (default: the one named by latest)
#   NS_RELEASE_URL     where releases live (default: https://liamlangli.com/ns)
#   NS_NO_MODIFY_PATH  set to 1 to leave the user PATH alone

# Everything runs inside one script block, so a truncated download never runs
# a partial script.
& {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'
    # Windows PowerShell 5.1 may still default to TLS 1.0.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

    function Say($text) { Write-Host 'ns: ' -ForegroundColor Green -NoNewline; Write-Host $text }
    function Fail($text) { Write-Host 'ns: ' -ForegroundColor Red -NoNewline; Write-Host $text; throw $text }
    # The server sends these small text files as application/octet-stream,
    # which Invoke-RestMethod returns as bytes; read them back from disk.
    function Fetch-Text($url) {
        $file = [IO.Path]::GetTempFileName()
        try {
            Invoke-WebRequest -UseBasicParsing $url -OutFile $file
            return (Get-Content -Raw $file).Trim()
        } finally {
            Remove-Item -Force $file -ErrorAction SilentlyContinue
        }
    }

    # Download with the same bar install.sh draws: percent, size, speed and
    # eta. Invoke-WebRequest's own progress slows PowerShell 5.1 to a crawl, so
    # the response is streamed here and the bar drawn by hand.
    function Download($url, $path) {
        Add-Type -AssemblyName System.Net.Http
        $client = New-Object System.Net.Http.HttpClient
        try {
            $response = $client.GetAsync($url, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).Result
            if (-not $response.IsSuccessStatusCode) { Fail "download failed: $url ($([int]$response.StatusCode))" }
            $total = $response.Content.Headers.ContentLength
            $source = $response.Content.ReadAsStreamAsync().Result
            $target = [IO.File]::Create($path)
            $draw = -not [Console]::IsErrorRedirected
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $buffer = New-Object byte[] 65536
            $got = [long]0
            $last = -1.0
            try {
                while (($read = $source.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $target.Write($buffer, 0, $read)
                    $got += $read
                    if ($draw -and $clock.Elapsed.TotalSeconds - $last -ge 0.2) {
                        Draw-Progress $got $total $clock.Elapsed.TotalSeconds $false
                        $last = $clock.Elapsed.TotalSeconds
                    }
                }
            } finally {
                $target.Dispose()
                $source.Dispose()
            }
            if ($draw) {
                Draw-Progress $got $total $clock.Elapsed.TotalSeconds $true
                [Console]::Error.WriteLine()
            }
        } finally {
            $client.Dispose()
        }
    }
    function Clock($seconds) {
        $s = [int][Math]::Round($seconds)
        if ($s -ge 60) { return '{0}m{1:00}s' -f [int][Math]::Floor($s / 60), ($s % 60) }
        return "${s}s"
    }
    function Draw-Progress($got, $total, $seconds, $done) {
        $mb = 1048576.0
        $rate = if ($seconds -gt 0) { $got / $seconds } else { 0 }
        $speed = if ($rate -gt 0) { '{0:0.0} MB/s' -f ($rate / $mb) } else { '-- MB/s' }
        $tail = if ($done) { 'in ' + (Clock $seconds) }
                elseif ($seconds -ge 1 -and $rate -gt 0 -and $total) { 'eta ' + (Clock (($total - $got) / $rate)) }
                else { 'eta --' }
        if ($total) {
            $part = [Math]::Min(1.0, $got / $total)
            $fill = [int][Math]::Floor($part * 24)
            $bar = ('#' * $fill) + ('-' * (24 - $fill))
            $line = '  [{0}] {1,3}%  {2:0.0}/{3:0.0} MB  {4}  {5}   ' -f $bar, [int]($part * 100), ($got / $mb), ($total / $mb), $speed, $tail
        } else {
            $line = '  {0:0.0} MB  {1}  {2}   ' -f ($got / $mb), $speed, $tail
        }
        [Console]::Error.Write("`r$line")
    }

    $base = if ($env:NS_RELEASE_URL) { $env:NS_RELEASE_URL.TrimEnd('/') } else { 'https://liamlangli.com/ns' }
    $root = Join-Path $HOME '.ns'
    $platform = 'windows-x86_64'

    $onWindows = $env:OS -eq 'Windows_NT'
    if ($onWindows) {
        $arch = $env:PROCESSOR_ARCHITEW6432
        if (-not $arch) { $arch = $env:PROCESSOR_ARCHITECTURE }
        if ($arch -eq 'ARM64') { Say 'ARM64 Windows runs the x86_64 build under emulation' }
        elseif ($arch -ne 'AMD64') { Fail "unsupported architecture $arch; ns needs 64-bit Windows" }
    }

    $version = $env:NS_VERSION
    if (-not $version) { $version = Fetch-Text "$base/latest" }
    if (-not $version) { Fail 'no release version published' }

    $archive = "ns-$platform.zip"
    $work = Join-Path ([IO.Path]::GetTempPath()) ("ns-install-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null
    try {
        Say "downloading $version for $platform"
        $zip = Join-Path $work $archive
        Download "$base/$version/$archive" $zip
        $expected = ((Fetch-Text "$base/$version/$archive.sha256") -split '\s+')[0]
        $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash
        Say 'verifying checksum'
        if ($expected.ToLower() -ne $actual.ToLower()) { Fail "checksum mismatch for $archive" }

        Say 'unpacking'
        $stage = Join-Path $work 'stage'
        # Expand-Archive differs between PowerShell 5.1 and 7; .NET does not.
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($zip, $stage)
        if (-not (Test-Path (Join-Path $stage 'bin/ns.exe'))) { Fail 'archive has no bin\ns.exe' }
        New-Item -ItemType Directory -Force -Path $root | Out-Null
        # Copy over the old install; anything else already in .ns is kept.
        Copy-Item -Path (Join-Path $stage '*') -Destination $root -Recurse -Force
    } finally {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }

    $bin = Join-Path $root 'bin'
    if ($onWindows) {
        & (Join-Path $bin 'ns.exe') --version | Out-Null
        if ($LASTEXITCODE -ne 0) { Fail 'installed ns.exe does not run' }
    }
    Say "installed $version into $root"

    if ($onWindows) {
        $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        $entries = if ($userPath) { $userPath -split ';' } else { @() }
        if ($entries -notcontains $bin) {
            if ($env:NS_NO_MODIFY_PATH -eq '1') {
                Say "add $bin to PATH to run ns"
            } else {
                $joined = (@($bin) + ($entries | Where-Object { $_ })) -join ';'
                [Environment]::SetEnvironmentVariable('Path', $joined, 'User')
                Say "added $bin to the user PATH; open a new terminal to use it"
            }
        }
        if (($env:Path -split ';') -notcontains $bin) { $env:Path = "$bin;$env:Path" }
    }
    Say 'get started: ns create hello; cd hello; ns run'
}
