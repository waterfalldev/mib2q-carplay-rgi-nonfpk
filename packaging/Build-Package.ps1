#requires -version 5.1
<#
.SYNOPSIS
Build a checked M.I.B. package from one local Git commit and external stock inputs.
.DESCRIPTION
No fetch, push or vehicle access. Source is exported with LF line endings; the
exported builder, runtime, tests and templates all belong to the recorded commit.
Firmware, dependencies, scratch and packages must be outside this repository.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$FirmwareRoot,
    [Parameter(Mandatory=$true)][string]$Dependencies,
    [Parameter(Mandatory=$true)][string]$WorkRoot,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [string]$Ref = 'HEAD',
    [string]$JavaHome = '',
    [string]$ExpectedToolchainImageId = '',
    [string]$HostTestImage = 'carplay-rgi-host-tests:local',
    # Optional name suffix, e.g. a local trial number; the commit still identifies the source.
    [ValidatePattern('^[A-Za-z0-9]*$')][string]$Label = '',
    [switch]$ForceRebuild,
    [switch]$ArmRollback
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
. "$PSScriptRoot/PackageTools.ps1"
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
foreach ($name in @('FirmwareRoot','Dependencies','WorkRoot','OutputRoot')) {
    $value = [IO.Path]::GetFullPath((Get-Variable $name -ValueOnly))
    # Reject both descendants and ancestors: a work root may never own the repo.
    $sep = [IO.Path]::DirectorySeparatorChar
    if (($value + $sep).StartsWith($repo + $sep, [StringComparison]::OrdinalIgnoreCase) -or
        ($repo + $sep).StartsWith($value + $sep, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$name must be separate from the repository: $value"
    }
    Set-Variable $name $value
}
$Git = (Get-Command git -ErrorAction Stop).Source
$gitArgs = @('-c', ('safe.directory=' + $repo.Replace('\','/')), '-C', $repo)
$dirty = @(Invoke-Native -Exe $Git -Arguments ($gitArgs + @('status','--porcelain')) -Capture)
if ($dirty.Count) { throw 'Commit or otherwise account for working changes before building a release package.' }
$commit = (Invoke-Native -Exe $Git -Arguments ($gitArgs + @('rev-parse','--verify', ($Ref + '^{commit}'))) -Capture | Select-Object -First 1).Trim()
if ($commit -notmatch '^[0-9a-f]{40}$') { throw 'Source did not resolve to one full Git commit.' }
$profileData = Import-PowerShellDataFile -LiteralPath (Join-Path $FirmwareRoot 'Firmware.psd1')
$firmware = [string]$profileData.Name
if ($firmware -notmatch '^[A-Za-z0-9_]+$') { throw 'Invalid firmware profile name.' }
$short = $commit.Substring(0,12)
New-Item -ItemType Directory -Force -Path $WorkRoot, $OutputRoot | Out-Null
$run = Join-Path $WorkRoot ('source-' + $short + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
Assert-SafeChildPath -Root $WorkRoot -Candidate $run -Description 'source export'
$source = Join-Path $run 'source'
$scratch = Join-Path $run 'scratch'
New-Item -ItemType Directory -Path $source, $scratch | Out-Null
$archive = Join-Path $run 'source.tar'
Invoke-Native -Exe $Git -Arguments ($gitArgs + @('-c','core.autocrlf=false','archive','--format=tar',('--output=' + $archive),$commit))
# Relative names: GNU tar (first on PATH under Git Bash) reads "C:\..." as a remote host.
$tar = (Get-Command tar -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
Push-Location -LiteralPath $run
try { Invoke-Native -Exe $tar -Arguments @('-xf','source.tar','-C','source') } finally { Pop-Location }
$final = Join-Path $OutputRoot ('mib2q-carplay-rgi_' + $firmware + '_source_' + $short + $(if ($Label) { '_' + $Label } else { '' }))
if ((Test-Path -LiteralPath $final) -and -not $ForceRebuild) { throw "Completed package already exists: $final" }
$pending = Join-Path $OutputRoot ('.pending-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $pending | Out-Null
# Failed exports/staging stay outside the repository for diagnosis. Completed
# packages move only after every check succeeds; publication restores moved
# packages if a later archive/publication move fails.
& (Join-Path $source 'packaging/Build-Snapshot.ps1') -FirmwareRoot $FirmwareRoot -Firmware $firmware `
    -Dependencies $Dependencies -ScratchRoot $scratch -ReleaseDirectory $pending -Commit $commit `
    -JavaHome $JavaHome -ExpectedToolchainImageId $ExpectedToolchainImageId -HostTestImage $HostTestImage -ArmRollback:$ArmRollback
Publish-PreparedPackage -Root $OutputRoot -Pending $pending -Final $final -AllowReplace:$ForceRebuild
Write-Host "Completed package: $final"
Write-Host "Exact source: $commit"
Write-Output $final
