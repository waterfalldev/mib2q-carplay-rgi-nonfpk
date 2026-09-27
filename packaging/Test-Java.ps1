# Shared Docker test runner; this adapter records the package validation results.
Write-Step "Running Java tests against the built JAR and $FirmwareName stock library"
. "$PSScriptRoot/JavaDocker.ps1"
$frames = if (Get-Variable RgdContractFrames -ErrorAction SilentlyContinue) { $RgdContractFrames } else { '' }
Invoke-JavaDocker -SourceRoot $WorkTree -StockJar $LsdJar -Dependencies $DepsDirectory `
    -OutputRoot $JavaWork -Image $JavaImageId -Action @('test','all') -Frames $frames -SkipBuild | ForEach-Object { Write-Host $_ }
$HostSuiteResults = @()
$seen = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $JavaWork 'host-tests.tsv')) {
    $parts = $line -split "`t", 2
    if ($parts.Count -ne 2 -or $parts[1] -notmatch 'PASS' -or $seen.ContainsKey($parts[0])) {
        throw "Invalid or duplicate Java test result: $line"
    }
    $seen[$parts[0]] = $true
    $HostSuiteResults += [ordered]@{ name = $parts[0]; result = $parts[1] }
}
$expectedCount = if ($frames) { 42 } else { 41 }
if ($HostSuiteResults.Count -ne $expectedCount) { throw "Expected $expectedCount Java suite results, got $($HostSuiteResults.Count)." }
$vcTextResult = @($HostSuiteResults | Where-Object { $_.name -eq 'VCTextScrollTest' })
if ($vcTextResult.Count -ne 1) { throw 'VC resource runtime test result is missing.' }
Set-Guard 'javaVcTextRuntimeLoad' $vcTextResult[0].result
Set-Guard 'javaHostSuites' ("PASS ($($HostSuiteResults.Count) suites against the supplied stock JAR)")
$auditSummary = (Get-Content -LiteralPath (Join-Path $JavaWork 'stock-linkage.txt') | Select-Object -Last 1)
if ($auditSummary -notmatch '^JavaStockLinkageAudit: .*errors=0$') { throw "Stock linkage audit failed: $auditSummary" }
Set-Guard 'javaStockLinkage' ("PASS ($($auditSummary.Substring('JavaStockLinkageAudit: '.Length)))")
