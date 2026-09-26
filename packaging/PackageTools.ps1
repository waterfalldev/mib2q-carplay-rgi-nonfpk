# Shared build, configuration, integrity and publication helpers.
# Dot-sourcing defines functions only; it never starts a build.
$script:ExeSuffix = if ([IO.Path]::DirectorySeparatorChar -eq '\') { '.exe' } else { '' }
$Utf8NoBom = New-Object Text.UTF8Encoding($false)
function Write-Step {
    param([string]$Message)

    Write-Host ''
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Assert-FileExists {
    param(
        [string]$Path,
        [string]$Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Description`: $Path"
    }
}

function Invoke-Native {
    param(
        [Parameter(Mandatory=$true)]
        [string]$Exe,

        [string[]]$Arguments = @(),

        [switch]$Capture
    )

    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'

    # Cleared first: if the process never launches, $LASTEXITCODE keeps the
    # previous command's value, which would otherwise read as success.
    $global:LASTEXITCODE = $null

    try {
        if ($Capture) {
            $output = & $Exe @Arguments 2>&1
        }
        else {
            & $Exe @Arguments
        }

        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldEap
    }

    if ($null -eq $code) {
        throw "Command did not run: $Exe"
    }

    if ($code -ne 0) {
        $displayArgs = $Arguments -join ' '

        if ($Capture) {
            $text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
            throw "Command failed ($code): $Exe $displayArgs`n$text"
        }

        throw "Command failed ($code): $Exe $displayArgs"
    }

    if ($Capture) {
        return @(
            $output |
                ForEach-Object { $_.ToString() }
        )
    }
}

function Get-Sha256 {
    param([string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Assert-SafeChildPath {
    # Recursive cleanup is permitted only for a concrete descendant of the
    # configured root. This prevents an empty/malformed path from widening a
    # cleanup to the output root or another unrelated directory.
    param(
        [string]$Root,
        [string]$Candidate,
        [string]$Description
    )

    $rootFull = [System.IO.Path]::GetFullPath($Root).
        TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    $candidateFull = [System.IO.Path]::GetFullPath($Candidate).
        TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    $prefix = $rootFull + [System.IO.Path]::DirectorySeparatorChar

    if (-not $candidateFull.StartsWith(
        $prefix,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "$Description is not safely contained by its root.`n`n    Root:      $rootFull`n    Candidate: $candidateFull"
    }
}

function Publish-PreparedPackage {
    param(
        [string]$Root,
        [string]$Pending,
        [string]$Final,
        [switch]$AllowReplace
    )

    Assert-SafeChildPath -Root $Root -Candidate $Pending -Description 'pending package'
    Assert-SafeChildPath -Root $Root -Candidate $Final -Description 'completed package'
    if (-not (Test-Path -LiteralPath $Pending -PathType Container)) {
        throw "Validated pending package directory is missing: $Pending"
    }

    # Plan every move before touching a completed package. Only completed
    # package folders are archived; partial builds and unrelated folders stay.
    $archive = Join-Path $Root 'Archive'
    Assert-SafeChildPath -Root $Root -Candidate $archive -Description 'package archive'
    if (Test-Path -LiteralPath $archive) {
        $archiveItem = Get-Item -LiteralPath $archive -Force
        if (-not $archiveItem.PSIsContainer -or ($archiveItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to use a non-directory or link as the package archive: $archive"
        }
    }
    $older = @(Get-ChildItem -LiteralPath $Root -Directory | Where-Object {
        $_.Name -like 'mib2q-carplay-rgi_*' -and
        $_.FullName -ne [IO.Path]::GetFullPath($Final) -and
        (Test-Path -LiteralPath (Join-Path $_.FullName 'PACKAGE-VALIDATION.json') -PathType Leaf)
    })
    if (Test-Path -LiteralPath $Final) {
        if (-not $AllowReplace) {
            throw "Completed package appeared during preparation; refusing to replace without -ForceRebuild: $Final"
        }
        $existing = Get-Item -LiteralPath $Final -Force
        if (-not $existing.PSIsContainer -or
            ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to replace a non-directory or link at $Final"
        }
        $older += $existing
    }
    $moves = @($older | ForEach-Object {
        if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to archive a linked package: $($_.FullName)"
        }
        $destination = Join-Path $archive $_.Name
        if (Test-Path -LiteralPath $destination) {
            $destination += '.archived-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N')
        }
        Assert-SafeChildPath -Root $Root -Candidate $_.FullName -Description 'old package source'
        Assert-SafeChildPath -Root $archive -Candidate $destination -Description 'archived package destination'
        [pscustomobject]@{ Source = $_.FullName; Destination = $destination }
    })
    if ($moves.Count -gt 0 -and -not (Test-Path -LiteralPath $archive)) {
        New-Item -ItemType Directory -Path $archive -ErrorAction Stop | Out-Null
    }

    $moved = New-Object System.Collections.ArrayList
    try {
        foreach ($move in $moves) {
            Move-Item -LiteralPath $move.Source -Destination $move.Destination -ErrorAction Stop
            [void]$moved.Add($move)
        }
        Move-Item -LiteralPath $Pending -Destination $Final -ErrorAction Stop
    }
    catch {
        for ($i = $moved.Count - 1; $i -ge 0; $i--) {
            $move = $moved[$i]
            if (-not (Test-Path -LiteralPath $move.Source)) {
                try {
                    Move-Item -LiteralPath $move.Destination -Destination $move.Source -ErrorAction Stop
                }
                catch {
                    Write-Warning "Could not restore $($move.Source); preserved at $($move.Destination) : $_"
                }
            }
        }
        throw
    }

    foreach ($move in $moves) {
        Write-Host "Archived previous package: $($move.Destination)"
    }
}

function ConvertFrom-HashCommentJson {
    # The Audi config files use full-line # comments around otherwise-valid
    # JSON. Keep the original bytes for patching, but strip only those comment
    # lines for semantic validation.
    param(
        [string]$Path,
        [string]$Description
    )

    try {
        $json = (
            [System.IO.File]::ReadAllLines($Path) |
                Where-Object { -not $_.TrimStart().StartsWith('#') }
        ) -join "`n"

        return $json | ConvertFrom-Json
    }
    catch {
        throw "$Description is not valid Audi hash-comment JSON: $Path`n$($_.Exception.Message)"
    }
}

function ConvertTo-ComparableJson {
    param($Object)

    return $Object | ConvertTo-Json -Depth 100 -Compress
}

function Assert-LfOnly {
    param(
        [string]$Path,
        [string]$Description
    )

    $bytes = [System.IO.File]::ReadAllBytes($Path)

    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 13) {
            throw "$Description contains CR bytes; QNX shell text must use LF only: $Path"
        }
    }
}

function Assert-JavaJar {
    param(
        [string]$Path,
        [int]$ExpectedMajor,
        [string[]]$RequiredClasses = @()
    )

    Add-Type -AssemblyName System.IO.Compression | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

    $archive = [System.IO.Compression.ZipFile]::OpenRead($Path)
    $classCount = 0

    try {
        # Inspect required entries and every class header in the same archive.
        foreach ($requiredClass in $RequiredClasses) {
            if (@($archive.Entries.FullName) -cnotcontains $requiredClass) {
                throw "Generated Java JAR is missing $requiredClass"
            }
        }

        foreach ($entry in $archive.Entries) {
            if (-not $entry.FullName.EndsWith('.class')) {
                continue
            }

            $classCount++
            $stream = $entry.Open()

            try {
                $header = New-Object byte[] 8
                $read = $stream.Read($header, 0, $header.Length)
            }
            finally {
                $stream.Dispose()
            }

            if (
                $read -ne 8 -or
                $header[0] -ne 0xCA -or
                $header[1] -ne 0xFE -or
                $header[2] -ne 0xBA -or
                $header[3] -ne 0xBE
            ) {
                throw "JAR entry is not a valid Java class: $($entry.FullName)"
            }

            $major = ([int]$header[6] -shl 8) + [int]$header[7]

            if ($major -ne $ExpectedMajor) {
                throw "JAR entry $($entry.FullName) has class major $major; expected $ExpectedMajor."
            }
        }
    }
    finally {
        $archive.Dispose()
    }

    if ($classCount -eq 0) {
        throw 'Generated Java JAR contains no class files.'
    }

    return $classCount
}

function Get-JavaResourceInventory {
    # Every non-class file the pinned source ships inside carplay_hook.jar
    # (upstream scripts/build_java.sh copies java_resources/ into the JAR),
    # keyed by its JAR entry name. Ordinal order, so the manifest is stable.
    param([string]$ResourceRoot)

    $inventory = @()

    if (-not (Test-Path -LiteralPath $ResourceRoot -PathType Container)) {
        return ,$inventory
    }

    $rootFull = [System.IO.Path]::GetFullPath($ResourceRoot).
        TrimEnd([System.IO.Path]::DirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar

    foreach ($file in @(Get-ChildItem -LiteralPath $ResourceRoot -Recurse -File -Force)) {
        $inventory += [pscustomobject][ordered]@{
            path = $file.FullName.Substring($rootFull.Length).Replace('\', '/')
            bytes = $file.Length
            sha256 = Get-Sha256 $file.FullName
            source = $file.FullName
        }
    }

    return ,@(Invoke-OrdinalSort -Items $inventory -Property 'path')
}

function Assert-JavaResources {
    # The September JAR compiled every class yet omitted vc-text.bin, so the
    # RGI listener died in VCTextData's initializer on the car. Class checks
    # cannot see that; this compares each resource entry by name and SHA-256.
    param(
        [string]$Path,
        [object[]]$Resources
    )

    Add-Type -AssemblyName System.IO.Compression | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

    $archive = [System.IO.Compression.ZipFile]::OpenRead($Path)

    try {
        foreach ($resource in $Resources) {
            $entry = $archive.GetEntry($resource.path)

            if ($null -eq $entry) {
                throw "Generated Java JAR is missing resource $($resource.path)"
            }

            $stream = $entry.Open()
            $sha = [System.Security.Cryptography.SHA256]::Create()

            try {
                $actual = ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToUpperInvariant()
            }
            finally {
                $sha.Dispose()
                $stream.Dispose()
            }

            if ($actual -ne $resource.sha256) {
                throw "JAR resource $($resource.path) differs from the pinned source.`n    Expected: $($resource.sha256)`n    Actual:   $actual"
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-Sha256SumsFile {
    param(
        [string]$Root,
        [string]$SumsPath
    )

    $expected = @{}

    foreach ($line in [System.IO.File]::ReadAllLines($SumsPath)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }

        if ($line -notmatch '^([0-9A-Fa-f]{64})  (.+)$') {
            throw "Malformed SHA256SUMS line: $line"
        }

        $relative = $Matches[2] -replace '\\', '/'

        if ($expected.ContainsKey($relative)) {
            throw "Duplicate SHA256SUMS path: $relative"
        }

        $expected[$relative] = $Matches[1].ToUpperInvariant()
    }

    Assert-SafeChildPath -Root $Root -Candidate $SumsPath -Description 'SHA256SUMS file'
    $sumsRelative = $SumsPath.Substring($Root.Length).TrimStart('\', '/') -replace '\\', '/'
    # ACTION is the on-card choice between install and rollback, so it is not checksummed.
    $actualRecords = Get-PackageFileDigests -Root $Root -Exclude @($sumsRelative, 'ACTION')

    if ($expected.Count -ne $actualRecords.Count) {
        throw "SHA256SUMS covers $($expected.Count) files, but staging contains $($actualRecords.Count) files excluding the sums file."
    }

    foreach ($record in $actualRecords) {
        if (-not $expected.ContainsKey($record.path)) {
            throw "SHA256SUMS is missing staged file: $($record.path)"
        }

        if ($record.sha256 -ne $expected[$record.path]) {
            throw "SHA256SUMS mismatch for staged file: $($record.path)"
        }
    }
}

function Invoke-OrdinalSort {
    # Sort-Object compares strings with the current culture, which orders
    # "META-INF/" before or after "com/" depending on the machine's locale.
    # Anything whose order ends up inside a hash has to be ordinal instead.
    param(
        [object[]]$Items,
        [string]$Property
    )

    if ($null -eq $Items -or $Items.Count -le 1) {
        return @($Items)
    }

    $sorted = [object[]]$Items.Clone()

    [Array]::Sort(
        $sorted,
        [System.Comparison[object]]{
            param($a, $b)
            [string]::CompareOrdinal([string]$a.$Property, [string]$b.$Property)
        })

    return $sorted
}

function Set-Guard {
    param(
        [string]$Name,
        [string]$Result = 'PASS'
    )

    if (-not $Guards.Contains($Name)) {
        throw "Unknown compatibility guard '$Name'. Add it to `$ExpectedGuardNames."
    }

    $Guards[$Name] = $Result

    Write-Host "$Name : $Result"
}

function Assert-Sha256 {
    param(
        [string]$Path,
        [string]$Expected,
        [string]$Description
    )

    $actual = Get-Sha256 $Path
    $wanted = $Expected.ToUpperInvariant()

    if ($actual -ne $wanted) {
        throw "SHA-256 mismatch for $Description.`n`n    Path:     $Path`n    Expected: $wanted`n    Actual:   $actual"
    }
}

function Get-PosixCksum {
    param([string]$Path)

    # POSIX 1003.2 cksum: CRC-32/CKSUM. Polynomial 0x04C11DB7, MSB first, no
    # reflection, the file length fed in after the data, final value inverted.
    $table = New-Object long[] 256

    for ($i = 0; $i -lt 256; $i++) {
        $value = [long]$i -shl 24

        for ($bit = 0; $bit -lt 8; $bit++) {
            if ($value -band 0x80000000L) {
                $value = (($value -shl 1) -band 0xFFFFFFFFL) -bxor 0x04C11DB7L
            }
            else {
                $value = ($value -shl 1) -band 0xFFFFFFFFL
            }
        }

        $table[$i] = $value
    }

    $crc = [long]0
    $length = [long]0
    $stream = [System.IO.File]::OpenRead($Path)

    try {
        $buffer = New-Object byte[] 65536

        while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $length += $read

            for ($i = 0; $i -lt $read; $i++) {
                $index = [int]((($crc -shr 24) -bxor $buffer[$i]) -band 0xFF)
                $crc = (($crc -shl 8) -band 0xFFFFFFFFL) -bxor $table[$index]
            }
        }
    }
    finally {
        $stream.Dispose()
    }

    $remaining = $length

    while ($remaining -gt 0) {
        $index = [int]((($crc -shr 24) -bxor ($remaining -band 0xFF)) -band 0xFF)
        $crc = (($crc -shl 8) -band 0xFFFFFFFFL) -bxor $table[$index]
        $remaining = $remaining -shr 8
    }

    return (-bnot $crc) -band 0xFFFFFFFFL
}

function Assert-StockBaseline {
    param(
        [string]$Path,
        [string]$Description,
        [long]$ExpectedSize,
        [long]$ExpectedCksum,
        [string]$ExpectedSha256
    )

    $actualSize = (Get-Item -LiteralPath $Path).Length

    if ($actualSize -ne $ExpectedSize) {
        throw "$Description is not the verified stock baseline.`n`n    Path:     $Path`n    Expected: $ExpectedSize bytes`n    Found:    $actualSize bytes"
    }

    # cksum is CRC32 and forgeable, so SHA-256 is what actually pins the file.
    Assert-Sha256 `
        -Path $Path `
        -Expected $ExpectedSha256 `
        -Description $Description

    $actualCksum = Get-PosixCksum $Path

    if ($actualCksum -ne $ExpectedCksum) {
        throw "$Description has the expected contents but the wrong POSIX cksum.`n`n    Path:     $Path`n    Expected: $ExpectedCksum`n    Found:    $actualCksum"
    }

    Write-Host "$Description : $actualSize bytes, cksum $actualCksum (PASS)"
}

function Import-FirmwareProfile {
    # Firmware\<name>\Firmware.psd1 holds every firmware-specific pin. Missing
    # and unknown keys are refused rather than defaulted, so a typo cannot
    # quietly drop a pin; files it names must stay inside the firmware folder.
    param(
        [string]$Directory,
        [string]$Name
    )

    $profilePath = Join-Path $Directory 'Firmware.psd1'
    if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
        throw "Firmware profile not found: $profilePath"
    }
    $data = Import-PowerShellDataFile -LiteralPath $profilePath

    $assertTable = {
        param($Table, [string]$Where, [string[]]$Keys)
        if ($Table -isnot [System.Collections.IDictionary]) {
            throw "Firmware profile $Where is not a table: $profilePath"
        }
        $actualKeys = @($Table.Keys | ForEach-Object { [string]$_ })
        foreach ($key in $Keys) {
            if ($actualKeys -cnotcontains $key) { throw "Firmware profile $Where is missing $key`: $profilePath" }
        }
        foreach ($key in $actualKeys) {
            if ($Keys -cnotcontains $key) { throw "Firmware profile $Where has unknown key $key`: $profilePath" }
        }
    }
    $assertText = {
        param($Value, [string]$Where, [string]$Pattern)
        if ($Value -isnot [string] -or $Value -cnotmatch $Pattern) {
            throw "Firmware profile $Where is not valid ($Pattern): $Value"
        }
    }
    $assertNumber = {
        param($Value, [string]$Where)
        if (-not ($Value -is [int] -or $Value -is [long]) -or $Value -lt 1 -or $Value -gt 0xFFFFFFFFL) {
            throw "Firmware profile $Where is not a whole number from 1 to 4294967295: $Value"
        }
    }
    $sha256Pattern = '^[0-9A-F]{64}$'
    $namePattern = '^[A-Za-z0-9_]+$'

    & $assertTable $data 'root' @(
        'SchemaVersion', 'Name', 'FirmwareTrain', 'MmxImage', 'LsdJxeSha256', 'LsdJarSha256',
        'StockSmartphone', 'StockDio')
    if (-not ($data.SchemaVersion -is [int]) -or $data.SchemaVersion -ne 2) {
        throw "Firmware profile SchemaVersion must be 2: $profilePath"
    }
    if ($data.Name -isnot [string] -or $data.Name -cne $Name) {
        throw "Firmware profile $profilePath names firmware '$($data.Name)', not '$Name'."
    }
    & $assertText $data.FirmwareTrain 'FirmwareTrain' $namePattern
    & $assertText $data.MmxImage 'MmxImage' $namePattern
    & $assertText $data.LsdJxeSha256 'LsdJxeSha256' $sha256Pattern
    & $assertText $data.LsdJarSha256 'LsdJarSha256' $sha256Pattern
    foreach ($stockKey in @('StockSmartphone', 'StockDio')) {
        $stock = $data[$stockKey]
        & $assertTable $stock $stockKey @('Bytes', 'Cksum', 'Sha256')
        & $assertNumber $stock.Bytes "$stockKey.Bytes"
        & $assertNumber $stock.Cksum "$stockKey.Cksum"
        & $assertText $stock.Sha256 "$stockKey.Sha256" $sha256Pattern
    }
    return [pscustomobject]@{ Path = $profilePath; Data = $data }
}

function Write-LfUtf8 {
    param(
        [string]$Path,
        [string]$Content
    )

    $normalized = $Content -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($Path, $normalized, $Utf8NoBom)
}

function Expand-TokenTemplate {
    param(
        [string]$Template,
        [System.Collections.IDictionary]$Values
    )

    $expanded = $Template

    foreach ($key in $Values.Keys) {
        $token = '@@' + $key + '@@'

        if (-not $expanded.Contains($token)) {
            throw "Installer template does not contain expected token $token"
        }

        $expanded = $expanded.Replace($token, [string]$Values[$key])
    }

    if ($expanded -match '@@[A-Z0-9_]+@@') {
        throw "Installer template contains an unresolved token: $($Matches[0])"
    }

    return $expanded
}

function Find-GitSh {
    param([string]$GitExe)
    if (-not $script:ExeSuffix) { return (Get-Command sh -ErrorAction Stop).Source }

    $gitRoot = Split-Path -Parent (Split-Path -Parent $GitExe)
    $candidates = @(
        (Join-Path $gitRoot 'bin\sh.exe')
        (Join-Path $gitRoot 'usr\bin\sh.exe')
    )

    $pathSh = Get-Command sh.exe -ErrorAction SilentlyContinue

    if ($pathSh) {
        $candidates += $pathSh.Source
    }

    foreach ($candidate in $candidates | Select-Object -Unique) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw 'Git-compatible sh.exe was not found for installer syntax checks.'
}

function Assert-QnxInstallerScript {
    param(
        [string]$Path,
        [string]$GitSh
    )

    Assert-LfOnly -Path $Path -Description (Split-Path -Leaf $Path)

    $text = [System.IO.File]::ReadAllText($Path)

    if (-not $text.StartsWith("#!/bin/sh`n")) {
        throw "Installer script has no /bin/sh shebang: $Path"
    }

    $bannedPatterns = @(
        '(?m)^\s*#!\s*/bin/bash'
        '(?m)(^|\s)(pgrep|pkill|readlink|timeout|sha256sum)(\s|$)'
        '(?m)\bsleep\s+[0-9]+\.[0-9]+'
        '(?m)\[\['
        '(?m)\b(chmod|chown)\s+--'
        '(?m)\b(mv|cp|rm|mkdir|grep)\s+--'

        # A failed redirection on a POSIX special built-in ends a
        # non-interactive shell immediately, skipping any || handler on the same
        # line. Truncating with ": >" therefore turns a read-only filesystem
        # into a silent exit with no logged reason; use an external command.
        '(?m)^\s*:\s*>'
    )

    foreach ($pattern in $bannedPatterns) {
        if ($text -match $pattern) {
            throw "Installer uses a command or syntax outside the validated QNX profile: $($Matches[0])"
        }
    }

    $nonCommentText = (($text -split "`n") |
        Where-Object { -not $_.TrimStart().StartsWith('#') }) -join "`n"

    if ($nonCommentText -match '(?m)(^|[;&|])\s*(?:/[A-Za-z0-9_.-]+/)?(reboot|shutdown)(\s|[;&|]|$)') {
        throw "Installer script contains an automatic reboot/shutdown command: $Path"
    }

    Invoke-Native `
        -Exe $GitSh `
        -Arguments @('-n', $Path.Replace('\', '/'))
}

function Save-WebFile {
    param(
        [string]$Uri,
        [string]$OutFile,

        [Parameter(Mandatory=$true)]
        [ValidatePattern('^[0-9A-Fa-f]{64}$')]
        [string]$ExpectedSha256
    )

    $parent = Split-Path -Parent $OutFile
    if ($parent) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }

    Write-Host "Downloading $Uri"

    $request = @{
        Uri = $Uri
        OutFile = $OutFile
        Headers = @{ 'User-Agent' = 'CarPlay-RGI-Prep' }
    }

    if ($PSVersionTable.PSVersion.Major -lt 6) {
        $request.UseBasicParsing = $true
    }

    Invoke-WebRequest @request

    Assert-Sha256 `
        -Path $OutFile `
        -Expected $ExpectedSha256 `
        -Description "download $(Split-Path -Leaf $OutFile)"
}

function Set-DeterministicJar {
    # jar cf stamps every entry with the current time, so the same inputs
    # produce a different SHA-256 on every run and the recorded build hash
    # cannot identify an artifact. Rewrite the archive with a fixed timestamp
    # and a stable entry order so the hash depends only on the class bytes.
    param([string]$Path)

    Add-Type -AssemblyName System.IO.Compression | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null

    # Reproducible for a given machine and .NET version: the entry set, order
    # and timestamps are fixed here, but the deflate stream itself is whatever
    # System.IO.Compression produces, which is not guaranteed identical across
    # .NET versions.
    #
    # The zip epoch: the earliest timestamp the format can represent.
    $fixedTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
    $rewritten = $Path + '.deterministic'

    if (Test-Path -LiteralPath $rewritten) {
        Remove-Item -LiteralPath $rewritten -Force
    }

    $source = [System.IO.Compression.ZipFile]::OpenRead($Path)

    try {
        $target = [System.IO.Compression.ZipFile]::Open(
            $rewritten,
            [System.IO.Compression.ZipArchiveMode]::Create)

        try {
            $ordered = Invoke-OrdinalSort -Items @($source.Entries) -Property 'FullName'

            foreach ($entry in $ordered) {
                $copy = $target.CreateEntry(
                    $entry.FullName,
                    [System.IO.Compression.CompressionLevel]::Optimal)

                $copy.LastWriteTime = $fixedTime

                $reader = $entry.Open()

                try {
                    $writer = $copy.Open()

                    try {
                        $reader.CopyTo($writer)
                    }
                    finally {
                        $writer.Dispose()
                    }
                }
                finally {
                    $reader.Dispose()
                }
            }
        }
        finally {
            $target.Dispose()
        }
    }
    finally {
        $source.Dispose()
    }

    Move-Item -LiteralPath $rewritten -Destination $Path -Force
}

function Get-PackageFileDigests {
    # One record per staged file: the published relative path, its size and its
    # SHA-256. Both the manifest and SHA256SUMS.txt are built from these, so
    # nothing is hashed twice and the path spelling is defined in one place.
    param(
        [string]$Root,
        [string[]]$Exclude = @()
    )

    $records = @()

    $files = Invoke-OrdinalSort `
        -Items @(Get-ChildItem -LiteralPath $Root -Recurse -File) `
        -Property 'FullName'

    foreach ($file in $files) {
        $relative = $file.FullName.Substring($Root.Length).
            TrimStart('\', '/') -replace '\\', '/'
        if ($Exclude -contains $relative) { continue }
        $records += [ordered]@{
            path = $relative
            bytes = $file.Length
            sha256 = Get-Sha256 $file.FullName
        }
    }

    return $records
}

function Assert-ElfArm32 {
    param(
        [string]$Path,
        [string]$Name
    )

    $stream = [System.IO.File]::OpenRead($Path)

    try {
        $header = New-Object byte[] 20
        $read = $stream.Read($header, 0, $header.Length)
    }
    finally {
        $stream.Dispose()
    }

    if ($read -lt 20) {
        throw "$Name is too small to be a valid ELF file."
    }

    if (
        $header[0] -ne 0x7F -or
        $header[1] -ne 0x45 -or
        $header[2] -ne 0x4C -or
        $header[3] -ne 0x46
    ) {
        throw "$Name does not have an ELF header."
    }

    # EI_CLASS == 1 = 32-bit ELF
    if ($header[4] -ne 1) {
        throw "$Name is not a 32-bit ELF binary."
    }

    # EI_DATA == 1 = little endian
    if ($header[5] -ne 1) {
        throw "$Name is not a little-endian ELF binary."
    }

    # e_type: 2 = ET_EXEC, 3 = ET_DYN
    $type = [int]$header[16] + ([int]$header[17] -shl 8)

    if ($type -ne 2 -and $type -ne 3) {
        throw "$Name is not an executable or shared object (e_type=$type)."
    }

    # e_machine == 40 = ARM
    $machine = [int]$header[18] + ([int]$header[19] -shl 8)

    if ($machine -ne 40) {
        throw "$Name is not an ARM ELF binary (e_machine=$machine)."
    }
}

# JDK 8 discovery

function Test-Jdk8Home {
    # Not $Home: that is a read-only, AllScope automatic variable, and binding a
    # parameter to it fails outright.
    param([string]$JdkHome)

    if ([string]::IsNullOrWhiteSpace($JdkHome)) {
        return $false
    }

    $javac = Join-Path $JdkHome ('bin/javac' + $script:ExeSuffix)
    $jar   = Join-Path $JdkHome ('bin/jar' + $script:ExeSuffix)
    $javap = Join-Path $JdkHome ('bin/javap' + $script:ExeSuffix)
    $release = Join-Path $JdkHome 'release'

    if (
        -not (Test-Path -LiteralPath $javac) -or
        -not (Test-Path -LiteralPath $jar) -or
        -not (Test-Path -LiteralPath $javap)
    ) {
        return $false
    }

    if (Test-Path -LiteralPath $release) {
        $releaseText = [System.IO.File]::ReadAllText($release)

        if ($releaseText -match 'JAVA_VERSION="1\.8') {
            return $true
        }
    }

    return $false
}

function Find-Jdk8 {
    param([string]$RequestedHome)

    $candidates = @()

    if (-not [string]::IsNullOrWhiteSpace($RequestedHome)) {
        $candidates += $RequestedHome
    }

    if (-not [string]::IsNullOrWhiteSpace($env:JAVA_HOME)) {
        $candidates += $env:JAVA_HOME
    }

    # The results are wrapped in @() before .FullName is read: under
    # Set-StrictMode 2.0, reading a property off an empty pipeline result
    # throws, so a machine with (say) C:\Program Files\Java holding only a JRE
    # would fail here instead of falling through to the next location.
    $searchRoots = @(
        [ordered]@{ Path = 'C:\Program Files\Eclipse Adoptium'; Pattern = '^jdk-?8' }
        [ordered]@{ Path = 'C:\Program Files\Java';             Pattern = '^jdk(1\.8|8)' }
    )

    foreach ($searchRoot in $searchRoots) {
        if (-not (Test-Path -LiteralPath $searchRoot.Path)) {
            continue
        }

        $found = @(
            Get-ChildItem `
                -LiteralPath $searchRoot.Path `
                -Directory `
                -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match $searchRoot.Pattern } |
                Sort-Object Name -Descending
        )

        $candidates += @($found | ForEach-Object { $_.FullName })
    }

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (Test-Jdk8Home $candidate) {
            return $candidate
        }
    }

    throw @"
JDK 8 was not found.

Install Temurin 8, for example:
    winget install EclipseAdoptium.Temurin.8.JDK

Then rerun this script, or pass:
    -JavaHome 'C:\path\to\jdk8'
"@
}

# Config patching

function Set-CarPlayChildBlock {
    param(
        [string]$StockPath,
        [string]$ChildPath,
        [string]$Destination
    )

    $lines = [System.IO.File]::ReadAllLines($StockPath)

    $startMatches = @()

    for ($i = 0; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match '^\s*"carplay"\s*:\s*\{') {
            $startMatches += $i
        }
    }

    if ($startMatches.Count -ne 1) {
        throw "Expected exactly one children.carplay block in $StockPath; found $($startMatches.Count)."
    }

    $start = $startMatches[0]
    $next = -1

    for ($i = $start + 1; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match '^\s*"carlife"\s*:\s*\{') {
            $next = $i
            break
        }
    }

    if ($next -lt 0) {
        throw "Could not find children.carlife after children.carplay in $StockPath."
    }

    $indent = ([regex]::Match($lines[$start], '^\s*')).Value
    $childLines = [System.IO.File]::ReadAllLines($ChildPath)

    $replacement = New-Object System.Collections.Generic.List[string]

    foreach ($line in $childLines) {
        if ($line.Length -eq 0) {
            $replacement.Add('')
        }
        else {
            $replacement.Add($indent + $line)
        }
    }

    if ($replacement.Count -eq 0) {
        throw "Upstream carplay_child.json is empty."
    }

    # Upstream ships carplay_child.json as a bare object, and the replaced line
    # carried the key, so it has to be put back or the member loses its name.
    # The key text is taken from the stock line rather than reinvented, so the
    # original spelling and spacing survive.
    $stockKey = ([regex]::Match($lines[$start], '^\s*"carplay"\s*:\s*')).Value

    $replacement[0] = $stockKey + $replacement[0].TrimStart()

    # children.carplay is followed by children.carlife, so it needs a comma.
    $replacement[$replacement.Count - 1] =
        $replacement[$replacement.Count - 1] + ','

    $output = New-Object System.Collections.Generic.List[string]

    for ($i = 0; $i -lt $start; $i++) {
        $output.Add($lines[$i])
    }

    foreach ($line in $replacement) {
        $output.Add($line)
    }

    for ($i = $next; $i -lt $lines.Length; $i++) {
        $output.Add($lines[$i])
    }

    Write-LfUtf8 `
        -Path $Destination `
        -Content (($output -join "`n") + "`n")
}

function Add-MessageIds {
    param(
        [string]$StockPath,
        [string]$Destination,
        [string[]]$SentIds,
        [string[]]$ReceivedIds
    )

    $lines = [System.IO.File]::ReadAllLines($StockPath)

    function Update-ArrayLine {
        param(
            [string[]]$InputLines,
            [string]$Key,
            [string[]]$Ids
        )

        # Not $matches: -match overwrites the automatic $Matches hashtable in
        # this scope, so the collection would stop being an array.
        $keyLines = @()

        for ($i = 0; $i -lt $InputLines.Length; $i++) {
            if ($InputLines[$i] -match ('"' + [regex]::Escape($Key) + '"\s*:')) {
                $keyLines += $i
            }
        }

        if ($keyLines.Count -ne 1) {
            throw "Expected exactly one $Key entry; found $($keyLines.Count)."
        }

        $index = $keyLines[0]
        $line = $InputLines[$index]

        foreach ($id in $Ids) {
            $quoted = '"' + $id + '"'

            if ($line.Contains($quoted)) {
                continue
            }

            $close = $line.LastIndexOf(']')

            if ($close -lt 0) {
                throw "Could not locate closing ] for $Key."
            }

            $before = $line.Substring(0, $close)
            $after = $line.Substring($close)

            if ($before.TrimEnd().EndsWith('[')) {
                $separator = ''
            }
            else {
                $separator = ', '
            }

            $line = $before + $separator + $quoted + $after
        }

        $InputLines[$index] = $line
        return $InputLines
    }

    $lines = Update-ArrayLine `
        -InputLines $lines `
        -Key 'MessagesSentByAccessory' `
        -Ids $SentIds

    $lines = Update-ArrayLine `
        -InputLines $lines `
        -Key 'MessagesReceivedFromDevice' `
        -Ids $ReceivedIds

    Write-LfUtf8 `
        -Path $Destination `
        -Content (($lines -join "`n") + "`n")
}

function Assert-StringSequence {
    param(
        [object[]]$Actual,
        [string[]]$Expected,
        [string]$Description
    )

    $actualStrings = @($Actual | ForEach-Object { [string]$_ })

    if ($actualStrings.Count -ne $Expected.Count) {
        throw "$Description has $($actualStrings.Count) entries; expected $($Expected.Count)."
    }

    for ($i = 0; $i -lt $Expected.Count; $i++) {
        if ($actualStrings[$i] -cne $Expected[$i]) {
            throw "$Description differs at index $i. Expected $($Expected[$i]); found $($actualStrings[$i])."
        }
    }
}

function Assert-ConfigPatchScope {
    param(
        [string]$StockSmartphone,
        [string]$PatchedSmartphone,
        [string]$UpstreamChild,
        [string]$StockDio,
        [string]$PatchedDio,
        [string[]]$SentIds,
        [string[]]$ReceivedIds
    )

    $stockSmartObject = ConvertFrom-HashCommentJson `
        -Path $StockSmartphone `
        -Description 'stock smartphone_integrator.json'
    $patchedSmartObject = ConvertFrom-HashCommentJson `
        -Path $PatchedSmartphone `
        -Description 'patched smartphone_integrator.json'

    try {
        $upstreamChildObject =
            [System.IO.File]::ReadAllText($UpstreamChild) |
            ConvertFrom-Json
    }
    catch {
        throw "Upstream carplay_child.json is invalid JSON: $($_.Exception.Message)"
    }

    $patchedCarplayJson = ConvertTo-ComparableJson $patchedSmartObject.children.carplay
    if ($patchedCarplayJson -match 'LD_PRELOAD') {
        throw 'Patched children.carplay unexpectedly contains LD_PRELOAD.'
    }

    if (
        $patchedCarplayJson -cne
        (ConvertTo-ComparableJson $upstreamChildObject)
    ) {
        throw 'Patched children.carplay does not exactly match the pinned upstream child object.'
    }

    # Make the one allowed object equal, then require the entire remaining
    # semantic document to match. This catches accidental edits outside the
    # children.carplay block while ignoring Audi's comments/formatting.
    $stockSmartObject.children.carplay = $patchedSmartObject.children.carplay

    if (
        (ConvertTo-ComparableJson $stockSmartObject) -cne
        (ConvertTo-ComparableJson $patchedSmartObject)
    ) {
        throw 'smartphone_integrator.json changed outside children.carplay.'
    }

    # Keep the raw occurrence guard as well as semantic array checks: a JSON
    # parser alone can hide duplicate keys. Do not depend on text indentation.
    $patchedDioText = [System.IO.File]::ReadAllText($PatchedDio)
    foreach ($id in (@($SentIds) + @($ReceivedIds))) {
        $count = [regex]::Matches($patchedDioText, [regex]::Escape('"' + $id + '"')).Count
        if ($count -ne 1) {
            throw "Expected exactly one $id in patched dio_manager.json; found $count."
        }
    }

    $stockDioObject = ConvertFrom-HashCommentJson `
        -Path $StockDio `
        -Description 'stock dio_manager.json'
    $patchedDioObject = ConvertFrom-HashCommentJson `
        -Path $PatchedDio `
        -Description 'patched dio_manager.json'

    $expectedSent =
        @($stockDioObject.iap2.MessagesSentByAccessory) + @($SentIds)
    $expectedReceived =
        @($stockDioObject.iap2.MessagesReceivedFromDevice) + @($ReceivedIds)

    Assert-StringSequence `
        -Actual @($patchedDioObject.iap2.MessagesSentByAccessory) `
        -Expected $expectedSent `
        -Description 'iap2.MessagesSentByAccessory'
    Assert-StringSequence `
        -Actual @($patchedDioObject.iap2.MessagesReceivedFromDevice) `
        -Expected $expectedReceived `
        -Description 'iap2.MessagesReceivedFromDevice'

    # As above, equalise the two allowed arrays and compare the rest of the
    # parsed document to prove no other setting changed.
    $stockDioObject.iap2.MessagesSentByAccessory =
        $patchedDioObject.iap2.MessagesSentByAccessory
    $stockDioObject.iap2.MessagesReceivedFromDevice =
        $patchedDioObject.iap2.MessagesReceivedFromDevice

    if (
        (ConvertTo-ComparableJson $stockDioObject) -cne
        (ConvertTo-ComparableJson $patchedDioObject)
    ) {
        throw 'dio_manager.json changed outside the two permitted iAP2 message arrays.'
    }
}
