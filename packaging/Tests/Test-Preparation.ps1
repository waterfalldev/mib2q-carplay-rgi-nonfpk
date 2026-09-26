param(
    [Parameter(Mandatory=$true)][string]$PackageDirectory,
    [string]$TestRootParent = [IO.Path]::GetTempPath()
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. "$PSScriptRoot/../PackageTools.ps1"
$testRoot = Join-Path $TestRootParent ('carplay-preparation-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$script:checks = 0
function Pass($label) { $script:checks++; Write-Host "PASS: $label" }
function Refuses($label, [scriptblock]$action, $pattern) {
    $failure = $null
    try { & $action | Out-Null } catch { $failure = $_.Exception.Message }
    if (-not $failure -or $failure -notmatch $pattern) { throw "FAIL: $label; actual error: $failure" }
    Pass $label
}
function WriteJson($path, $value, [switch]$Compact) {
    $json = $value | ConvertTo-Json -Depth 100 -Compress:$Compact
    [IO.File]::WriteAllText($path, $json, (New-Object Text.UTF8Encoding($false)))
}
function FixtureJar($name, $entryName, [byte[]]$header) {
    $path = Join-Path $testRoot $name
    $archive = [IO.Compression.ZipFile]::Open($path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        if ($entryName) {
            $stream = $archive.CreateEntry($entryName).Open()
            try { $stream.Write($header, 0, $header.Length) } finally { $stream.Dispose() }
        }
    } finally { $archive.Dispose() }
    return $path
}

$toolingRoot = Split-Path -Parent $PSScriptRoot
$sharedCollector = Join-Path (Split-Path -Parent $toolingRoot) 'deploy/mib/collect-logs.sh'
$emittedCollector = Join-Path $PackageDirectory 'sdcard/mod/carplay-rgi/installer/collect-logs.sh'
if ((Get-Sha256 $emittedCollector) -ne (Get-Sha256 $sharedCollector)) { throw 'Packaged collector differs from its standalone source' }
Pass 'shared collector is packaged byte-for-byte'
$package = Join-Path $PackageDirectory 'sdcard/mod/carplay-rgi'
$jar = Join-Path $package 'payload/jars/carplay_hook.jar'
$required = @('com/luka/carplay/core/CarPlayApp.class','com/luka/carplay/core/ScreenModule.class','de/audi/tghu/fwhmi/DisplayManagerMIB2High.class')
$manifest = Get-Content -Raw -LiteralPath (Join-Path $package 'meta/manifest.json') | ConvertFrom-Json
if ((Assert-JavaJar -Path $jar -ExpectedMajor 48 -RequiredClasses $required) -ne $manifest.build.javaClassCount) { throw 'Class count mismatch' }
Pass 'built JAR contains required classes and every class is Java 1.4'
Refuses 'wrong class version refused' { Assert-JavaJar -Path $jar -ExpectedMajor 49 } 'class major 48; expected 49'
Refuses 'missing required class refused' { Assert-JavaJar -Path $jar -ExpectedMajor 48 -RequiredClasses 'Missing.class' } 'missing Missing.class'
$empty = FixtureJar 'empty.jar' '' @()
Refuses 'empty JAR refused' { Assert-JavaJar -Path $empty -ExpectedMajor 48 } 'contains no class files'
$invalid = FixtureJar 'invalid.jar' 'Bad.class' @(0,0,0,0,0,0,0,48)
Refuses 'invalid class signature refused' { Assert-JavaJar -Path $invalid -ExpectedMajor 48 } 'not a valid Java class'
$short = FixtureJar 'short.jar' 'Short.class' @(0xCA,0xFE)
Refuses 'truncated class header refused' { Assert-JavaJar -Path $short -ExpectedMajor 48 } 'not a valid Java class'
$lookalike = FixtureJar 'lookalike.jar' 'OtherRequired.class' @(0xCA,0xFE,0xBA,0xBE,0,0,0,48)
Refuses 'required class uses exact entry name, not substring' { Assert-JavaJar -Path $lookalike -ExpectedMajor 48 -RequiredClasses 'Required.class' } 'missing Required.class'

$smartPath = Join-Path $testRoot 'smartphone.json'
$dioPath = Join-Path $testRoot 'dio.json'
$smartSource = Join-Path $package 'payload/config/smartphone_integrator.json'
$dioSource = Join-Path $package 'payload/config/dio_manager.json'
$argsForConfig = @{
    StockSmartphone = Join-Path $package 'rollback/smartphone_integrator.stock.json'
    PatchedSmartphone = $smartPath
    UpstreamChild = Join-Path $package 'meta/upstream_carplay_child.json'
    StockDio = Join-Path $package 'rollback/dio_manager.stock.json'
    PatchedDio = $dioPath
    SentIds = @('0x5200','0x5203')
    ReceivedIds = @('0x5201','0x5202','0x5204')
}
Copy-Item -LiteralPath $smartSource -Destination $smartPath
Copy-Item -LiteralPath $dioSource -Destination $dioPath
Assert-ConfigPatchScope @argsForConfig
Pass 'prepared stock-based config changes accepted'
$smart = ConvertFrom-HashCommentJson -Path $smartSource -Description 'fixture'
WriteJson $smartPath $smart -Compact
Assert-ConfigPatchScope @argsForConfig
Pass 'equivalent JSON formatting accepted'
$smart.children.carplay.params = 'unexpected'
WriteJson $smartPath $smart
Refuses 'changed CarPlay child refused' { Assert-ConfigPatchScope @argsForConfig } 'does not exactly match'
$smart = ConvertFrom-HashCommentJson -Path $smartSource -Description 'fixture'
$smart.children.carplay.exec = 'CARPLAY_STARTUP.SH'
WriteJson $smartPath $smart
Refuses 'case-only launcher change refused on case-sensitive QNX' { Assert-ConfigPatchScope @argsForConfig } 'does not exactly match'
$smart = ConvertFrom-HashCommentJson -Path $smartSource -Description 'fixture'
$smart | Add-Member -NotePropertyName unrelatedChange -NotePropertyValue $true
WriteJson $smartPath $smart
Refuses 'smartphone change outside child refused' { Assert-ConfigPatchScope @argsForConfig } 'changed outside children.carplay'
Copy-Item -LiteralPath $smartSource -Destination $smartPath -Force
$smart = ConvertFrom-HashCommentJson -Path $smartSource -Description 'fixture'
$smart.children.carplay.envs += 'LD_PRELOAD=unexpected.so'
WriteJson $smartPath $smart
$originalChild = $argsForConfig.UpstreamChild
$argsForConfig.UpstreamChild = Join-Path $testRoot 'child.json'
WriteJson $argsForConfig.UpstreamChild $smart.children.carplay
Refuses 'preload refused even when the upstream object also matches' { Assert-ConfigPatchScope @argsForConfig } 'unexpectedly contains LD_PRELOAD'
$argsForConfig.UpstreamChild = $originalChild
Copy-Item -LiteralPath $smartSource -Destination $smartPath -Force
$dio = ConvertFrom-HashCommentJson -Path $dioSource -Description 'fixture'
$dio.iap2.MessagesSentByAccessory += '0x5200'
WriteJson $dioPath $dio
Refuses 'duplicate RGI ID refused' { Assert-ConfigPatchScope @argsForConfig } 'exactly one|entries; expected'
$dio = ConvertFrom-HashCommentJson -Path $dioSource -Description 'fixture'
$dio.iap2.MessagesReceivedFromDevice = @($dio.iap2.MessagesReceivedFromDevice | Where-Object { $_ -ne '0x5204' })
WriteJson $dioPath $dio
Refuses 'missing RGI ID refused' { Assert-ConfigPatchScope @argsForConfig } 'exactly one|entries; expected'
$dio = ConvertFrom-HashCommentJson -Path $dioSource -Description 'fixture'
$dio | Add-Member -NotePropertyName unrelatedChange -NotePropertyValue $true
WriteJson $dioPath $dio
Refuses 'DIO change outside message arrays refused' { Assert-ConfigPatchScope @argsForConfig } 'changed outside the two permitted'
$dio = ConvertFrom-HashCommentJson -Path $dioSource -Description 'fixture'
$dio.iap2.MessagesSentByAccessory = @($dio.iap2.MessagesSentByAccessory | Where-Object { $_ -ne '0x5200' })
$dio.iap2.MessagesReceivedFromDevice += '0x5200'
WriteJson $dioPath $dio
Refuses 'RGI ID in wrong direction refused despite correct total count' { Assert-ConfigPatchScope @argsForConfig } 'entries; expected'
Copy-Item -LiteralPath $dioSource -Destination $dioPath -Force
[IO.File]::AppendAllText($dioPath, "`n# duplicate literal: `"0x5200`"`n")
Refuses 'raw duplicate occurrence check retained alongside semantic checks' { Assert-ConfigPatchScope @argsForConfig } 'Expected exactly one 0x5200'

$publishRoot = Join-Path $testRoot 'publication'
New-Item -ItemType Directory -Path $publishRoot | Out-Null
$final = Join-Path $publishRoot 'completed'
$pending = Join-Path $publishRoot 'pending'
New-Item -ItemType Directory -Path $pending | Out-Null
[IO.File]::WriteAllText((Join-Path $pending 'new.txt'), 'new')
Publish-PreparedPackage -Root $publishRoot -Pending $pending -Final $final
if (-not (Test-Path -LiteralPath (Join-Path $final 'new.txt')) -or (Test-Path -LiteralPath $pending)) { throw 'First publication failed' }
Pass 'first publication moves validated pending package into place'

New-Item -ItemType Directory -Path $pending | Out-Null
[IO.File]::WriteAllText((Join-Path $pending 'replacement.txt'), 'replacement')
Refuses 'replacement without force refused' { Publish-PreparedPackage -Root $publishRoot -Pending $pending -Final $final } 'without -ForceRebuild'
Publish-PreparedPackage -Root $publishRoot -Pending $pending -Final $final -AllowReplace
if (-not (Test-Path -LiteralPath (Join-Path $final 'replacement.txt')) -or (Test-Path -LiteralPath (Join-Path $final 'new.txt'))) { throw 'Replacement publication failed' }
if (-not (Test-Path (Join-Path $publishRoot 'Archive/completed/new.txt'))) { throw 'Replacement failed to preserve the previous package in Archive' }
Pass 'replacement publishes new package and archives the previous copy'

New-Item -ItemType Directory -Path $pending | Out-Null
[IO.File]::WriteAllText((Join-Path $pending 'failed.txt'), 'failed')
& {
    $script:moveCalls = 0
    function Move-Item {
        [CmdletBinding()]
        param([string]$LiteralPath, [string]$Destination)
        $script:moveCalls++
        if ($script:moveCalls -eq 2) { throw 'injected publish failure' }
        Microsoft.PowerShell.Management\Move-Item -LiteralPath $LiteralPath -Destination $Destination -ErrorAction Stop
    }
    Refuses 'failed publication is reported' { Publish-PreparedPackage -Root $publishRoot -Pending $pending -Final $final -AllowReplace } 'injected publish failure'
}
if (-not (Test-Path -LiteralPath (Join-Path $final 'replacement.txt')) -or -not (Test-Path -LiteralPath (Join-Path $pending 'failed.txt'))) { throw 'Failed replacement did not retain the old and pending packages' }
if (@(Get-ChildItem -LiteralPath $publishRoot -Directory).Count -ne 3) { throw 'Failed replacement left an unexpected package directory' }
Pass 'failed replacement restores the last completed package'

# Archive all prior completed versions, preserve name collisions and unrelated
# directories, and undo every archive move if final publication fails.
$versionsRoot = Join-Path $testRoot 'versions-publication'
foreach ($name in @('mib2q-carplay-rgi_v1','mib2q-carplay-rgi_v2','Archive/mib2q-carplay-rgi_v1','notes','.pending-v3')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $versionsRoot $name) | Out-Null
    [IO.File]::WriteAllText((Join-Path $versionsRoot "$name/identity.txt"), $name)
}
foreach ($version in @('v1','v2')) {
    [IO.File]::WriteAllText((Join-Path $versionsRoot "mib2q-carplay-rgi_$version/PACKAGE-VALIDATION.json"), '{}')
}
$versionsPending = Join-Path $versionsRoot '.pending-v3'
$versionsFinal = Join-Path $versionsRoot 'mib2q-carplay-rgi_v3'
& {
    $script:moveCalls = 0
    function Move-Item {
        [CmdletBinding()]
        param([string]$LiteralPath, [string]$Destination)
        $script:moveCalls++
        if ($script:moveCalls -eq 3) { throw 'injected final publication failure' }
        Microsoft.PowerShell.Management\Move-Item -LiteralPath $LiteralPath -Destination $Destination -ErrorAction Stop
    }
    Refuses 'publication failure after multiple archives is reported' { Publish-PreparedPackage $versionsRoot $versionsPending $versionsFinal } 'injected final publication failure'
}
foreach ($version in @('v1','v2')) {
    if (-not (Test-Path (Join-Path $versionsRoot "mib2q-carplay-rgi_$version/identity.txt"))) { throw 'Old version was not restored after failed publication' }
}
Pass 'failed publication restores all previous versions'
Publish-PreparedPackage $versionsRoot $versionsPending $versionsFinal
if (@(Get-ChildItem $versionsRoot -Directory -Filter 'mib2q-carplay-rgi_*').Count -ne 1) { throw 'Older completed versions remain outside Archive' }
if (@(Get-ChildItem (Join-Path $versionsRoot 'Archive') -Directory).Count -ne 3) { throw 'Archive collision overwrote or lost a package' }
if ((Get-Content -Raw (Join-Path $versionsRoot 'Archive/mib2q-carplay-rgi_v1/identity.txt')) -ne 'Archive/mib2q-carplay-rgi_v1') { throw 'Existing archive changed' }
if (-not (Test-Path (Join-Path $versionsRoot 'notes/identity.txt'))) { throw 'Unrelated directory was moved' }
Pass 'new publication archives all older packages without overwriting archive collisions or moving unrelated folders'

$badRoot = Join-Path $testRoot 'bad-publication'
New-Item -ItemType Directory -Path $badRoot | Out-Null
$badPending = Join-Path $badRoot 'pending'
$badFinal = Join-Path $badRoot 'completed'
New-Item -ItemType Directory -Path $badPending | Out-Null
[IO.File]::WriteAllText($badFinal, 'not a package directory')
Refuses 'non-directory completed package refused' { Publish-PreparedPackage -Root $badRoot -Pending $badPending -Final $badFinal -AllowReplace } 'non-directory or link'
if (-not (Test-Path -LiteralPath $badPending -PathType Container)) { throw 'Refused publication damaged pending package' }

$digestRoot = Join-Path $testRoot 'digests'
$digestMeta = Join-Path $digestRoot 'meta'
$digestPayload = Join-Path $digestRoot 'payload'
New-Item -ItemType Directory -Path $digestMeta, $digestPayload | Out-Null
$digestSums = Join-Path $digestMeta 'SHA256SUMS.txt'
$sameNamedPayload = Join-Path $digestPayload 'SHA256SUMS.txt'
[IO.File]::WriteAllText((Join-Path $digestMeta 'manifest.json'), '{}')
[IO.File]::WriteAllText((Join-Path $digestPayload 'manifest.json'), 'payload manifest')
[IO.File]::WriteAllText($sameNamedPayload, 'payload sums')
$records = @(Get-PackageFileDigests -Root $digestRoot -Exclude @('meta/manifest.json','meta/SHA256SUMS.txt'))
if ($records.Count -ne 2 -or @($records | Where-Object { $_.path -like 'payload/*' }).Count -ne 2) { throw 'Relative-path digest exclusion dropped same-named payload files' }
Pass 'digest exclusions affect only exact meta paths'
$covered = @(Get-PackageFileDigests -Root $digestRoot -Exclude @('meta/SHA256SUMS.txt'))
[IO.File]::WriteAllText($digestSums, (($covered | ForEach-Object { $_.sha256 + '  ' + $_.path }) -join "`n") + "`n")
Assert-Sha256SumsFile -Root $digestRoot -SumsPath $digestSums
Pass 'SHA256SUMS validates same-named payload files'
# ACTION is the on-card install/rollback choice: present or changed, it never breaks the sums.
[IO.File]::WriteAllText((Join-Path $digestRoot 'ACTION'), "install`n")
Assert-Sha256SumsFile -Root $digestRoot -SumsPath $digestSums
[IO.File]::WriteAllText((Join-Path $digestRoot 'ACTION'), "rollback`n")
Assert-Sha256SumsFile -Root $digestRoot -SumsPath $digestSums
Pass 'ACTION can be changed on the card without breaking SHA256SUMS'
[IO.File]::AppendAllText($sameNamedPayload, 'tampered')
Refuses 'same-named payload tampering refused' { Assert-Sha256SumsFile -Root $digestRoot -SumsPath $digestSums } 'mismatch'

# Java resource completeness: the gate the September package never had.
$resources = @($manifest.build.javaResources | ForEach-Object {
    [pscustomobject]@{ path = $_.jarEntry; sha256 = $_.sha256 }
})
if (@($resources | ForEach-Object { $_.path }) -cnotcontains 'com/luka/carplay/rgd/vc-text.bin') {
    throw 'Manifest does not record com/luka/carplay/rgd/vc-text.bin as a JAR resource'
}
Assert-JavaResources -Path $jar -Resources $resources
Pass 'built JAR carries every recorded resource with its SHA-256'
$resourceTree = Join-Path $testRoot 'java_resources'
$vcText = Join-Path $resourceTree 'com/luka/carplay/rgd/vc-text.bin'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $vcText) | Out-Null
[IO.File]::WriteAllBytes($vcText, [byte[]](1,2,3))
$inventory = Get-JavaResourceInventory -ResourceRoot $resourceTree
if ($inventory.Count -ne 1 -or $inventory[0].path -cne 'com/luka/carplay/rgd/vc-text.bin') { throw 'Resource inventory uses wrong JAR entry names' }
Pass 'resource inventory maps files to exact JAR entry names'
if ((Get-JavaResourceInventory -ResourceRoot (Join-Path $testRoot 'no-such-resources')).Count -ne 0) { throw 'Missing resource tree should inventory as empty' }
Pass 'absent resource tree inventories as empty'
$classOnly = FixtureJar 'class-only.jar' 'com/luka/carplay/rgd/VCTextData.class' @(0xCA,0xFE,0xBA,0xBE,0,0,0,48)
Refuses 'JAR without vc-text.bin refused' { Assert-JavaResources -Path $classOnly -Resources $inventory } 'missing resource com/luka/carplay/rgd/vc-text.bin'
$wrongBytes = FixtureJar 'wrong-resource.jar' 'com/luka/carplay/rgd/vc-text.bin' ([byte[]](9,9,9))
Refuses 'JAR with a different vc-text.bin refused' { Assert-JavaResources -Path $wrongBytes -Resources $inventory } 'differs from the pinned source'
if ((Get-Sha256 (Join-Path $PackageDirectory 'sdcard/mod/custom.sh')) -ne
    (Get-Sha256 (Join-Path $package 'installer/command.sh'))) { throw 'M.I.B. custom.sh differs from its covered dispatcher' }
Pass 'M.I.B. custom.sh matches its checksum-covered dispatcher'
$entryRoot = Join-Path $testRoot 'entry'
New-Item -ItemType Directory -Path $entryRoot | Out-Null
$entryText = [IO.File]::ReadAllText((Join-Path $PackageDirectory 'sdcard/mod/command.sh'))
$forward = Join-Path $entryRoot 'custom.sh'
Write-LfUtf8 -Path $forward -Content "return 7`n"
$forwardPosix = $forward.Replace('\','/')
if ($forwardPosix -match '^([A-Za-z]):(.*)$') { $forwardPosix = '/' + $Matches[1].ToLowerInvariant() + $Matches[2] }
$entry = Join-Path $entryRoot 'command.sh'
Write-LfUtf8 -Path $entry -Content ($entryText.Replace('/net/mmx/fs/sda0/mod/custom.sh',$forwardPosix))
$shell = Find-GitSh (Get-Command git -ErrorAction Stop).Source
$out = & $shell -c '. "$1"; result=$?; echo "caller-alive:$result"' sh ($entry.Replace('\','/'))
if ($LASTEXITCODE -ne 0 -or $out -notcontains 'caller-alive:7') { throw 'M.I.B. compatibility entry did not preserve the sourced caller and status' }
Pass 'M.I.B. command entry forwards a failure without exiting its caller'
Write-Host "All $script:checks preparation assertions passed. Fixtures: $testRoot"
