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
        Invoke-WebRequest -UseBasicParsing "$base/$version/$archive" -OutFile $zip
        $expected = ((Fetch-Text "$base/$version/$archive.sha256") -split '\s+')[0]
        $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash
        if ($expected.ToLower() -ne $actual.ToLower()) { Fail "checksum mismatch for $archive" }

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
