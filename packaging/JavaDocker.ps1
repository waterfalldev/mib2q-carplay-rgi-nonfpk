# PowerShell adapter only. Compilation and tests live in scripts/java/.
function Resolve-JavaImage {
    param([string]$Image = 'eclipse-temurin:8-jdk-jammy')
    $docker = (Get-Command docker -ErrorAction Stop).Source
    & $docker image inspect $Image *> $null
    if ($LASTEXITCODE -ne 0) { Invoke-Native -Exe $docker -Arguments @('pull', $Image) | ForEach-Object { Write-Host $_ } }
    $id = @(Invoke-Native -Exe $docker -Arguments @('image','inspect','--format','{{.Id}}',$Image) -Capture)[0].Trim()
    if ($id -notmatch '^sha256:[0-9a-f]{64}$') { throw 'Java Docker image did not resolve to an immutable ID.' }
    return $id
}

function Invoke-JavaDocker {
    param(
        [string]$SourceRoot, [string]$StockJar, [string]$Dependencies,
        [string]$OutputRoot, [string]$Image, [string]$BuildId = 'host-check',
        [string[]]$Action = @('build'), [string]$Frames = '', [switch]$SkipBuild,
        [switch]$Capture
    )
    $settings = @{
        STOCK_JAR = [IO.Path]::GetFullPath($StockJar)
        CARPLAY_DEPENDENCIES = [IO.Path]::GetFullPath($Dependencies)
        JAVA_OUTPUT = [IO.Path]::GetFullPath($OutputRoot)
        ASM_JAR = Join-Path ([IO.Path]::GetFullPath($Dependencies)) 'asm-9.7.jar'
        ASM_TREE_JAR = Join-Path ([IO.Path]::GetFullPath($Dependencies)) 'asm-tree-9.7.jar'
        CARPLAY_JAVA_IMAGE = $Image
        CARPLAY_BUILD_ID = $BuildId
        JAVA_SKIP_BUILD = $(if ($SkipBuild) { '1' } else { '0' })
        RGD_CONTRACT_FRAMES = $Frames
        # Firmware packages use the verified combined JAR, never inherited shell overrides.
        STOCK_BOOT_JAR = ''; STOCK_RUNTIME_JAR = ''; CARPLAY_HOOK_JAR = ''; VC_UNICODE_TEST_DIR = ''
    }
    # The settings reach the launcher through env, never this process's environment: processes
    # Build-Snapshot's native job starts meanwhile would inherit them.  Empty means unset (env
    # takes every -u before the first assignment).
    $unset = @($settings.Keys | Where-Object { -not $settings[$_] } | ForEach-Object { '-u'; $_ })
    $assign = @($settings.Keys | Where-Object { $settings[$_] } | ForEach-Object { "$_=$($settings[$_])" })
    $git = (Get-Command git -ErrorAction Stop).Source
    $shell = Find-GitSh $git
    $launcher = (Join-Path $SourceRoot 'scripts/java/docker.sh').Replace('\','/')
    Invoke-Native -Exe $shell -Arguments (@('-c', 'exec env "$@"', 'sh') + $unset + $assign + @('sh', $launcher) + $Action) -Capture:$Capture
}
