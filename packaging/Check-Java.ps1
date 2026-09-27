#requires -version 7.0
param(
    [Parameter(Mandatory=$true)][string]$StockJar,
    [Parameter(Mandatory=$true)][string]$Dependencies,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [string]$JavaImage = 'eclipse-temurin:8-jdk-jammy',
    [string]$JavaHome = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/PackageTools.ps1"
. "$PSScriptRoot/BuildInputs.ps1"
$WorkTree = Split-Path -Parent $PSScriptRoot
$LsdJar = [IO.Path]::GetFullPath($StockJar)
$DepsDirectory = [IO.Path]::GetFullPath($Dependencies)
. "$PSScriptRoot/Dependencies.ps1"
$JavaWork = [IO.Path]::GetFullPath($OutputRoot)
$FirmwareName = 'supplied stock'
$java = & "$PSScriptRoot/Build-Java.ps1" -SourceRoot $WorkTree -StockJar $LsdJar -Dependencies $DepsDirectory -OutputRoot $JavaWork -BuildId host-check -JavaImage $JavaImage
$BuiltJavaJar = $java.Jar
$JavaImageId = $java.ImageId
$Guards = [ordered]@{ javaVcTextRuntimeLoad='NOT-RUN'; javaHostSuites='NOT-RUN'; javaStockLinkage='NOT-RUN' }
. "$PSScriptRoot/Test-Java.ps1"
