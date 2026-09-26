# Internal test step; supplied stock bytecode stays outside the repository.
    # Host-side runtime checks. Integrity checks faithfully verified an
    # incomplete JAR in September; these execute the shipping classes instead.

    Write-Step "Running host Java tests against the built JAR and $FirmwareName lsd.jar"

    $JavaExe = Join-Path $JavaHome ('bin/java' + $script:ExeSuffix)
    Assert-FileExists $JavaExe 'JDK 8 java.exe'

    $HostTestRoot = Join-Path $JavaWork 'host-tests'
    $HostTestClasses = Join-Path $HostTestRoot 'classes'
    # Deliberately empty working directory: the VC text data must come from
    # the JAR alone, never from a checkout file that happens to be visible.
    $HostTestCwd = Join-Path $HostTestRoot 'cwd'
    New-Item -ItemType Directory -Force -Path $HostTestClasses, $HostTestCwd | Out-Null

    $UpstreamTestRoot = Join-Path $WorkTree 'tests'

    # Upstream's own host suites (scripts/test_route_info.sh and
    # scripts/test_java_transports.sh), pointed at this firmware instead of MU1316.
    $UpstreamSuiteNames = @(
        'VCTextScrollTest', 'RouteInfoPresentationTest', 'RendererMapperDirectionTest',
        'RouteGuidanceDeltaTest', 'DistanceBargraphChainTest', 'KomoGraphicsStateTest',
        'ManeuverParityTest', 'RendererViewportTest', 'ClusterKdkSyncTest',
        'ClusterKdkBapChainTest', 'ClusterKdkRendererLifecycleTest',
        'LaneGuidanceTransportTest', 'LaneGuidanceLifecycleTest', 'RgiDeliveryRecoveryTest',
        'CurrentPositionDeliveryTest', 'RouteInfoTimeoutTest', 'CurrentPositionStockChainTest',
        'TouchpadControllerTest', 'CarplayBusTransportTest', 'RendererServerTransportTest',
        'GatedCombiServiceInitStateTest', 'ManeuverChainAudit',
        'RgdTeardownTest', 'NativeGuidanceGateTest', 'NativeGuidanceStateTest'
    )

    $hostTestSources = @(
        foreach ($suiteName in $UpstreamSuiteNames) {
            $suiteSource = Join-Path $UpstreamTestRoot ($suiteName + '.java')
            Assert-FileExists $suiteSource 'pinned upstream host test'
            $suiteSource
        }
    )

    $hostTestList = Join-Path $HostTestRoot 'sources.txt'
    Write-LfUtf8 `
        -Path $hostTestList `
        -Content ((($hostTestSources | ForEach-Object { '"' + $_.Replace('\', '/') + '"' }) -join "`n") + "`n")

    $HostClassPath = $BuiltJavaJar + [IO.Path]::PathSeparator + $LsdJar + [IO.Path]::PathSeparator + $OsgiFramework + [IO.Path]::PathSeparator + $OsgiTracker

    # Tests are host code (Unsafe, reflection), so they are compiled for the
    # host JDK; only the shipping classes are held to Java 1.4.
    Invoke-Native `
        -Exe $Javac `
        -Arguments @(
            '-nowarn', '-encoding', 'UTF-8',
            '-cp', $HostClassPath,
            '-d', $HostTestClasses,
            ('@' + $hostTestList)
        ) `
        -Capture | Out-Null

    # Classes reconstructed from the romized JXE carry J9 invokespecial forms
    # that HotSpot's verifier rejects; upstream disables it for these probes.
    $HostSuiteRuns = @(
        [ordered]@{ Name = 'VCTextScrollTest'; Main = 'com.luka.carplay.rgd.VCTextScrollTest'; ClassPath = ($HostTestClasses + [IO.Path]::PathSeparator + $BuiltJavaJar); Verify = $true; Arguments = @() }
        [ordered]@{ Name = 'RouteInfoPresentationTest'; Main = 'RouteInfoPresentationTest'; Verify = $true; Arguments = @() }
        [ordered]@{ Name = 'RendererMapperDirectionTest'; Main = 'RendererMapperDirectionTest'; Verify = $true; Arguments = @() }
        [ordered]@{ Name = 'RouteGuidanceDeltaTest'; Main = 'RouteGuidanceDeltaTest'; Verify = $true; Arguments = @() }
        [ordered]@{ Name = 'DistanceBargraphChainTest'; Main = 'DistanceBargraphChainTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'KomoGraphicsStateTest'; Main = 'KomoGraphicsStateTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'ManeuverParityTest'; Main = 'ManeuverParityTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'RendererViewportTest'; Main = 'RendererViewportTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'ClusterKdkSyncTest'; Main = 'ClusterKdkSyncTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'ClusterKdkBapChainTest'; Main = 'ClusterKdkBapChainTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'ClusterKdkRendererLifecycleTest'; Main = 'ClusterKdkRendererLifecycleTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'LaneGuidanceTransportTest'; Main = 'com.luka.carplay.rgd.LaneGuidanceTransportTest'; Verify = $false; Arguments = @((Join-Path $HostTestRoot 'lane-guidance-wire.bin')) }
        [ordered]@{ Name = 'LaneGuidanceLifecycleTest'; Main = 'com.luka.carplay.rgd.LaneGuidanceLifecycleTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'RgiDeliveryRecoveryTest'; Main = 'com.luka.carplay.rgd.RgiDeliveryRecoveryTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'CurrentPositionDeliveryTest'; Main = 'com.luka.carplay.rgd.CurrentPositionDeliveryTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'RouteInfoTimeoutTest'; Main = 'com.luka.carplay.rgd.RouteInfoTimeoutTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'CurrentPositionStockChainTest'; Main = 'com.luka.carplay.rgd.CurrentPositionStockChainTest'; Verify = $false; Arguments = @() }
        [ordered]@{ Name = 'TouchpadControllerTest'; Main = 'TouchpadControllerTest'; ClassPath = ($HostTestClasses + [IO.Path]::PathSeparator + $BuiltJavaJar); Verify = $true; Arguments = @() }
        [ordered]@{ Name = 'CarplayBusTransportTest'; Main = 'com.luka.carplay.bus.CarplayBusTransportTest'; ClassPath = ($HostTestClasses + [IO.Path]::PathSeparator + $BuiltJavaJar); Verify = $true; Arguments = @() }
        [ordered]@{ Name = 'RendererServerTransportTest'; Main = 'com.luka.carplay.rgd.RendererServerTransportTest'; ClassPath = ($HostTestClasses + [IO.Path]::PathSeparator + $BuiltJavaJar); Verify = $true; Arguments = @() }
        [ordered]@{ Name = 'GatedCombiServiceInitStateTest'; Main = 'com.luka.carplay.rgd.GatedCombiServiceInitStateTest'; ClassPath = ($HostTestClasses + [IO.Path]::PathSeparator + $BuiltJavaJar + [IO.Path]::PathSeparator + $LsdJar); Verify = $true; Arguments = @() }
    )
    foreach ($main in @('com.luka.carplay.core.RgdTeardownTest','com.luka.carplay.core.NativeGuidanceGateTest','NativeGuidanceStateTest')) {
        $HostSuiteRuns += [ordered]@{ Name = $main.Split('.')[-1]; Main = $main; Verify = $false; Arguments = @() }
    }
    $HostSuiteResults = @()

    Push-Location $HostTestCwd

    try {
        foreach ($run in $HostSuiteRuns) {
            $runClassPath = if ($run.Contains('ClassPath')) { $run.ClassPath }
                else { $HostTestClasses + [IO.Path]::PathSeparator + $HostClassPath }

            $javaArguments = @()
            if (-not $run.Verify) {
                $javaArguments += '-Xverify:none'
            }
            $javaArguments += @('-cp', $runClassPath, $run.Main) + @($run.Arguments)

            $suiteOutput = Invoke-Native -Exe $JavaExe -Arguments $javaArguments -Capture
            $suiteSummary = (@($suiteOutput) | Where-Object { $_ -match '\S' } | Select-Object -Last 1)

            if ($suiteSummary -notmatch 'PASS') {
                throw "Host test $($run.Name) did not report PASS:`n$(@($suiteOutput) -join "`n")"
            }

            Write-Host "PASS $($run.Name)"
            $HostSuiteResults += [ordered]@{ name = $run.Name; result = $suiteSummary.Trim() }
        }
    }
    finally {
        Pop-Location
    }

    # Suites with their own compilation sets (formerly scripts/test_java_transports.sh,
    # scripts/test_pdc.sh and tests/test_rgd_native_contract.py). A suite passes when its
    # last line reports PASS; a negative control must fail on the unpatched stock classes
    # with the defect it names.
    $PatchCore = Join-Path $WorkTree 'java_patch/com/luka/carplay/core'
    $Stubs = Join-Path $UpstreamTestRoot 'stubs'
    $AsmClassPath = $AsmJar + [IO.Path]::PathSeparator + $AsmTreeJar
    $OsgiClassPath = $OsgiFramework + [IO.Path]::PathSeparator + $OsgiTracker
    $compileSet = {
        param([string]$Name, [string]$ClassPath, [string[]]$Sources)
        $directory = Join-Path $HostTestRoot $Name
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        $arguments = @('-nowarn', '-encoding', 'UTF-8', '-d', $directory)
        if ($ClassPath) { $arguments += @('-cp', $ClassPath) }
        Invoke-Native -Exe $Javac -Arguments ($arguments + $Sources) -Capture | Out-Null
        $directory
    }
    $runSuite = {
        param([string]$Name, [string]$ClassPath, [string]$Main, [string[]]$Arguments, [switch]$NoVerify)
        $javaArguments = @()
        if ($NoVerify) { $javaArguments += '-Xverify:none' }
        $suiteOutput = Invoke-Native -Exe $JavaExe -Arguments ($javaArguments + @('-cp', $ClassPath, $Main) + @($Arguments)) -Capture
        $suiteSummary = (@($suiteOutput) | Where-Object { $_ -match '\S' } | Select-Object -Last 1)
        if ($suiteSummary -notmatch 'PASS') {
            throw "Host test $Name did not report PASS:`n$(@($suiteOutput) -join "`n")"
        }
        Write-Host "PASS $Name"
        $script:HostSuiteResults += [ordered]@{ name = $Name; result = $suiteSummary.Trim() }
    }
    $expectStockFailure = {
        param([string]$Name, [string]$ClassPath, [string]$Main, [string]$Defect)
        $eap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { $stockOutput = @(& $JavaExe -cp $ClassPath $Main 2>&1 | ForEach-Object { $_.ToString() }); $code = $LASTEXITCODE }
        finally { $ErrorActionPreference = $eap }
        if ($code -eq 0 -or -not ($stockOutput -join "`n").Contains($Defect)) {
            throw "Negative control $Name did not reproduce the stock defect '$Defect':`n$($stockOutput -join "`n")"
        }
        Write-Host "PASS $Name (stock reproduces: $Defect)"
        $script:HostSuiteResults += [ordered]@{ name = $Name; result = "PASS (unpatched stock fails: $Defect)" }
    }

    Push-Location $HostTestCwd
    try {
        # The real lifecycle worker against controllable external modules.
        $lifecycle = & $compileSet 'lifecycle' '' @(
            (Join-Path $PatchCore 'CarPlayApp.java'), (Join-Path $PatchCore 'Module.java'),
            (Join-Path $UpstreamTestRoot 'CarPlayAppLifecycleTest.java'),
            (Join-Path $Stubs 'app-lifecycle/com/luka/carplay/core/LifecycleFixtures.java'),
            (Join-Path $Stubs 'app-lifecycle/com/luka/carplay/bus/CarplayBus.java'),
            (Join-Path $Stubs 'app-lifecycle/com/luka/carplay/framework/Log.java'),
            (Join-Path $Stubs 'app-lifecycle/com/luka/carplay/pdc/PdcSmallStageGuard.java'),
            (Join-Path $Stubs 'app-lifecycle/de/audi/app/terminalmode/IContext.java'),
            (Join-Path $Stubs 'app-lifecycle/de/audi/atip/base/IFrameworkAccess.java'))
        foreach ($scenario in @('publication','during-start','replug','failure','bounce')) {
            & $runSuite "CarPlayAppLifecycleTest $scenario" $lifecycle 'com.luka.carplay.core.CarPlayAppLifecycleTest' @($scenario)
        }

        # Parking popups: this firmware's stock resource policy, properties and commands
        # with a fake physical HMI.
        $pdcStubs = & $compileSet 'pdc-stubs' ($LsdJar + [IO.Path]::PathSeparator + $OsgiClassPath) @(
            (Join-Path $Stubs 'pdc/com/luka/carplay/core/CarPlayApp.java'),
            (Join-Path $Stubs 'app-lifecycle/com/luka/carplay/framework/Log.java'),
            (Join-Path $Stubs 'pdc/de/audi/atip/hmi/view/Screen.java'),
            (Join-Path $Stubs 'pdc/de/esolutions/hmi/widgets/audi/base/AbstractScreenWidget.java'))
        $pdcBase = @($pdcStubs, $BuiltJavaJar, $LsdJar, $OsgiClassPath, $AsmClassPath) -join [IO.Path]::PathSeparator
        $pdcTests = & $compileSet 'pdc-tests' $pdcBase @('PdcResourcePolicyTest','PdcExternalEventsTest','OpsAudioDrawerTest','OpsStatusLineTest' |
            ForEach-Object { Join-Path $UpstreamTestRoot ($_ + '.java') })
        foreach ($suite in @('PdcResourcePolicyTest','PdcExternalEventsTest','OpsAudioDrawerTest','OpsStatusLineTest')) {
            & $runSuite $suite ($pdcTests + [IO.Path]::PathSeparator + $pdcBase) $suite @()
        }
        # The PDC suites fake CarPlayApp.active; the real lifecycle worker must release the
        # presentation policy on disconnect without a later HMI callback.
        $pdcLifecycle = & $compileSet 'pdc-lifecycle' $pdcBase @(
            (Join-Path $PatchCore 'CarPlayApp.java'), (Join-Path $PatchCore 'Module.java'),
            (Join-Path $Stubs 'app-lifecycle/com/luka/carplay/core/LifecycleFixtures.java'),
            (Join-Path $Stubs 'app-lifecycle/com/luka/carplay/bus/CarplayBus.java'),
            (Join-Path $Stubs 'app-lifecycle/com/luka/carplay/framework/Log.java'))
        $pdcLifecycleCp = @($pdcLifecycle, $pdcTests, $pdcBase) -join [IO.Path]::PathSeparator
        $pdcLifecycleTest = & $compileSet 'pdc-lifecycle-test' $pdcLifecycleCp @((Join-Path $UpstreamTestRoot 'CarPlayPdcLifecycleTest.java'))
        & $runSuite 'CarPlayPdcLifecycleTest' ($pdcLifecycleTest + [IO.Path]::PathSeparator + $pdcLifecycleCp) 'CarPlayPdcLifecycleTest' @()
        # Stock classes first on the class path: each suite must fail with the stock defect.
        $stockFirst = @($pdcTests, $pdcStubs, $LsdJar, $BuiltJavaJar, $OsgiClassPath, $AsmClassPath) -join [IO.Path]::PathSeparator
        & $expectStockFailure 'PdcResourcePolicyTest on stock' $stockFirst 'PdcResourcePolicyTest' 'pure OPS 108 toggled Main Wizard'
        & $expectStockFailure 'OpsAudioDrawerTest on stock' $stockFirst 'OpsAudioDrawerTest' 'APS drawer still selected over CarPlay + side OPS'
        & $expectStockFailure 'OpsStatusLineTest on stock' $stockFirst 'OpsStatusLineTest' 'MMI status line 62 remained over CarPlay'

        # Native route-guidance contract: frames written by the real C parser and slot
        # writer (the package builder runs that half in Docker), parsed by the built JAR
        # and sent through this firmware's stock BAP classes.
        if ((Test-Path variable:RgdContractFrames) -and $RgdContractFrames) {
            $frames = @('old-slot.txt','new-no-angle.txt','new-known-angle.txt','new-generation.txt' |
                ForEach-Object { Join-Path $RgdContractFrames $_ })
            foreach ($frame in $frames) { Assert-FileExists $frame 'native RGI contract frame' }
            $probe = & $compileSet 'rgd-contract' ($HostTestClasses + [IO.Path]::PathSeparator + $HostClassPath) @((Join-Path $UpstreamTestRoot 'RgdNativeContractProbe.java'))
            & $runSuite 'RgdNativeContractProbe' (@($probe, $HostTestClasses, $HostClassPath) -join [IO.Path]::PathSeparator) 'RgdNativeContractProbe' $frames -NoVerify
        } else {
            Write-Host 'SKIPPED RgdNativeContractProbe: no native frames supplied (the package build runs it)'
        }
    }
    finally {
        Pop-Location
    }

    $vcTextResult = @($HostSuiteResults | Where-Object { $_.name -eq 'VCTextScrollTest' })[0].result
    if ($vcTextResult -notmatch 'VCTextScrollTest: PASS') {
        throw "VCTextScrollTest summary is not a pass: $vcTextResult"
    }

    Set-Guard 'javaVcTextRuntimeLoad' ("PASS ($vcTextResult; vc-text.bin loaded from the built JAR only)")
    Set-Guard 'javaHostSuites' ("PASS ($($HostSuiteResults.Count) suites against the supplied stock JAR)")

    # Every member reference in both directions: patch -> stock, and every
    # remaining stock caller of a replaced class -> the replacement.
    $AuditClasses = Join-Path $HostTestRoot 'audit'
    New-Item -ItemType Directory -Force -Path $AuditClasses | Out-Null
    $AuditSource = Join-Path $UpstreamTestRoot 'JavaStockLinkageAudit.java'
    Assert-FileExists $AuditSource 'pinned upstream JavaStockLinkageAudit.java'

    Invoke-Native `
        -Exe $Javac `
        -Arguments @('-nowarn', '-cp', $AsmClassPath, '-d', $AuditClasses, $AuditSource) `
        -Capture | Out-Null

    $auditOutput = Invoke-Native `
        -Exe $JavaExe `
        -Arguments @(
            '-Xmx2g', '-cp', ($AuditClasses + [IO.Path]::PathSeparator + $AsmClassPath), 'JavaStockLinkageAudit',
            $BuiltJavaJar, $LsdJar, $OsgiFramework, $OsgiTracker
        ) `
        -Capture
    $auditSummary = (@($auditOutput) | Select-Object -Last 1)

    if ($auditSummary -notmatch '^JavaStockLinkageAudit: .*errors=0$') {
        throw "Stock linkage audit against $FirmwareName lsd.jar failed:`n$(@($auditOutput) -join "`n")"
    }

    Set-Guard 'javaStockLinkage' ("PASS ($($auditSummary.Substring('JavaStockLinkageAudit: '.Length)))")
