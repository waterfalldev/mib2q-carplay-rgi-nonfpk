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
    $AsmClassPath = $AsmJar + [IO.Path]::PathSeparator + $AsmTreeJar

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
