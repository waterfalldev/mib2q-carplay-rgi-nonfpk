param([string]$TestRootParent = [IO.Path]::GetTempPath())
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../PackageTools.ps1"
$root = Join-Path $TestRootParent ('profile-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
# Synthetic identifiers and hashes only; no firmware or vehicle fixture is shipped.
$profileText = @'
@{
    SchemaVersion = 2
    Name = 'FIXTURE'
    FirmwareTrain = 'TEST_TRAIN'
    MmxImage = 'TEST_IMAGE'
    LsdJxeSha256 = '0000000000000000000000000000000000000000000000000000000000000000'
    LsdJarSha256 = '1111111111111111111111111111111111111111111111111111111111111111'
    StockSmartphone = @{ Bytes = 1; Cksum = 1; Sha256 = '2222222222222222222222222222222222222222222222222222222222222222' }
    StockDio = @{ Bytes = 2; Cksum = 2; Sha256 = '3333333333333333333333333333333333333333333333333333333333333333' }
}
'@
$path = Join-Path $root 'Firmware.psd1'
Write-LfUtf8 -Path $path -Content $profileText
$loaded = Import-FirmwareProfile -Directory $root -Name FIXTURE
if ($loaded.Data.SchemaVersion -ne 2) { throw 'Valid external profile failed to load' }
$count = 1
foreach ($case in @(
    @{ Find='SchemaVersion = 2'; Replace='SchemaVersion = 1'; Error='SchemaVersion must be 2' }
    @{ Find="Name = 'FIXTURE'"; Replace="Name = 'OTHER'"; Error='names firmware' }
    @{ Find="    MmxImage = 'TEST_IMAGE'"; Replace=''; Error='missing MmxImage' }
    @{ Find='SchemaVersion = 2'; Replace='SchemaVersion = 2; JavaPatch = @{}'; Error='unknown key JavaPatch' }
    @{ Find='Bytes = 2'; Replace='Bytes = 0'; Error='whole number' }
    @{ Find='Cksum = 2;'; Replace=''; Error='StockDio is missing Cksum' }
    @{ Find='1111111111111111111111111111111111111111111111111111111111111111'; Replace='invalid'; Error='LsdJarSha256 is not valid' }
    @{ Find='TEST_TRAIN'; Replace="bad train"; Error='FirmwareTrain is not valid' }
)) {
    Write-LfUtf8 -Path $path -Content ($profileText.Replace($case.Find,$case.Replace))
    $failure = ''
    try { Import-FirmwareProfile -Directory $root -Name FIXTURE | Out-Null } catch { $failure = $_.Exception.Message }
    if ($failure -notmatch $case.Error) { throw "Profile validation failure: expected $($case.Error), got $failure" }
    $count++
}
Write-Host "Profile validation: $count checks passed"
