    $BuildDependencies = @(
        [ordered]@{
            Name = 'org.osgi.framework'
            Version = '1.10.0'
            File = 'org.osgi.framework-1.10.0.jar'
            Description = 'OSGi framework JAR'
            Url = 'https://repo1.maven.org/maven2/org/osgi/org.osgi.framework/1.10.0/org.osgi.framework-1.10.0.jar'
            Sha256 = $OsgiFrameworkSha256
        }
        [ordered]@{
            Name = 'org.osgi.util.tracker'
            Version = '1.5.4'
            File = 'org.osgi.util.tracker-1.5.4.jar'
            Description = 'OSGi tracker JAR'
            Url = 'https://repo1.maven.org/maven2/org/osgi/org.osgi.util.tracker/1.5.4/org.osgi.util.tracker-1.5.4.jar'
            Sha256 = $OsgiTrackerSha256
        }
        [ordered]@{
            Name = 'org.ow2.asm:asm'
            Version = '9.7'
            File = 'asm-9.7.jar'
            Description = 'ASM JAR (host linkage audit only)'
            Url = 'https://repo1.maven.org/maven2/org/ow2/asm/asm/9.7/asm-9.7.jar'
            Sha256 = $AsmSha256
        }
        [ordered]@{
            Name = 'org.ow2.asm:asm-tree'
            Version = '9.7'
            File = 'asm-tree-9.7.jar'
            Description = 'ASM tree JAR (host linkage audit only)'
            Url = 'https://repo1.maven.org/maven2/org/ow2/asm/asm-tree/9.7/asm-tree-9.7.jar'
            Sha256 = $AsmTreeSha256
        }
    )

    $DependencyPaths = @{}

    foreach ($dependency in $BuildDependencies) {
        $dependencyPath = Join-Path $DepsDirectory $dependency.File

        if (-not (Test-Path -LiteralPath $dependencyPath)) {
            Save-WebFile `
                -Uri $dependency.Url `
                -OutFile $dependencyPath `
                -ExpectedSha256 $dependency.Sha256
        }

        Assert-FileExists $dependencyPath $dependency.Description

        # Rechecked on every run, not just after a download, so a corrupted or
        # tampered cache cannot survive silently.
        Assert-Sha256 `
            -Path $dependencyPath `
            -Expected $dependency.Sha256 `
            -Description $dependency.Description

        $DependencyPaths[$dependency.File] = $dependencyPath
    }

    $OsgiFramework = $DependencyPaths['org.osgi.framework-1.10.0.jar']
    $OsgiTracker = $DependencyPaths['org.osgi.util.tracker-1.5.4.jar']
    $AsmJar = $DependencyPaths['asm-9.7.jar']
    $AsmTreeJar = $DependencyPaths['asm-tree-9.7.jar']

    Write-Host 'OSGi and ASM dependency checksums: PASS'
