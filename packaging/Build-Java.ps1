#requires -version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$StockJar,
    [Parameter(Mandatory=$true)][string]$Dependencies,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9._-]+$')][string]$BuildId,
    [string]$JavaImage = 'eclipse-temurin:8-jdk-jammy',
    # Accepted for older callers; all Java tools now run in Docker.
    [string]$JavaHome = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/PackageTools.ps1"
. "$PSScriptRoot/BuildInputs.ps1"
. "$PSScriptRoot/JavaDocker.ps1"
$WorkTree = [IO.Path]::GetFullPath($SourceRoot)
$LsdJar = [IO.Path]::GetFullPath($StockJar)
$DepsDirectory = [IO.Path]::GetFullPath($Dependencies)
$JavaWork = [IO.Path]::GetFullPath($OutputRoot)
Assert-FileExists $LsdJar 'external stock JAR'
if (Test-Path -LiteralPath $JavaWork) { throw "Java output must be a fresh directory: $JavaWork" }
New-Item -ItemType Directory -Path $JavaWork | Out-Null
. "$PSScriptRoot/Dependencies.ps1"
$JavaImageId = Resolve-JavaImage $JavaImage
Invoke-JavaDocker -SourceRoot $WorkTree -StockJar $LsdJar -Dependencies $DepsDirectory `
    -OutputRoot $JavaWork -Image $JavaImageId -BuildId $BuildId | ForEach-Object { Write-Host $_ }
$BuiltJavaJar = Join-Path $JavaWork 'carplay_hook.jar'
$JavaResources = Get-JavaResourceInventory -ResourceRoot (Join-Path $WorkTree 'java_resources')
$count = Assert-JavaJar -Path $BuiltJavaJar -ExpectedMajor 48 -RequiredClasses @('com/luka/carplay/core/CarPlayApp.class','com/luka/carplay/rgd/VCTextData.class')
Assert-JavaResources -Path $BuiltJavaJar -Resources $JavaResources
[pscustomobject]@{ Jar = $BuiltJavaJar; ClassCount = $count; Resources = $JavaResources; BuildId = $BuildId; ImageId = $JavaImageId }
