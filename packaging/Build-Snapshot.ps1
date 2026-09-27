#requires -version 7.0
# Internal entry point. Build-Package.ps1 exports an exact commit before calling this.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$FirmwareRoot,
    [Parameter(Mandatory=$true)][string]$Firmware,
    [Parameter(Mandatory=$true)][string]$Dependencies,
    [Parameter(Mandatory=$true)][string]$ScratchRoot,
    [Parameter(Mandatory=$true)][string]$ReleaseDirectory,
    [Parameter(Mandatory=$true)][string]$Commit,
    [string]$JavaHome = '',
    [string]$JavaImage = 'eclipse-temurin:8-jdk-jammy',
    [string]$ExpectedToolchainImageId = '',
    [string]$HostTestImage = 'carplay-rgi-host-tests:local',
    [switch]$ArmRollback
)
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/PackageTools.ps1"
. "$PSScriptRoot/BuildInputs.ps1"
$WorkTree = Split-Path -Parent $PSScriptRoot
$Git = (Get-Command git -ErrorAction Stop).Source
$GitSh = Find-GitSh $Git
. "$PSScriptRoot/JavaDocker.ps1"
$JavaImageId = Resolve-JavaImage $JavaImage
$JavaWork = Join-Path $ScratchRoot 'java'
$DepsDirectory = $Dependencies
& "$PSScriptRoot/Tests/Test-Profile.ps1" -TestRootParent $ScratchRoot
. "$PSScriptRoot/Dependencies.ps1"
$ShortCommit = $Commit.Substring(0, [Math]::Min(12, $Commit.Length))
$Tag = 'source-' + $ShortCommit
$SafeTag = $Tag
$BuildId = $Tag
$ShellTemplates = @{}
$TemplateProvenance = @(foreach ($name in @('command.sh','common.sh','install.sh','rollback.sh','collect-logs.sh')) {
    $p = Join-Path $WorkTree ('deploy/mib/' + $name)
    Assert-LfOnly -Path $p -Description $name
    $ShellTemplates[$name] = [IO.File]::ReadAllText($p)
    [ordered]@{ path = 'deploy/mib/' + $name; sha256 = Get-Sha256 $p }
})
$ExpectedGuardNames = @(
    'stockConfigBaseline'
    'stockSysConst541KdkCreation'
    'upstreamSupervisorNoLdPreload'
    'upstreamRgiMessageIds'
    'nativeSourceBuild'
    'nativeArm32Elf'
    'mostOutputHostTests'
    'javaVcTextRuntimeLoad'
    'javaHostSuites'
    'javaStockLinkage'
    'qnxTextLf'
    'configPatchScope'
    'installerWritabilityPreflight'
    'installerHmiLibraryIdentity'
    'installerPreservesConfigModes'
    'installerRestoresReadOnlyMounts'
    'installerPayloadCksums'
    'installerQnxShellProfile'
    'installerNoAutomaticReboot'
    'installerEntryPoint'
    'installerMibSdExecution'
    'installerActionControl'
    'rollbackPackage'
)
$Guards = [ordered]@{}
foreach ($name in $ExpectedGuardNames) { $Guards[$name] = 'NOT-RUN' }
$FirmwareProfile = Import-FirmwareProfile -Directory $FirmwareRoot -Name $Firmware
$FirmwareProfileSha256 = Get-Sha256 $FirmwareProfile.Path
$FirmwareName = $FirmwareProfile.Data.Name
$FirmwareTrain = $FirmwareProfile.Data.FirmwareTrain
$FirmwareMmxImage = $FirmwareProfile.Data.MmxImage

$ExpectedLsdJxeSha256 = $FirmwareProfile.Data.LsdJxeSha256
# lsd.jar is the actual compile input, so it is pinned at least as strictly as
# the JXE it was extracted from.
$ExpectedLsdJarSha256 = $FirmwareProfile.Data.LsdJarSha256

$ExpectedStockSmartphoneSize = $FirmwareProfile.Data.StockSmartphone.Bytes
$ExpectedStockSmartphoneCksum = $FirmwareProfile.Data.StockSmartphone.Cksum
$ExpectedStockSmartphoneSha256 = $FirmwareProfile.Data.StockSmartphone.Sha256

$ExpectedStockDioSize = $FirmwareProfile.Data.StockDio.Bytes
$ExpectedStockDioCksum = $FirmwareProfile.Data.StockDio.Cksum
$ExpectedStockDioSha256 = $FirmwareProfile.Data.StockDio.Sha256

# Validate firmware inputs

Write-Step "Validating $FirmwareName firmware inputs"

$LsdJxe = Join-Path $FirmwareRoot 'lsd.jxe'
$LsdJar = Join-Path $FirmwareRoot 'lsd.jar'

Assert-FileExists $LsdJxe "$FirmwareName lsd.jxe"
Assert-FileExists $LsdJar "$FirmwareName lsd.jar"

$lsdJxeInfo = Get-Item -LiteralPath $LsdJxe
$lsdJarInfo = Get-Item -LiteralPath $LsdJar

# The installer refuses a unit whose /ifs/lsd.jxe differs from this pinned file, the
# library the JAR is compiled and link-checked against. The car has cksum, not SHA-256;
# one read of the 60 MB file gives both.
$lsdJxeIdentity = Get-PosixCksum $LsdJxe -Sha256

# The pinned hashes subsume any size comparison, so these are the only gate.
Assert-Sha256 `
    -Path $LsdJxe `
    -Expected $ExpectedLsdJxeSha256 `
    -Description "$FirmwareName lsd.jxe" `
    -Digest $lsdJxeIdentity.Sha256

Assert-Sha256 `
    -Path $LsdJar `
    -Expected $ExpectedLsdJarSha256 `
    -Description "$FirmwareName lsd.jar (Java compile input)"

$ExpectedLsdJxeCksum = $lsdJxeIdentity.Cksum
$ExpectedLsdJxeBytes = $lsdJxeInfo.Length
Write-Host "lsd.jxe identity for the installer: $ExpectedLsdJxeCksum`:$ExpectedLsdJxeBytes"

# Both files were read off this unit and sit beside the firmware images. There
# is nothing to search for: the contents are pinned by SHA-256 immediately
# below, so the only thing a lookup could add is the chance of silently
# preferring some other copy over the one the operator is looking at.
$StockSmartphone = Join-Path $FirmwareRoot 'smartphone_integrator.json'
$StockDio = Join-Path $FirmwareRoot 'dio_manager.json'

Assert-FileExists $StockSmartphone "$FirmwareName stock smartphone_integrator.json"
Assert-FileExists $StockDio "$FirmwareName stock dio_manager.json"

# Size alone would accept any same-length file, so the baseline recorded off
# the car is recomputed here rather than just carried into the manifest.
Assert-StockBaseline `
    -Path $StockSmartphone `
    -Description 'Stock smartphone_integrator.json' `
    -ExpectedSize $ExpectedStockSmartphoneSize `
    -ExpectedCksum $ExpectedStockSmartphoneCksum `
    -ExpectedSha256 $ExpectedStockSmartphoneSha256

Assert-StockBaseline `
    -Path $StockDio `
    -Description 'Stock dio_manager.json' `
    -ExpectedSize $ExpectedStockDioSize `
    -ExpectedCksum $ExpectedStockDioCksum `
    -ExpectedSha256 $ExpectedStockDioSha256

Set-Guard 'stockConfigBaseline'

$stockSmartText = [System.IO.File]::ReadAllText($StockSmartphone)
$stockDioText = [System.IO.File]::ReadAllText($StockDio)

if ($stockSmartText -notmatch '"exec"\s*:\s*"dio_manager"') {
    throw 'Stock smartphone_integrator.json does not contain the expected dio_manager CarPlay child.'
}

if ($stockSmartText -notmatch '"cleanupScript"\s*:\s*"/etc/scripts/carplay_cleanup\.sh"') {
    throw 'Stock smartphone_integrator.json does not contain the expected Audi CarPlay cleanup script.'
}

if ($stockDioText -notmatch ('"firmwareVersion"\s*:\s*"' + [regex]::Escape($FirmwareMmxImage) + '"')) {
    throw "Stock dio_manager.json does not identify the expected $FirmwareName MMX image family $FirmwareMmxImage."
}

foreach ($id in $RgiMessageIds) {
    if ($stockDioText.Contains('"' + $id + '"')) {
        throw "Stock dio_manager.json already contains RGI message ID $id. Refusing to treat it as stock."
    }
}

# Already verified against these constants above, so no need to hash 199 MB
# again just to print it.
Write-Host "lsd.jxe SHA256: $ExpectedLsdJxeSha256"
Write-Host "lsd.jar SHA256: $ExpectedLsdJarSha256"

# Validate stock DisplayManager bytecode

Write-Step "Rechecking $FirmwareName DisplayManager compatibility baseline"

$stockDmClass = 'de.audi.tghu.fwhmi.DisplayManagerMIB2High'

$stockBytecodeLines = Invoke-JavaDocker -SourceRoot $WorkTree -StockJar $LsdJar `
    -Dependencies $Dependencies -OutputRoot (Join-Path $ScratchRoot 'stock-inspection') `
    -Image $JavaImageId -Action @('javap', $stockDmClass) -Capture

# javap indents each method signature by two spaces and its body by more, so
# the disassembly splits cleanly into per-method blocks. The class reads
# SysConst 541 in three different methods; only configureDM is the one that
# creates the KDK backing displayables, and 101/102 are small enough to occur
# incidentally elsewhere, so the evidence is only counted inside that method.
$configureDmBody = New-Object System.Collections.Generic.List[string]
$inConfigureDm = $false

foreach ($line in $stockBytecodeLines) {
    if ($line -match '^\s{2}\S.*\(') {
        $inConfigureDm = $line -match '\bconfigureDM\s*\('
        continue
    }

    if ($inConfigureDm) {
        $configureDmBody.Add($line)
    }
}

if ($configureDmBody.Count -eq 0) {
    throw @"
$FirmwareName stock DisplayManager has no configureDM method in its disassembly.

Stop and review compatibility manually before preparing this package.
"@
}

$configureDmText = $configureDmBody -join "`n"

foreach ($pattern in @(
    'sipush\s+541',
    'iconst_2',
    'if_icmpne',
    'bipush\s+102',
    'bipush\s+101',
    'createImageDisplayable'
)) {
    if ($configureDmText -notmatch $pattern) {
        throw @"
$FirmwareName stock DisplayManager no longer matches the compatibility baseline.

Missing bytecode pattern in configureDM:
    $pattern

Stop and review compatibility manually before preparing this package.
"@
    }
}

Set-Guard 'stockSysConst541KdkCreation'

    # Upstream architecture guards

    Write-Step 'Checking upstream architecture assumptions'

    $DisplayManagerSource = Join-Path `
        $WorkTree `
        'java_patch\de\audi\tghu\fwhmi\DisplayManagerMIB2High.java'

    $CarPlayChildPath = Join-Path `
        $WorkTree `
        'deploy\smartphone_integrator\carplay_child.json'

    $StartupScript = Join-Path `
        $WorkTree `
        'deploy\smartphone_integrator\carplay_startup.sh'

    $CleanupScript = Join-Path `
        $WorkTree `
        'deploy\smartphone_integrator\carplay_cleanup.sh'

    $ProcessesScript = Join-Path `
        $WorkTree `
        'deploy\smartphone_integrator\carplay_processes.sh'

    $FlagAtlasSource = Join-Path `
        $WorkTree `
        'maneuver_render\resources\flag_atlas.rgba'

    $MonitorScript = Join-Path $WorkTree 'deploy/smartphone_integrator/carplay_monitor.sh'
    Assert-FileExists $MonitorScript 'runtime monitor'

    # The RGI message IDs are checked against the header that defines them,
    # not against prose: README.md did not mention them at MHI2Q-2026-08-30.
    $Iap2Protocol = Join-Path `
        $WorkTree `
        'hook\framework\iap2_protocol.h'

    foreach ($required in @(
        $DisplayManagerSource,
        $CarPlayChildPath,
        $StartupScript,
        $CleanupScript,
        $ProcessesScript,
        $FlagAtlasSource,
        $Iap2Protocol
    )) {
        Assert-FileExists $required 'required upstream release file'
    }

    $childObject =
        [System.IO.File]::ReadAllText($CarPlayChildPath) |
        ConvertFrom-Json

    if ($childObject.exec -ne 'carplay_startup.sh') {
        throw 'Unexpected upstream CarPlay child exec. Manual review required.'
    }

    if ($childObject.path -ne '/mnt/app/root/hooks') {
        throw 'Unexpected upstream CarPlay child path. Manual review required.'
    }

    if (
        $childObject.cleanupScript -ne
        '/mnt/app/root/hooks/carplay_cleanup.sh'
    ) {
        throw 'Unexpected upstream cleanupScript. Manual review required.'
    }

    foreach ($envLine in @($childObject.envs)) {
        if ($envLine -match 'LD_PRELOAD') {
            throw @"
Upstream children.carplay.envs now contains LD_PRELOAD.

That violates the supervisor ownership model previously validated.
Manual review required.
"@
        }
    }

    Set-Guard 'upstreamSupervisorNoLdPreload'

    $iap2Text =
        [System.IO.File]::ReadAllText($Iap2Protocol)

    # Each ID is checked against the symbol it belongs to, so a renumbering
    # upstream fails here rather than silently changing what gets registered
    # in dio_manager.json below.
    foreach ($rgiMessage in $ExpectedRgiMessages) {
        $definition =
            '#define\s+' + [regex]::Escape($rgiMessage.Symbol) +
            '\s+' + [regex]::Escape($rgiMessage.Id) + '\b'

        if ($iap2Text -notmatch $definition) {
            throw @"
Upstream no longer defines the validated RGI message ID:
    $($rgiMessage.Symbol) = $($rgiMessage.Id)

hook/framework/iap2_protocol.h is the authoritative definition behind the
dio_manager.json registration. Manual review required before preparing this
update.
"@
        }
    }

    Set-Guard 'upstreamRgiMessageIds'


Write-Step 'Building native components from the exported source'
$Docker = (Get-Command docker -ErrorAction Stop).Source
$ToolchainImageId = (Invoke-Native -Exe $Docker -Arguments @('image','inspect','--format','{{.Id}}','qnx65-armv7-toolchain:latest') -Capture | Select-Object -First 1).Trim()
if ($ExpectedToolchainImageId -and $ToolchainImageId -ne $ExpectedToolchainImageId) { throw 'QNX toolchain image does not match the requested pin.' }
$savedImage = $env:QNX_TOOLCHAIN_IMAGE
# Pass the inspected immutable ID to every native step, even if a tag moves. The native
# steps run in Docker alongside the Java build and tests below; they share no files
# (build/ versus the Java scratch) and are collected before anything uses their output.
$env:QNX_TOOLCHAIN_IMAGE = $ToolchainImageId
$NativeSteps = [System.Collections.Generic.List[string[]]]::new()
$NativeSteps.Add([string[]]@((Join-Path $WorkTree 'scripts/build_hook.sh').Replace('\','/')))
$NativeSteps.Add([string[]]@((Join-Path $WorkTree 'scripts/build_renderers.sh').Replace('\','/')))
$NativeSteps.Add([string[]]@((Join-Path $WorkTree 'tests/most/run-native-tests.sh').Replace('\','/'), $WorkTree.Replace('\','/')))
# Each step runs through Invoke-IsolatedProcess: its shell gets its own hidden console. Two Git
# Bash trees on one console, these steps' and the Java tests' on the main thread, can deadlock
# in the MSYS2 runtime (Git for Windows 2.55, msys-2.0.dll 3.6.10): a shell waits forever on a
# docker that has already exited. A PC repro hung 4 of 4 times on a shared console, with or
# without MSYS=disable_pcon, and 0 of 4 with this job's shells in their own console. The shell
# writes the step's stdout and stderr, in order, to a file in the scratch tree, read after the
# step exits (asynchronous pipe reads came back empty inside the build). A step still running
# after 20 minutes (they take about a minute) is killed and fails the build by name with the
# output it had written.
$nativeJob = Start-ThreadJob -ArgumentList $GitSh, $NativeSteps, $ScratchRoot, "$PSScriptRoot/PackageTools.ps1" -ScriptBlock {
    param($GitSh, $Steps, $ScratchRoot, $PackageTools)
    . $PackageTools
    $index = 0
    foreach ($step in $Steps) {
        $index++
        $outFile = Join-Path $ScratchRoot "native-step-$index.output.txt"
        $run = Invoke-IsolatedProcess -Exe $GitSh -OutputFile $outFile -TimeoutMs (20 * 60 * 1000) `
            -Arguments (@('-c', 'out=$1; shift; exec sh "$@" > "$out" 2>&1', 'sh', $outFile.Replace('\','/')) + $step)
        $output = @($run.Output | Where-Object { $_ -ne '' })
        if ($run.TimedOut) { $output += 'Native step timed out after 20 minutes and was killed.' }
        [pscustomobject]@{ Step = $step -join ' '; ExitCode = $run.ExitCode; Output = $output }
        if ($run.ExitCode -ne 0) { break }
    }
}
# Until the native steps are collected, a failure here must also stop them, or they go on
# writing build/ in the scratch tree kept for diagnosis. Stop-Job lets the step in progress
# finish (it does not kill the native process) and starts no further step.
try {
$java = & "$PSScriptRoot/Build-Java.ps1" -SourceRoot $WorkTree -StockJar $LsdJar -Dependencies $Dependencies -OutputRoot $JavaWork -BuildId $BuildId -JavaImage $JavaImageId
$BuiltJavaJar = $java.Jar
$JavaClassCount = $java.ClassCount
$JavaResources = $java.Resources
$HostTestImageId = (Invoke-Native -Exe $Docker -Arguments @('image','inspect','--format','{{.Id}}',$HostTestImage) -Capture | Select-Object -First 1).Trim()
# C half of the native route-guidance contract, in the host-test image; Test-Java.ps1
# feeds its frames to the built JAR. UBSan only: ASan hangs at random under Docker.
$RgdContractFrames = Join-Path $ScratchRoot 'rgd-contract'
New-Item -ItemType Directory -Force -Path $RgdContractFrames | Out-Null
Invoke-Native -Exe $Docker -Arguments @('run','--rm','--network','none','--mount',('type=bind,source=' + $WorkTree + ',target=/src,readonly'),'--mount',('type=bind,source=' + $RgdContractFrames + ',target=/out'),'--workdir','/src',$HostTestImageId,'bash','-c','export PATH=/usr/sbin:/usr/bin:/sbin:/bin PYTHONDONTWRITEBYTECODE=1 RGD_CONTRACT_STAGE=native RGD_CONTRACT_OUT=/out RGD_CONTRACT_SANITIZE=undefined; python3 tests/test_rgd_native_contract.py')
. "$PSScriptRoot/Test-Java.ps1"
Write-Step 'Collecting the native build'
$NativeResults = @(Receive-Job -Job $nativeJob -Wait -AutoRemoveJob)
}
finally {
    $env:QNX_TOOLCHAIN_IMAGE = $savedImage
    if ($nativeJob.State -eq 'Running') { Stop-Job -Job $nativeJob }
    Remove-Job -Job $nativeJob -Force -ErrorAction SilentlyContinue
}
foreach ($result in $NativeResults) {
    $result.Output | ForEach-Object { Write-Host $_ }
    if ($result.ExitCode -ne 0) { throw "Native step failed ($($result.ExitCode)): $($result.Step)" }
}
if ($NativeResults.Count -ne $NativeSteps.Count) { throw 'Not every native step ran.' }
# MOST host suites: a zero exit is not enough, each must report exactly once, with no failures.
$mostOutput = $NativeResults[-1].Output
$MostSuiteResults = @(foreach ($suite in @('most_output_platform_test','most_output_render_test','most_mask_cache_test')) {
    $lines = @($mostOutput | Where-Object { $_ -match ('^' + $suite + ': [1-9][0-9]* checks, 0 failures$') })
    if ($lines.Count -ne 1) { throw "Native host suite $suite did not report one passing result." }
    [ordered]@{ name = $suite; result = $lines[0] }
})
$DownloadedHook = Join-Path $WorkTree 'build/libcarplay_hook.so'
$PackagedRenderer = Join-Path $WorkTree 'build/maneuver_render'
Assert-ElfArm32 -Path $DownloadedHook -Name 'hook'
Assert-ElfArm32 -Path $PackagedRenderer -Name 'renderer'
Set-Guard 'nativeSourceBuild'
Set-Guard 'nativeArm32Elf'
Set-Guard 'mostOutputHostTests' ('PASS (' + (($MostSuiteResults | ForEach-Object { $_.result }) -join '; ') + ')')
# Native and shell checks consume the same source and freshly built JAR.
Copy-Item -LiteralPath $BuiltJavaJar -Destination (Join-Path $WorkTree 'build/carplay_hook.jar')
Invoke-Native -Exe $Docker -Arguments @('run','--rm','--network','none','--mount',('type=bind,source=' + $WorkTree + ',target=/src,readonly'),'--workdir','/src',$HostTestImageId,'bash','-c','export PATH=/usr/sbin:/usr/bin:/sbin:/bin; bash scripts/run_tests.sh')
$BuiltJarSha256 = Get-Sha256 $BuiltJavaJar
. "$PSScriptRoot/Stage-Package.ps1"

Write-Step 'Recording source provenance and validating the complete package'
$missing = @($Guards.Keys | Where-Object { $Guards[$_] -eq 'NOT-RUN' })
if ($missing.Count) { throw "Unrun compatibility checks: $($missing -join ', ')" }
$manifest = [ordered]@{
    schemaVersion = 5
    package = 'mib2q-carplay-rgi-' + $FirmwareName
    preparedUtc = [DateTime]::UtcNow.ToString('o')
    source = [ordered]@{ kind = 'git-archive'; commit = $Commit; label = $Tag }
    vehicleBaseline = [ordered]@{ firmwareTrain = $FirmwareTrain; muVersion = $FirmwareName; mmxImage = $FirmwareMmxImage }
    build = [ordered]@{
        buildId = $BuildId
        firmwareProfile = [ordered]@{ name = $FirmwareName; sha256 = $FirmwareProfileSha256 }
        javaClassCount = $JavaClassCount
        javaTargetMajorVersion = 48
        javaToolchainImageId = $JavaImageId
        javaCompilerVersion = (Get-Content -LiteralPath (Join-Path $JavaWork 'javac-version.txt') -Raw).Trim()
        javaResources = @($JavaResources | ForEach-Object { [ordered]@{ jarEntry=$_.path; bytes=$_.bytes; sha256=$_.sha256 } })
        hostTests = $HostSuiteResults
        shellTemplates = $TemplateProvenance
        nativeToolchainImageId = $ToolchainImageId
        hostTestImageId = $HostTestImageId
    }
    nativeArtifacts = [ordered]@{
        sourceCommit = $Commit
        hookSha256 = Get-Sha256 $DownloadedHook
        rendererSha256 = Get-Sha256 $PackagedRenderer
        flagAtlas = $AtlasProvenance
    }
    firmwareInputs = [ordered]@{ lsdJxeSha256=$ExpectedLsdJxeSha256; lsdJarSha256=$ExpectedLsdJarSha256 }
    compatibilityGuards = $Guards
    configPatches = $ConfigPatchPlan
    installer = [ordered]@{
        armedAction = $ArmedAction
        actionAfterSuccessfulInstall = 'rollback'
        automaticReboot = $false
        rollbackCapturesLogsFirst = $true
    }
}
Write-LfUtf8 -Path (Join-Path $PackageRoot 'PREINSTALL.txt') -Content @"
CarPlay RGI $FirmwareName / $FirmwareTrain
Source commit: $Commit
Build ID: $BuildId
QNX toolchain: $ToolchainImageId
Initial ACTION: $ArmedAction

This package contains private stock configuration and rollback inputs. Do not publish it.
Copy the contents of sdcard to the M.I.B. card. Run Individual Script once.
Only an install result 0 arms rollback; the result is the last line shown. The next run captures logs and uninstalls.
Review the result before restarting the unit manually. There is no automatic reboot.
Local checks do not establish vehicle behavior on an untested firmware or cluster.
"@
    # ACTION (install or rollback) may be changed on the card, so it is not listed.
    $payloadFileObjects = Get-PackageFileDigests `
        -Root $PackageRoot `
        -Exclude @('meta/manifest.json', 'meta/SHA256SUMS.txt', 'ACTION')

    $manifest.files = $payloadFileObjects

    $ManifestPath =
        Join-Path $MetaDirectory 'manifest.json'

    Write-LfUtf8 `
        -Path $ManifestPath `
        -Content (($manifest | ConvertTo-Json -Depth 10) + "`n")

    # SHA256 list, excluding itself.
    # Reuses the digests already computed for the manifest and adds the
    # manifest itself, which did not exist when those were taken.
    $sumRecords =
        @($payloadFileObjects) + @(
            [ordered]@{
                path = 'meta/manifest.json'
                sha256 = Get-Sha256 $ManifestPath
            }
        )

    $sumLines =
        @(
            Invoke-OrdinalSort -Items $sumRecords -Property 'path' |
            ForEach-Object { $_.sha256 + '  ' + $_.path }
        )

    $SumsPath = Join-Path $MetaDirectory 'SHA256SUMS.txt'

    Write-LfUtf8 `
        -Path $SumsPath `
        -Content (($sumLines -join "`n") + "`n")

    Assert-Sha256SumsFile `
        -Root $PackageRoot `
        -SumsPath $SumsPath

    $PublicationGuards = [ordered]@{ stagedChecksums = 'PASS' }
    Write-Host 'stagedChecksums : PASS'


& "$PSScriptRoot/Tests/Test-Preparation.ps1" -PackageDirectory $ReleaseDirectory -TestRootParent $ScratchRoot
& "$PSScriptRoot/Tests/Test-Installer.ps1" -PackageDirectory $ReleaseDirectory -TestRootParent $ScratchRoot
Write-LfUtf8 -Path (Join-Path $ReleaseDirectory 'PACKAGE-VALIDATION.json') -Content (([ordered]@{
    schemaVersion = 4; commit = $Commit; sourceLabel = $Tag; armedAction = $ArmedAction
    result = 'LOCAL_CHECKS_PASSED_VEHICLE_TEST_PENDING'; publicationGuards = $PublicationGuards
    entryPoint = [ordered]@{ file='mod/command.sh'; sha256=Get-Sha256 $MibCommandPath; forwardsTo='mod/custom.sh' }
    dispatcher = [ordered]@{ file='mod/custom.sh'; sha256=Get-Sha256 (Join-Path $SdRoot 'mod/custom.sh'); matchesPackagedCopy='mod/carplay-rgi/installer/command.sh' }
    stagedManifestSha256 = Get-Sha256 $ManifestPath
} | ConvertTo-Json -Depth 10) + "`n")
