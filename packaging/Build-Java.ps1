#requires -version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$StockJar,
    [Parameter(Mandatory=$true)][string]$Dependencies,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9._-]+$')][string]$BuildId,
    [string]$JavaHome = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/PackageTools.ps1"
. "$PSScriptRoot/BuildInputs.ps1"
$JavaHome = Find-Jdk8 $JavaHome
$Javac = Join-Path $JavaHome ('bin/javac' + $script:ExeSuffix)
$Jar = Join-Path $JavaHome ('bin/jar' + $script:ExeSuffix)
$WorkTree = [IO.Path]::GetFullPath($SourceRoot)
$LsdJar = [IO.Path]::GetFullPath($StockJar)
$DepsDirectory = [IO.Path]::GetFullPath($Dependencies)
$JavaWork = [IO.Path]::GetFullPath($OutputRoot)
$FirmwareName = 'stock-compatible'
Assert-FileExists $LsdJar 'external stock JAR'
if (Test-Path -LiteralPath $JavaWork) { throw "Java output must be a fresh directory: $JavaWork" }
New-Item -ItemType Directory -Path $JavaWork | Out-Null
. "$PSScriptRoot/Dependencies.ps1"
    $JavaSourceRoot = Join-Path $WorkTree 'java_patch'

    if (-not (Test-Path -LiteralPath $JavaSourceRoot -PathType Container)) {
        throw "Missing java_patch source directory: $JavaSourceRoot"
    }

    $ClassesDirectory = Join-Path $JavaWork 'classes'
    $GeneratedDirectory = Join-Path $JavaWork 'generated'

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $ClassesDirectory, $GeneratedDirectory |
        Out-Null

    $GeneratedCore = Join-Path `
        $GeneratedDirectory `
        'com\luka\carplay\core'

    New-Item `
        -ItemType Directory `
        -Force `
        -Path $GeneratedCore |
        Out-Null

    $OriginalCarPlayApp = Join-Path `
        $JavaSourceRoot `
        'com\luka\carplay\core\CarPlayApp.java'

    Assert-FileExists $OriginalCarPlayApp 'CarPlayApp.java'

    $carPlaySource =
        [System.IO.File]::ReadAllText($OriginalCarPlayApp)

    if (-not $carPlaySource.Contains('@BUILD_ID@')) {
        throw 'CarPlayApp.java no longer contains @BUILD_ID@. Manual build review required.'
    }

    $GeneratedCarPlayApp =
        Join-Path $GeneratedCore 'CarPlayApp.java'

    Write-LfUtf8 `
        -Path $GeneratedCarPlayApp `
        -Content ($carPlaySource.Replace('@BUILD_ID@', $BuildId))

    $allSources =
        Get-ChildItem `
            -LiteralPath $JavaSourceRoot `
            -Recurse `
            -Filter '*.java' `
            -File

    $sourcePaths = @()

    foreach ($javaFile in $allSources) {
        if (
            $javaFile.FullName -ieq
            $OriginalCarPlayApp
        ) {
            continue
        }

        $sourcePaths += $javaFile.FullName
    }

    $sourcePaths += $GeneratedCarPlayApp

    $SourceList = Join-Path $JavaWork 'sources.txt'

    # javac parses @argfiles with StreamTokenizer, which treats a backslash as
    # an escape character inside quotes, so a quoted Windows path is silently
    # mangled. javac accepts forward slashes on Windows, and the quotes are
    # what allow a path containing spaces (the worktree lives under %TEMP%,
    # which sits below the user profile).
    $sourceListText =
        (
            $sourcePaths |
            ForEach-Object {
                '"' + $_.Replace('\', '/') + '"'
            }
        ) -join "`n"

    Write-LfUtf8 `
        -Path $SourceList `
        -Content ($sourceListText + "`n")

    $ClassPath =
        $LsdJar + [IO.Path]::PathSeparator +
        $OsgiFramework + [IO.Path]::PathSeparator +
        $OsgiTracker

    Write-Host "Compiling $($sourcePaths.Count) Java files..."
    Write-Host "Build ID: $BuildId"

    # lsd.jar carries the head unit's own class library (Foundation 1.1 - it
    # has java.lang.String but no StringBuilder), so using it as the bootstrap
    # classpath compiles against what the car will actually run. Without this,
    # javac resolves against JDK 8's rt.jar and a post-1.4 API compiles clean
    # here only to fail at runtime on the unit.
    Invoke-Native `
        -Exe $Javac `
        -Arguments @(
            '-encoding', 'UTF-8',
            '-source', '1.4',
            '-target', '1.4',
            '-bootclasspath', $LsdJar,
            '-cp', $ClassPath,
            '-sourcepath', ($GeneratedDirectory + [IO.Path]::PathSeparator + $JavaSourceRoot),
            '-d', $ClassesDirectory,
            '-Xlint:-options',
            ('@' + $SourceList)
        )

    # Non-class resources, exactly as upstream scripts/build_java.sh ships them
    # ("cp -R java_resources/. classes/"). Omitting this step is what left the
    # September JAR without com/luka/carplay/rgd/vc-text.bin.
    $JavaResourceRoot = Join-Path $WorkTree 'java_resources'
    $JavaResources = Get-JavaResourceInventory -ResourceRoot $JavaResourceRoot

    $VcTextSource = Join-Path $JavaSourceRoot 'com\luka\carplay\rgd\VCTextData.java'
    if ((Test-Path -LiteralPath $VcTextSource) -and
        (@($JavaResources | ForEach-Object { $_.path }) -cnotcontains 'com/luka/carplay/rgd/vc-text.bin')) {
        throw 'VCTextData.java is present but java_resources/ has no com/luka/carplay/rgd/vc-text.bin.'
    }

    foreach ($resource in $JavaResources) {
        if ($resource.path.EndsWith('.class')) {
            throw "java_resources/ may not ship class files: $($resource.path)"
        }
        $resourceTarget = Join-Path $ClassesDirectory ($resource.path.Replace('/', '\'))
        if (Test-Path -LiteralPath $resourceTarget) {
            throw "java_resources/ entry collides with compiled output: $($resource.path)"
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resourceTarget) | Out-Null
        Copy-Item -LiteralPath $resource.source -Destination $resourceTarget
    }

    Write-Host "Java resources copied into the JAR tree: $($JavaResources.Count)"

    $BuiltJavaJar =
        Join-Path $JavaWork 'carplay_hook.jar'

    Push-Location $ClassesDirectory

    try {
        Invoke-Native `
            -Exe $Jar `
            -Arguments @(
                'cf',
                $BuiltJavaJar,
                '.'
            )
    }
    finally {
        Pop-Location
    }

    Assert-FileExists $BuiltJavaJar "$FirmwareName carplay_hook.jar"

    Set-DeterministicJar $BuiltJavaJar


$count = Assert-JavaJar -Path $BuiltJavaJar -ExpectedMajor 48 -RequiredClasses @('com/luka/carplay/core/CarPlayApp.class','com/luka/carplay/rgd/VCTextData.class')
Assert-JavaResources -Path $BuiltJavaJar -Resources $JavaResources
Write-Host "Java 1.4: $count classes; complete resources"
[pscustomobject]@{ Jar = $BuiltJavaJar; ClassCount = $count; Resources = $JavaResources; BuildId = $BuildId }
