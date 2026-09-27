#requires -version 7.0
param(
    [Parameter(Mandatory=$true)][string]$PackageDirectory,
    [switch]$DispatcherOnly,
    # Optional source overlay in temporary fixtures only, for testing future builds.
    [string]$CollectorScript = '',
    [string]$DispatcherScript = '',
    [string]$TestRootParent = [IO.Path]::GetTempPath(),
    # Run every unit in sequence in this process (for debugging).
    [switch]$Sequential,
    [int]$ThrottleLimit = 0,
    # Internal: run one unit inside an existing fixture root (parallel children).
    [string]$Unit = '',
    [string]$TestRoot = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../PackageTools.ps1"
$sh = Find-GitSh (Get-Command git -ErrorAction Stop).Source
$utf8 = New-Object System.Text.UTF8Encoding($false)
if ($TestRoot) {
    $testRoot = $TestRoot
} else {
    $testRoot = Join-Path $TestRootParent ('carplay-rgi-test-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $testRoot | Out-Null
}
$script:checks = 0
function Assert($condition, $label) {
    if (-not $condition) { throw "FAIL: $label" }
    $script:checks++
    Write-Host "PASS: $label"
}
function WriteText($path, $text) { [IO.File]::WriteAllText($path, ($text -replace "`r`n", "`n"), $utf8) }
function Posix($path) {
    $full = [IO.Path]::GetFullPath($path).Replace('\','/')
    if ($full -match '^([A-Za-z]):(.*)$') { return '/' + $Matches[1].ToLowerInvariant() + $Matches[2] }
    return $full
}
function WriteCollector($root, $source, $destination) {
    $p = Posix $root
    $probeDirectory = Join-Path $root 'probes'
    New-Item -ItemType Directory -Force -Path $probeDirectory | Out-Null
    # Executable fixtures, so the watchdog kills the probe itself. No QNX
    # diagnostics are available on the Windows host; never query host services.
    $probeMock = @'
#!/bin/sh
echo "probe ${0##*/} $*" >> "$PROBE_TEST_ROOT/trace.txt"
case "${AUDIO_SCENARIO:-}:${0##*/}" in
    hangs:sloginfo)
        echo "partial audio diagnostic"
        echo "$$" > "$PROBE_TEST_ROOT/probe.pid"
        exec sleep 30
        ;;
    fails:sloginfo) echo "audio diagnostic unavailable"; exit 7 ;;
    empty:sloginfo) exit 0 ;;
esac
case "${0##*/}" in
    sloginfo)
        [ "$*" = "-t" ] || exit 99
        count=0
        [ ! -r "$PROBE_TEST_ROOT/slog-count" ] || count=$(cat "$PROBE_TEST_ROOT/slog-count")
        count=$((count + 1))
        echo "$count" > "$PROBE_TEST_ROOT/slog-count"
        echo "system snapshot $count"
        case "${AUDIO_SCENARIO:-}:$count" in
            live-fails:3) echo 'partial audio diagnostic'; exit 7 ;;
            live-hangs:3)
                echo 'partial audio diagnostic'
                echo "$$" > "$PROBE_TEST_ROOT/probe.pid"
                exec sleep 30
                ;;
        esac
        echo "00:05:12.345 io-audio: simulated underrun"
        echo "00:05:12.346 vendor event without audio keywords"
        ;;
    on)
        echo "on $*" >> "$PROBE_TEST_ROOT/trace.txt"
        if [ "$1:$2:$3:$4:$5" = '-d:-s:-f:mmx:/bin/sh' ]; then
            [ "${SCENARIO:-}" != worker-launch-fails ] || exit 7
            [ "${SCENARIO:-}" != worker-no-ack ] || exit 0
            shift 5
            /bin/sh "$@" </dev/null &
        else
            exit 99
        fi
        ;;
    pidin)
        if [ "${AUDIO_SCENARIO:-}" = live-interrupt ]; then
            echo 'partial thread diagnostic'
            echo "$$" > "$PROBE_TEST_ROOT/active-probe.pid"
            kill -TERM "$PPID"
            exec sleep 60
        fi
        echo "simulated pidin $*"
        ;;
    dmdt) echo "simulated dmdt $*" ;;
esac
'@
    foreach ($probe in @('sloginfo','on','pidin','dmdt')) { WriteText (Join-Path $probeDirectory $probe) $probeMock }
    $text = [IO.File]::ReadAllText($source).Replace("`r`n", "`n")
    $text = $text.Replace('/net/mmx/fs/sda0', "$p/sd").Replace('/tmp/carplay', "$p/tmp/carplay").
        Replace('/tmp/maneuver_render.log', "$p/tmp/maneuver_render.log").
        Replace('/mnt/app/', "$p/app/").Replace('/mnt/system/', "$p/system/").Replace('/eso/bin/apps/dmdt', "$p/probes/dmdt").
        Replace('/ramdisk/pps/', "$p/ramdisk/pps/").Replace("`nsleep 5 ", "`nsleep 0 ").
        Replace('sleep 10 ||', 'sleep 0 ||')
    # Accelerate healthy probe polling only. Keep the real five-second timeout
    # for hangs, and leave arming acknowledgements and cleanup waits unchanged.
    $text = $text.Replace("        sleep 1`n    done`n    # Check again", "        probe_poll_delay`n    done`n    # Check again")
    # This is after the no-argument launcher: it must use the launcher's own on
    # mock to reach MMX before these worker-only diagnostic mocks take effect.
    $probeSetup = @'
unset -f on
# Hold the detached worker until the caller has returned and tested its launch.
# A bounded gate also prevents an assertion failure leaving a fixture waiting.
sleep() {
    if [ "$1" = 30 ] && [ -n "${BACKGROUND_GATE:-}" ]; then
        for attempt in $(seq 1 200); do
            [ ! -e "$BACKGROUND_GATE" ] || return 0
            command sleep 0.1
        done
        return 1
    fi
    command sleep "$@"
}
probe_poll_delay() {
    case "${AUDIO_SCENARIO:-}:${PROBE_NAME:-}" in
        hangs:sloginfo-mmx.txt|live-hangs:sloginfo-live-03.txt) command sleep 1 ;;
        *) command sleep 0.2 ;;
    esac
}
rmdir() {
    if [ "${SCENARIO:-}" = lock-cleanup-fails ] && [ "${1##*/}" = .live-active ]; then return 1; fi
    command rmdir "$@"
}
command() {
    if [ "${AUDIO_SCENARIO:-}:$1" = 'missing:-v' ]; then
        case "$2" in sloginfo|on|pidin) return 1 ;; esac
    fi
    builtin command "$@"
}
'@
    $text = $text.Replace("`nexport PATH`n", "`nPATH='$p/probes':`$PATH`nexport PATH`nPROBE_TEST_ROOT='$p'`nexport PROBE_TEST_ROOT`n$probeSetup`n")
    # Same Windows sandbox mkdir workaround as the installer fixtures below.
    $mock = @'
mkdir() {
    if [ "$1" = -p ]; then
        shift
        for path in "$@"; do
            [ -d "$path" ] && continue
            command mkdir "$path" || return 1
        done
    else
        command mkdir "$@"
    fi
}
'@
    WriteText $destination ($text.Replace("#!/bin/sh`n", "#!/bin/sh`n$mock`necho `"collect `$0 `${1:-}`" >> '$p/trace.txt'`n"))
}
function Fixture($name) {
    $root = Join-Path $testRoot $name
    $sd = Join-Path $root 'sd'
    New-Item -ItemType Directory -Path $sd | Out-Null
    Copy-Item -LiteralPath (Join-Path $PackageDirectory 'sdcard\mod') -Destination $sd -Recurse
    if ($DispatcherScript) {
        WriteText (Join-Path $sd 'mod/command.sh') ([IO.File]::ReadAllText($DispatcherScript))
    }
    foreach ($dir in @('app/root','app/eso/hmi/lsd/jars','system/etc/eso/production','tmp','ifs')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $root $dir) | Out-Null
    }
    foreach ($name in @('smartphone_integrator','dio_manager')) {
        Copy-Item -LiteralPath (Join-Path $sd "mod/carplay-rgi/rollback/$name.stock.json") -Destination (Join-Path $root "system/etc/eso/production/$name.json")
    }
    # A small stand-in for the unit's HMI library; the fixture expects its identity.
    # The build guard proves the real installer carries the pinned lsd.jxe identity.
    $hmiLibrary = Join-Path $root 'ifs/lsd.jxe'
    WriteText $hmiLibrary "stand-in HMI library`n"
    $hmiIdentity = '{0}:{1}' -f (Get-PosixCksum $hmiLibrary), (Get-Item -LiteralPath $hmiLibrary).Length
    $p = Posix $root
    foreach ($name in @('install','rollback')) {
        $text = [IO.File]::ReadAllText((Join-Path $sd "mod/carplay-rgi/installer/$name.sh"))
        $text = $text.Replace('/net/mmx/fs/sda0', "$p/sd").Replace('/mnt/app', "$p/app").Replace('/mnt/system', "$p/system").Replace('/mnt/persist', "$p/persist").Replace('/ifs/lsd.jxe', "$p/ifs/lsd.jxe")
        $text = $text -replace '(?m)^HMI_LIBRARY_IDENTITY=\d+:\d+$', "HMI_LIBRARY_IDENTITY=$hmiIdentity"
        $mocks = @'
SYNC_COUNT=0
sync() {
    SYNC_COUNT=$((SYNC_COUNT + 1))
    # Lose the SD log at this sync: every later log line fails.
    if [ "$SYNC_COUNT" = "${LOSE_LOG_AT_SYNC:-0}" ]; then rm -f "$LOG" && mkdir "$LOG"; fi
    [ "${FAIL_SYNC:-0}" != 1 ]
}
sleep() { [ "${FAIL_SLEEP:-0}" != 1 ]; }
cp() { [ "${FAIL_CP:-0}" != 1 ] && command cp "$@"; }
# In the Codex Windows sandbox, Git Bash mkdir -p reopens protected ancestor
# directories even when they already exist. The fixture creates the parents;
# emulate QNX's idempotent -p for only those exact existing-parent targets.
mkdir() {
    if [ "$1" = -p ]; then
        shift
        for path in "$@"; do
            [ -d "$path" ] && continue
            command mkdir "$path" || return 1
        done
    else
        command mkdir "$@"
    fi
}
MV_COUNT=0
mv() {
    MV_COUNT=$((MV_COUNT + 1))
    [ "$MV_COUNT" != "${FAIL_MV:-0}" ] || return 1
    command mv "$@"
}
'@
        WriteText (Join-Path $root "$name.sh") ($text.Replace("#!/bin/sh`n", "#!/bin/sh`n$mocks`n"))
    }
    # The packaged log collector, its unit paths moved onto this fixture once for every
    # harness; the settle delay dropped; each run traced.
    $collector = Join-Path $sd 'mod/carplay-rgi/installer/collect-logs.sh'
    $source = if ($CollectorScript) { $CollectorScript } else { $collector }
    WriteCollector $root $source $collector
    return $root
}
function Run($root, $action, $fault='') {
    $text = ''
    if ($fault) { $text += "$fault`n" }
    $text += '. "' + (Posix (Join-Path $root "$action.sh")) + '"' + "`n"
    $runner = Join-Path $root 'runner.sh'
    WriteText $runner $text
    $output = & $sh -c 'PATH=/usr/bin:/bin:$PATH; bash "$1"' sh (Posix $runner) 2>&1
    $rc = $LASTEXITCODE
    WriteText (Join-Path $root "$action-last-output.txt") ($output -join "`n")
    return $rc
}
function IsStock($root) {
    foreach ($name in @('smartphone_integrator','dio_manager')) {
        $actual = (Get-FileHash (Join-Path $root "system/etc/eso/production/$name.json")).Hash
        $expected = (Get-FileHash (Join-Path $root "sd/mod/carplay-rgi/rollback/$name.stock.json")).Hash
        if ($actual -ne $expected) { return $false }
    }
    return $true
}
function Recovered($root) {
    Assert ((Run $root 'rollback') -eq 0) 'rollback succeeds'
    Assert (IsStock $root) 'both stock configurations restored byte-for-byte'
    Assert (-not (Test-Path (Join-Path $root 'app/eso/hmi/lsd/jars/carplay_hook.jar'))) 'introduced JAR removed'
    Assert (-not (Test-Path (Join-Path $root 'app/root/hooks/carplay_rgi_install.state'))) 'managed state removed'
    Assert (-not (Test-Path (Join-Path $root 'app/root/hooks/carplay_monitor.sh'))) 'runtime monitor removed'
}
# Install and recovery scenarios, each on its own simulated unit.
$normalBody = {
    $root = Fixture 'normal'
    Assert ((Run $root 'install') -eq 0) 'first install succeeds'
    Assert ((Run $root 'install') -eq 0) 'managed reinstall succeeds'
    Recovered $root
}

# The renderer's shader cache on the persist partition has no install record:
# rollback removes only its own names, never recursively, and never fails on it.
$shaderCacheBody = {
    param($case)
    $cacheNames = @('0123456789abcdef.bin', 'fedcba9876543210.bin', '0123456789abcdef.bin.4242')
    $root = Fixture $case
    Assert ((Run $root 'install') -eq 0) "$case install succeeds"
    $cache = Join-Path $root 'persist/var/app/luka_carplay_maneuver'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    foreach ($name in $cacheNames) { WriteText (Join-Path $cache $name) 'program binary' }
    if ($case -eq 'shader-cache-foreign') {
        foreach ($name in @('notes.txt', 'ABCDEF0123456789.bin', '0123456789abcde.bin')) { WriteText (Join-Path $cache $name) 'not the renderer''s' }
    }
    if ($case -eq 'shader-cache-stuck') { New-Item -ItemType Directory -Path (Join-Path $cache 'aaaaaaaaaaaaaaaa.bin') | Out-Null }
    Recovered $root
    $rollbackLog = [IO.File]::ReadAllText((Join-Path $root 'sd/mod/carplay-rgi-rollback.log'))
    foreach ($name in $cacheNames) { Assert (-not (Test-Path (Join-Path $cache $name))) "$case removes cache file $name" }
    if ($case -eq 'shader-cache') {
        Assert (-not (Test-Path $cache)) 'shader cache directory removed once empty'
    } else {
        Assert (Test-Path $cache) "$case leaves the cache directory in place"
        Assert ($rollbackLog.Contains('WARNING: ') -and $rollbackLog.Contains('SUCCESS:')) "$case warns without failing the rollback"
    }
    if ($case -eq 'shader-cache-foreign') {
        foreach ($name in @('notes.txt', 'ABCDEF0123456789.bin', '0123456789abcde.bin')) {
            Assert ([IO.File]::ReadAllText((Join-Path $cache $name)) -eq 'not the renderer''s') "foreign $name in the cache directory is preserved"
        }
    }
    if ($case -eq 'shader-cache-stuck') { Assert (Test-Path (Join-Path $cache 'aaaaaaaaaaaaaaaa.bin') -PathType Container) 'a directory with a cache name is never removed' }
}

$monitorUpgradeBody = {
    param($case)
    $root = Fixture $case
    Assert ((Run $root 'install') -eq 0) "$case initial managed install succeeds"
    $state = Join-Path $root 'app/root/hooks/carplay_rgi_install.state'
    WriteText $state (([IO.File]::ReadAllText($state)) -replace '(?m)^MONITOR=.*\n','')
    $monitor = Join-Path $root 'app/root/hooks/carplay_monitor.sh'
    if ($case -eq 'unowned-monitor') {
        WriteText $monitor 'unrecognised file'
        Assert ((Run $root 'install') -ne 0) 'old state cannot claim an unowned monitor'
        Assert ((Run $root 'rollback') -ne 0) 'rollback cannot remove an unowned monitor'
    } elseif ($case -eq 'upgrade') {
        Remove-Item -LiteralPath $monitor
        Assert ((Run $root 'install') -eq 0) 'older managed installation can acquire the new monitor'
        Recovered $root
    } else {
        # New payload committed, prior state still present: recovery must work.
        Recovered $root
    }
}

$installFaultBody = {
    param($fault)
    $root = Fixture $fault.Replace('=','-')
    Assert ((Run $root 'install' $fault) -ne 0) "$fault refused"
    Assert (IsStock $root) "$fault leaves stock configs intact"
    Assert (-not (Test-Path (Join-Path $root 'app/eso/hmi/lsd/jars/carplay_hook.jar'))) "$fault commits no JAR"
}

$payloadCorruptionBody = {
    $root = Fixture 'payload-corruption'
    Add-Content (Join-Path $root 'sd/mod/carplay-rgi/payload/hooks/carplay_startup.sh') 'corrupt'
    Assert ((Run $root 'install') -ne 0) 'corrupt payload refused'
    Assert (IsStock $root) 'corrupt payload leaves stock configs intact'
}

$navigationConflictBody = {
    $root = Fixture 'conflicting-navigation-patch'
    $conflict = Join-Path $root 'app/eso/hmi/lsd/jars/NavActiveIgnore.jar'
    WriteText $conflict 'owned by a different installer'
    Assert ((Run $root 'install') -ne 0) 'conflicting navigation patch is refused'
    Assert (([IO.File]::ReadAllText($conflict)) -eq 'owned by a different installer') 'unowned navigation patch is preserved'
    Assert (IsStock $root) 'navigation conflict leaves stock configs intact'
}

# Three backup renames precede the runtime files, two configs, then state.
# Fail each live commit independently and exercise recovery with the same package.
$renameBody = {
    param($n)
    $root = Fixture "rename-$n"
    Assert ((Run $root 'install' "FAIL_MV=$n") -ne 0) "rename $n failure propagates"
    Assert (Test-Path (Join-Path $root 'app/root/carplay-rgi-backup/owner-marker.txt')) "rename $n retains recovery ownership"
    Recovered $root
}

$unknownModificationBody = {
    $root = Fixture 'unknown-modification'
    Assert ((Run $root 'install') -eq 0) 'install before tamper succeeds'
    Add-Content (Join-Path $root 'app/root/hooks/carplay_startup.sh') 'unknown change'
    Assert ((Run $root 'rollback') -ne 0) 'rollback refuses an unrecognised modified target'
    Assert (Test-Path (Join-Path $root 'app/root/carplay-rgi-backup/owner-marker.txt')) 'refused rollback retains backups'
}

$rollbackRenameBody = {
    param($n)
    $root = Fixture "rollback-rename-$n"
    Assert ((Run $root 'install') -eq 0) "install before rollback failure $n succeeds"
    Assert ((Run $root 'rollback' "FAIL_MV=$n") -ne 0) "rollback rename $n failure propagates"
    Assert (Test-Path (Join-Path $root 'app/root/carplay-rgi-backup/owner-marker.txt')) "rollback rename $n preserves recovery material"
    Recovered $root
}

$settleFailsBody = {
    $root = Fixture 'settle-fails'
    Assert ((Run $root 'install' 'FAIL_SLEEP=1') -ne 0) 'settling failure is not reported as success'
    Recovered $root
}

# The SD log lost after every destination is staged (third sync): the installer
# must still remove what it staged and probed beside the firmware files.
$logLostBody = {
    $root = Fixture 'log-lost'
    Assert ((Run $root 'install' 'LOSE_LOG_AT_SYNC=3') -ne 0) 'install stops when its log can no longer be written'
    Assert ((Get-Content -Raw (Join-Path $root 'install-last-output.txt')).Contains('[RGI] ERROR! Cannot write')) 'a lost log is reported on the screen'
    $left = @(Get-ChildItem -LiteralPath (Join-Path $root 'app'), (Join-Path $root 'system') -Recurse -File |
        Where-Object { $_.Name -match '\.carplay-rgi-new\.|carplay-rgi-probe\.' })
    Assert ($left.Count -eq 0) "a lost log leaves no staged or probe file on the unit$(if ($left.Count) { ': ' + (@($left | ForEach-Object Name) -join ', ') })"
    Assert (IsStock $root) 'a lost log leaves stock configs intact'
    Assert (-not (Test-Path (Join-Path $root 'app/eso/hmi/lsd/jars/carplay_hook.jar'))) 'a lost log commits no JAR'
    Recovered $root
}

# The JAR must only reach the HMI library it was linked against (a firmware update
# can change it); rollback must still work when it has changed.
$hmiLibraryBody = {
    param($case)
    $root = Fixture "hmi-library-$case"
    $hmiLibrary = Join-Path $root 'ifs/lsd.jxe'
    $installLog = Join-Path $root 'sd/mod/carplay-rgi-install.log'
    $refused = {
        param($label)
        Assert ((Run $root 'install') -ne 0) "$label is refused"
        Assert (IsStock $root) "$label leaves stock configs intact"
        Assert (-not (Test-Path (Join-Path $root 'app/eso/hmi/lsd/jars/carplay_hook.jar'))) "$label installs no JAR"
        $shown = @(Get-Content (Join-Path $root 'install-last-output.txt') | Where-Object { $_ -match '\S' })
        Assert ($shown.Count -eq 1 -and $shown[0] -match '^\[RGI\] ERROR! ') "$label shows one [RGI] ERROR! line"
    }
    switch ($case) {
        mismatch {
            WriteText $hmiLibrary "a different firmware's HMI library`n"
            & $refused 'a different HMI library'
            Assert ((Get-Content -Raw $installLog).Contains('MISMATCH HMI library')) 'the install log records the HMI library found'
            Assert ((Get-Content -Raw (Join-Path $root 'install-last-output.txt')).Contains('not the')) 'the refusal names the HMI library mismatch'
        }
        cksum-differs {
            # Same size, other bytes: the checksum alone must refuse it.
            $original = [IO.File]::ReadAllText($hmiLibrary)
            WriteText $hmiLibrary $original.ToUpperInvariant()
            Assert ((Get-Item -LiteralPath $hmiLibrary).Length -eq $original.Length) 'the other HMI library has the same size'
            & $refused 'an HMI library of the same size'
            Assert ((Get-Content -Raw $installLog).Contains('MISMATCH HMI library')) 'the install log records the same-size HMI library found'
        }
        size-differs {
            # The expected checksum with another size: the byte count alone must refuse it.
            $install = Join-Path $root 'install.sh'
            $text = [IO.File]::ReadAllText($install)
            $changed = $text -replace '(?m)^(HMI_LIBRARY_IDENTITY=\d+):(\d+)$', { '{0}:{1}' -f $_.Groups[1].Value, ([long]$_.Groups[2].Value + 1) }
            Assert ($changed -ne $text) 'the installer now expects the same checksum at another size'
            WriteText $install $changed
            & $refused 'an HMI library of another size'
            Assert ((Get-Content -Raw $installLog).Contains('MISMATCH HMI library')) 'the install log records the HMI library size found'
        }
        missing {
            Remove-Item -LiteralPath $hmiLibrary
            & $refused 'a missing HMI library'
            Assert ((Get-Content -Raw $installLog).Contains('Cannot read this unit''s HMI library')) 'the install log records the unreadable HMI library'
        }
        changed {
            Assert ((Run $root 'install') -eq 0) 'install onto the linked HMI library succeeds'
            WriteText $hmiLibrary "HMI library after a firmware update`n"
            Assert ((Run $root 'install') -ne 0) 'reinstall after the HMI library changed is refused'
            Assert (Test-Path (Join-Path $root 'app/eso/hmi/lsd/jars/carplay_hook.jar')) 'a refused reinstall leaves the existing install untouched'
            Recovered $root
        }
    }
}
# Source command.sh exactly as M.I.B. does. Run its real MMX lock body in a
# subshell with a simulated SD; do not replace lock acquisition with success.
$dispatcherSource = if ($DispatcherScript) { $DispatcherScript } else { Join-Path $PackageDirectory 'sdcard/mod/custom.sh' }
$dispatcherTemplate = [IO.File]::ReadAllText($dispatcherSource)
$autoRollback = $dispatcherTemplate.Contains('Next run: Rollback')
$dispatcherScenarios = @('invalid-action','action-fails','lock-held',
    'lock-io-fails','readonly-fails','app-ro-fails','sd-rw-fails','app-rw-fails',
    'sys-rw-fails','release-fails','missing-utility','normal','rollback','rollback-slots-full',
    'rollback-collector-hangs')
if ($autoRollback) {
    $dispatcherScenarios += @('action-changed','action-stage-fails','action-rename-fails',
        'action-sync-fails','action-settle-fails','action-readback-fails','two-runs')
}
$dispatcherBody = {
    param($scenario)
    $root = Fixture "dispatcher-$scenario"
    $p = Posix $root
    $dispatcher = $dispatcherTemplate.Replace('/net/mmx/fs/sda0', "$p/sd")
    if ($scenario -eq 'rollback-collector-hangs') {
        # A probe that never returns; the 60 s watchdog shortened to 2 s for the test.
        Assert ($dispatcher.Contains('for ten in 1 2 3 4 5 6; do') -and $dispatcher.Contains('for second in 1 2 3 4 5 6 7 8 9 10; do')) 'dispatcher bounds the capture'
        $dispatcher = $dispatcher.Replace('for ten in 1 2 3 4 5 6; do', 'for ten in 1; do').Replace('for second in 1 2 3 4 5 6 7 8 9 10; do', 'for second in 1 2; do')
        WriteText (Join-Path $root 'sd/mod/carplay-rgi/installer/collect-logs.sh') "#!/bin/sh`necho `"collect `$0 `${1:-}`" >> '$p/trace.txt'`nsleep 30`n"
    }
    WriteText (Join-Path $root 'command.sh') $dispatcher
    # Live runtime state the collector must capture before a rollback.
    WriteText (Join-Path $root 'tmp/carplay_java.log') "java-log-$scenario`n"
    WriteText (Join-Path $root 'tmp/carplay_most_output') "0800 0252`n"
    WriteText (Join-Path $root 'tmp/carplay_most_frame.ppm') "P6`n800 252`n255`nframe-$scenario`n"
    if ($scenario -eq 'rollback-slots-full') {
        foreach ($slot in 1..9) {
            New-Item -ItemType Directory -Force -Path (Join-Path $root ('sd/mod/carplay-rgi-runtime-logs/0' + $slot)) | Out-Null
        }
    }
    # The retired "none" value exercises the same refusal as any invalid word.
    $action = switch ($scenario) { invalid-action {'none'} rollback {'rollback'} rollback-slots-full {'rollback'} rollback-collector-hangs {'rollback'} default {'install'} }
    WriteText (Join-Path $root 'sd/mod/carplay-rgi/ACTION') "$action`n"
    $lockPath = Join-Path $root 'sd/mod/carplay-rgi-install.lock'
    if ($scenario -eq 'lock-held') { New-Item -ItemType Directory -Path $lockPath | Out-Null }
    $mock = @'
# A file, not a variable: the dispatcher runs mount in a command substitution.
mount() {
    echo "mount $*" >> "$TRACE"
    case "$SCENARIO:$*" in
        sd-rw-fails:*sd|app-rw-fails:'-uw /net/mmx/mnt/app'|sys-rw-fails:'-uw /net/mmx/mnt/system'|\
        readonly-fails:'-ur /net/mmx/mnt/system'|app-ro-fails:'-ur /net/mmx/mnt/app')
            # Like the real tool, say why on stderr.
            echo "mount: simulated failure" >&2
            return 1
            ;;
    esac
    case "$*" in *sd) : > "$ROOT/sd-writable" ;; esac
    return 0
}
# This is a Bash-only test mock, never emitted into a QNX script.
command() {
    if [ "$SCENARIO:$1:$2" = 'missing-utility:-v:cksum' ]; then
        return 1
    fi
    builtin command "$@"
}
mkdir() {
    echo "lock-acquire $*" >> "$TRACE"
    case "$*" in */tmp/*) echo "mkdir: Function not implemented" >&2; return 1 ;; esac
    [ -e "$ROOT/sd-writable" ] || { echo "mkdir: SD is read-only" >&2; return 1; }
    [ "$SCENARIO" != lock-io-fails ] || { echo "mkdir: simulated I/O error" >&2; return 1; }
    command mkdir "$@" || return 1
    if [ "$SCENARIO" = action-changed ]; then
        echo rollback > "$ROOT/sd/mod/carplay-rgi/ACTION"
    fi
    return 0
}
chmod() { [ "$SCENARIO" != action-stage-fails ] && command chmod "$@"; }
mv() {
    echo "action-publish $*" >> "$TRACE"
    [ "$SCENARIO" != action-rename-fails ] && command mv "$@"
}
sync() { echo "action-sync" >> "$TRACE"; [ "$SCENARIO" != action-sync-fails ]; }
sleep() {
    if [ "$1" = 5 ]; then
        [ "$SCENARIO" != action-settle-fails ]
    else
        command sleep "$@"
    fi
}
cksum() {
    case "$SCENARIO:$1" in action-readback-fails:*/ACTION) return 1 ;; esac
    command cksum "$@"
}
rmdir() {
    echo "lock-release $*" >> "$TRACE"
    [ "$SCENARIO" != release-fails ] || { echo "rmdir: simulated failure" >&2; return 1; }
    command rmdir "$@"
}
on() {
    [ "$1:$2:$3" = '-f:mmx:/bin/sh' ] || return 99
    shift 3
    if [ "$1" = '-c' ]; then
        shift
        ( eval "$1" )
    else
        echo "action $1" >> "$TRACE"
        if [ "$SCENARIO" = two-runs ]; then
            /bin/sh "$ROOT/${1##*/}"
        else
            [ "$SCENARIO" != action-fails ]
        fi
    fi
}
. "$DISPATCHER"
result=$?
echo "RETURNED:$result" >> "$TRACE"
if [ "$SCENARIO" = two-runs ] && [ "$result" = 0 ]; then
    [ -f "$ROOT/app/eso/hmi/lsd/jars/carplay_hook.jar" ] && echo INSTALLED >> "$TRACE"
    . "$DISPATCHER"
    echo "RETURNED_AGAIN:$?" >> "$TRACE"
fi
exit 0
'@
    $prefix = "SCENARIO='$scenario'`nTRACE='$p/trace.txt'`nDISPATCHER='$p/command.sh'`nROOT='$p'`n"
    WriteText (Join-Path $root 'dispatcher-test.sh') ($prefix + $mock)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $output = & $sh -c 'PATH=/usr/bin:/bin:$PATH; bash "$1"' sh (Posix (Join-Path $root 'dispatcher-test.sh')) 2>&1
    $elapsed = $clock.Elapsed.TotalSeconds
    Assert ($LASTEXITCODE -eq 0) "$scenario returns to M.I.B. caller"
    $outputText = $output -join "`n"
    WriteText (Join-Path $root 'dispatcher-output.txt') $outputText
    $consoleNoise = @($output | ForEach-Object { $_.ToString() } | Where-Object { $_ -match '\S' -and $_ -notmatch '^\[RGI\] ' })
    Assert ($consoleNoise.Count -eq 0) "$scenario shows only [RGI] lines on the M.I.B. screen$(if ($consoleNoise.Count) { ': ' + ($consoleNoise -join ' | ') })"
    $resultLines = if ($scenario -eq 'two-runs') { 2 } else { 1 }
    Assert (@($output | Where-Object { $_ -match '^\[RGI\] (Success: 0|Failed: [1-9][0-9]*)$' }).Count -eq $resultLines) "$scenario reports one [RGI] result line per run"
    $screenLines = @($output | ForEach-Object { $_.ToString() } | Where-Object { $_ -match '^\[RGI\] ' })
    Assert ($screenLines.Count -and $screenLines[-1] -match '^\[RGI\] (Success: 0|Failed: [1-9][0-9]*)$') "$scenario shows its result as the last M.I.B. line"
    $trace = Get-Content -Raw (Join-Path $root 'trace.txt')
    $expected = if ($scenario -in @('normal','rollback','rollback-slots-full','rollback-collector-hangs','two-runs',
        'action-stage-fails','action-rename-fails','action-sync-fails','action-settle-fails','action-readback-fails')) {0} else {1}
    Assert ($trace.Contains("RETURNED:$expected")) "$scenario reports correct result"
    if ($scenario -eq 'invalid-action') {
        Assert (-not $trace.Contains('mount ')) "$scenario performs no remounts"
    }
    if ($scenario -in @('lock-held','lock-io-fails','sd-rw-fails','missing-utility')) {
        Assert (-not $trace.Contains('/net/mmx/mnt/')) "$scenario does not remount firmware"
    }
    if ($scenario -in @('invalid-action','lock-held','lock-io-fails','sd-rw-fails','missing-utility')) {
        Assert (-not $trace.Contains('action ')) "$scenario does not dispatch an action"
        Assert (-not $trace.Contains('lock-release ')) "$scenario does not release an unowned lock"
    }
    if ($scenario -eq 'lock-io-fails') {
        Assert ($outputText.Contains('simulated I/O error')) 'raw lock failure reaches M.I.B. output'
        Assert ($outputText.Contains('[RGI] ERROR! Could not create the SD lock: ')) 'I/O failure is not described as a held lock'
        Assert (-not $outputText.Contains('Another action holds the SD lock')) 'no false lock ownership diagnosis'
    }
    if ($scenario -eq 'missing-utility') {
        Assert ($outputText.Contains('[RGI] ERROR! Missing MMX tool: cksum')) 'missing command named before firmware remount'
    }
    if ($scenario -in @('sd-rw-fails','app-rw-fails','sys-rw-fails','readonly-fails','app-ro-fails')) {
        Assert ($outputText -match '\[RGI\] ERROR! [^\n]*: mount: simulated failure') "$scenario puts the mount error inside its [RGI] line"
    }
    if ($scenario -eq 'release-fails') {
        Assert ($outputText.Contains('[RGI] ERROR! Could not release the SD lock: rmdir: simulated failure.')) 'the lock release error is inside its [RGI] line'
    }
    if ($scenario -in @('normal','rollback','rollback-slots-full','rollback-collector-hangs','action-fails','readonly-fails','app-ro-fails')) {
        Assert ($trace.IndexOf('mount -uw ' + $p + '/sd') -lt $trace.IndexOf('lock-acquire ')) 'SD writable before lock'
        Assert ($trace.IndexOf('lock-acquire ') -lt $trace.IndexOf('mount -uw /net/mmx/mnt/app')) 'lock before firmware remount'
        Assert ($trace.Contains('mount -ur /net/mmx/mnt/system')) 'system read-only restoration attempted'
        Assert ($trace.Contains('mount -ur /net/mmx/mnt/app')) 'app read-only restoration attempted'
        Assert ($trace.IndexOf('mount -ur /net/mmx/mnt/app') -lt $trace.IndexOf('lock-release ')) 'release after restoration attempts'
    }
    if ($scenario -in @('app-rw-fails','sys-rw-fails')) {
        Assert (-not $trace.Contains('action ')) 'failed firmware remount prevents dispatch'
        Assert ($trace.Contains('lock-release ')) 'failed remount releases owned lock'
    }
    if ($scenario -eq 'sys-rw-fails') {
        Assert ($trace.Contains('mount -ur /net/mmx/mnt/app')) 'system remount failure still restores app'
    }
    Assert ((Test-Path $lockPath) -eq ($scenario -in @('lock-held','release-fails'))) "$scenario leaves expected lock ownership"
    if ($autoRollback) {
        $expectedAction = if ($scenario -in @('normal','two-runs','action-changed',
            'action-sync-fails','action-settle-fails','action-readback-fails')) { 'rollback' } else { $action }
        Assert ((Get-Content -Raw (Join-Path $root 'sd/mod/carplay-rgi/ACTION')) -ceq "$expectedAction`n") "$scenario leaves the correct next ACTION with LF"
        Assert (@(Get-ChildItem (Join-Path $root 'sd/mod/carplay-rgi') -Filter 'ACTION.next.*').Count -eq 0) "$scenario cleans its ACTION staging file"
        if ($scenario -eq 'normal') {
            Assert ($trace.IndexOf('action-publish ') -gt $trace.IndexOf('lock-release ')) 'ACTION changes only after successful lock release'
            Assert ($outputText.IndexOf('[RGI] Next run: Rollback') -lt $outputText.IndexOf('[RGI] Success: 0')) 'the ACTION change is reported before the result'
            Assert ($outputText.Contains('[RGI] Next run: Rollback')) 'success tells the operator the next run uninstalls'
        }
        # A stalled SD write in the ACTION save must not hide a completed install.
        $savingAt = $outputText.IndexOf('[RGI] Installed (result 0). Saving ACTION=rollback...')
        if ($expected -eq 0 -and $scenario -notlike 'rollback*') {
            $savedAt = [Math]::Max($outputText.IndexOf('[RGI] Next run: Rollback'), $outputText.IndexOf('[RGI] ERROR! Installed, but ACTION=rollback'))
            Assert ($savingAt -ge 0 -and $savingAt -lt $savedAt) "$scenario shows the install result before the ACTION save"
        } else {
            Assert ($savingAt -lt 0) "$scenario does not claim a completed install"
        }
        if ($scenario -like 'action-*-fails') {
            Assert ($outputText.Contains('[RGI] ERROR! Installed, but ACTION=rollback could not be saved')) 'ACTION publication failure is explicit'
            Assert ($outputText.Contains('[RGI] Success: 0') -and -not $outputText.Contains('[RGI] Failed')) 'setting failure does not rewrite the completed install result'
        }
        if ($expected -ne 0) {
            Assert (-not $trace.Contains('action-publish ')) "$scenario never publishes rollback after a failed action"
        }
        if ($scenario -eq 'action-changed') {
            Assert (-not $trace.Contains('/net/mmx/mnt/') -and -not $trace.Contains('action ')) 'stale ACTION is refused before firmware remounts'
        }
    }
    if ($scenario -in @('rollback','rollback-slots-full','rollback-collector-hangs')) {
        Assert ($trace.Contains('/installer/rollback.sh')) "$scenario selects rollback script"
        $collectAt = $trace.IndexOf('collect ')
        Assert ($collectAt -gt $trace.IndexOf('lock-acquire ') -and $collectAt -lt $trace.IndexOf('mount -uw /net/mmx/mnt/app')) "$scenario captures logs after the SD lock and before any firmware remount"
        Assert ($collectAt -lt $trace.IndexOf('action ')) "$scenario captures logs before the rollback runs"
        Assert ($trace.Contains('collect-logs.sh before-rollback')) "$scenario labels the capture before-rollback"
        Assert (([regex]::Matches($trace, 'collect ')).Count -eq 1) "$scenario captures once"
    } elseif ($scenario -ne 'two-runs') {
        Assert (-not $trace.Contains('collect ')) "$scenario does not run the log collector"
    }
    if ($scenario -eq 'two-runs') {
        Assert ($trace.Contains('INSTALLED') -and $trace.Contains('RETURNED_AGAIN:0')) 'first run installs and second run rolls back successfully'
        Assert ($trace.IndexOf('/installer/install.sh') -lt $trace.IndexOf('/installer/rollback.sh')) 'second invocation selects rollback without editing ACTION'
        Assert ($trace.IndexOf('collect ') -lt $trace.IndexOf('/installer/rollback.sh')) 'second invocation captures logs before uninstall'
        Assert (IsStock $root) 'second invocation restores both stock configurations'
        Assert (-not (Test-Path (Join-Path $root 'app/eso/hmi/lsd/jars/carplay_hook.jar'))) 'second invocation removes the installed JAR'
        Assert (-not (Test-Path (Join-Path $root 'app/root/hooks/carplay_rgi_install.state'))) 'second invocation removes managed state'
    }
    if ($scenario -eq 'rollback') {
        $capture = Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/01'
        Assert ((Get-Content -Raw (Join-Path $capture 'carplay_java.log')) -eq "java-log-rollback`n") 'rollback capture holds the live Java log'
        Assert ((Get-Content -Raw (Join-Path $capture 'carplay_most_output.txt')) -eq "0800 0252`n") 'rollback capture holds the renderer size request'
        Assert ((Get-Content -Raw (Join-Path $capture 'carplay_most_frame.ppm')) -eq "P6`n800 252`n255`nframe-rollback`n") 'rollback capture holds the renderer output frame'
        $summary = Get-Content -Raw (Join-Path $capture 'summary.txt')
        Assert ($summary.Contains('Capture: 01 (trigger: before-rollback)')) 'capture records its trigger'
        Assert ($summary.Contains('SUCCESS: runtime evidence copied')) 'capture completes before the rollback'
        Assert ((Get-Content -Raw (Join-Path $capture 'sloginfo-mmx.txt')).Contains('io-audio: simulated underrun') -and
            -not (Test-Path (Join-Path $capture 'sloginfo-rcc.txt')) -and
            (Get-Content -Raw (Join-Path $capture 'dmdt-gc.txt')).Contains('simulated dmdt gc') -and
            (Get-Content -Raw (Join-Path $capture 'dmdt-gs.txt')).Contains('simulated dmdt gs') -and
            (Get-Content -Raw (Join-Path $capture 'dmdt-gd.txt')).Contains('simulated dmdt gd')) 'rollback snapshot retains the system log once and the dmdt diagnostics'
        Assert ($outputText.Contains('[RGI] Logs saved to mod/carplay-rgi-runtime-logs')) 'M.I.B. output reports the capture'
    }
    if ($scenario -eq 'rollback-slots-full') {
        Assert ((Get-Content -Raw (Join-Path $root 'sd/mod/carplay-rgi-collect.out')).Contains('All 9 capture slots are full')) 'full capture slots are recorded in the capture output file'
        Assert ($outputText.Contains('[RGI] ERROR! Logs not saved or incomplete')) 'a capture that saved nothing is not reported as partly saved'
        Assert (-not (Test-Path (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/10'))) 'no capture slot beyond 09 is invented'
    }
    if ($scenario -eq 'rollback-collector-hangs') {
        Assert ((Get-Content -Raw (Join-Path $root 'sd/mod/carplay-rgi-collect.out')).Contains('did not finish within 60 s and was stopped')) 'a hung capture is stopped by the watchdog'
        Assert ($outputText.Contains('[RGI] ERROR! Log capture timed out after 60 s')) 'a stopped capture is flagged on the M.I.B. screen'
        Assert ($elapsed -lt 20) "a hung capture does not hold the rollback (dispatcher returned in $([int]$elapsed) s with a 30 s hang)"
    }
}
# A standalone SD contains only the shared script, with no package at all.
$standaloneScenarios = @('sourced','executed','lock-cleanup-fails','sd-rw-fails','slots-full','dispatch-fails','worker-launch-fails','worker-no-ack','invalid-argument')
$standaloneBody = {
    param($scenario)
    $root = Join-Path $testRoot "standalone-$scenario"
    foreach ($dir in @('sd/mod/carplay-rgi-runtime-logs','tmp')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $root $dir) | Out-Null
    }
    $p = Posix $root
    WriteCollector $root (Join-Path $PSScriptRoot '../../deploy/mib/collect-logs.sh') (Join-Path $root 'sd/mod/command.sh')
    WriteText (Join-Path $root 'tmp/carplay_java.log') "java-log-$scenario`n"
    if ($scenario -eq 'slots-full') {
        foreach ($slot in 1..9) {
            New-Item -ItemType Directory -Path (Join-Path $root "sd/mod/carplay-rgi-runtime-logs/0$slot") | Out-Null
        }
    }
    $mock = @'
mount() { echo "mount $*" >> "$TRACE"; [ "$SCENARIO" != sd-rw-fails ] || { echo "mount: simulated failure" >&2; return 1; }; }
on() {
    echo "on $*" >> "$TRACE"
    [ "$1:$2:$3" = '-f:mmx:/bin/sh' ] || return 99
    [ "$SCENARIO" != dispatch-fails ] || return 1
    shift 3
    ( PATH=/usr/bin:/bin; sh "$@" )
}
# Exported functions only simulate QNX tools for an executed script on the PC.
export -f mount on
BACKGROUND_GATE="${COLLECTOR%/sd/mod/command.sh}/release-worker"
if [ "$SCENARIO" = executed ]; then AUDIO_SCENARIO=live-fails; fi
export TRACE SCENARIO BACKGROUND_GATE AUDIO_SCENARIO
case "$SCENARIO" in
    executed) /bin/sh "$COLLECTOR" ;;
    invalid-argument) . "$COLLECTOR" unexpected ;;
    *) . "$COLLECTOR" ;;
esac
result=$?
echo "RETURNED:$result" >> "$TRACE"
if [ "$result" = 0 ]; then
    capture="${COLLECTOR%/command.sh}/carplay-rgi-runtime-logs/01"
    [ -f "$capture/STARTED" ] && [ ! -e "$capture/RESULT" ] && echo 'RETURNED-BEFORE-COMPLETION' >> "$TRACE"
    if [ "$SCENARIO" = sourced ]; then
        . "$COLLECTOR"
        echo "DUPLICATE:$?" >> "$TRACE"
        /bin/sh "$COLLECTOR" background 01 >> "$capture/duplicate-output.txt" 2>&1
        echo "DUPLICATE-WORKER:$?" >> "$TRACE"
        worker=$(cat "$capture/STARTED")
        kill -HUP "$worker"
        sleep 0.1
        kill -0 "$worker" 2>/dev/null && echo 'WORKER-SURVIVED-HUP' >> "$TRACE"
    fi
    echo release > "$BACKGROUND_GATE"
    for attempt in $(seq 1 1200); do
        if [ -f "$capture/RESULT" ]; then
            if [ "$SCENARIO" = lock-cleanup-fails ] || [ ! -e "${capture%/01}/.live-active" ]; then break; fi
        fi
        sleep 0.1
    done
fi
exit 0
'@
    $prefix = "SCENARIO='$scenario'`nTRACE='$p/trace.txt'`nCOLLECTOR='$p/sd/mod/command.sh'`n"
    WriteText (Join-Path $root 'collector-test.sh') ($prefix + $mock)
    $output = & $sh -c 'PATH=/usr/bin:/bin:$PATH; bash "$1"' sh (Posix (Join-Path $root 'collector-test.sh')) 2>&1
    Assert ($LASTEXITCODE -eq 0) "standalone $scenario returns to caller"
    $outputText = $output -join "`n"
    WriteText (Join-Path $root 'collector-output.txt') $outputText
    $screenLines = @($output | ForEach-Object { $_.ToString() } | Where-Object { $_ -match '^\[RGI\] ' })
    $resultAt = @(for ($i = 0; $i -lt $screenLines.Count; $i++) { if ($screenLines[$i] -match '^\[RGI\] (Success: 0|Failed: [1-9][0-9]*)$') { $i } })
    Assert ($resultAt.Count -eq 0 -or $resultAt[-1] -eq $screenLines.Count - 1) "standalone $scenario shows any result as the last M.I.B. line"
    $trace = Get-Content -Raw (Join-Path $root 'trace.txt')
    Assert (-not (Test-Path (Join-Path $root 'sd/mod/carplay-rgi'))) "standalone $scenario needs no package"
    Assert (-not $trace.Contains('/mnt/')) "standalone $scenario never remounts firmware"
    if ($scenario -in @('sourced','executed','lock-cleanup-fails')) {
        Assert ($trace.Contains('RETURNED:0') -and $outputText.Contains('[RGI] Capture 01 armed. Return to CarPlay now.') -and $outputText.Contains('[RGI] Success: 0')) "standalone $scenario reports successful arming"
        Assert ($trace.Contains('RETURNED-BEFORE-COMPLETION')) "standalone $scenario returns while its worker is still waiting"
        Assert ($trace.Contains("on -f mmx /bin/sh $p/sd/mod/command.sh arm") -and $trace.Contains("on -d -s -f mmx /bin/sh $p/sd/mod/command.sh background 01")) "standalone $scenario detaches its MMX worker into a new process group"
        $capture = Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/01'
        Assert ((Get-Content -Raw (Join-Path $capture 'carplay_java.log')) -eq "java-log-$scenario`n") 'standalone capture holds the live Java log'
        Assert ((Get-Content -Raw (Join-Path $capture 'summary.txt')).Contains('(trigger: background)')) 'standalone capture records its background trigger'
        Assert (-not $trace.Contains('probe on -f rcc') -and -not $trace.Contains('probe dmdt')) "standalone $scenario avoids disruptive remote and dmdt probes"
        $expected = if ($scenario -in @('executed','lock-cleanup-fails')) { '1' } else { '0' }
        Assert ((Get-Content -Raw (Join-Path $capture 'RESULT')).Trim() -eq $expected) "standalone $scenario saves its eventual collection result"
        if ($scenario -eq 'lock-cleanup-fails') {
            Assert ((Test-Path (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/.live-active')) -and
                (Get-Content -Raw (Join-Path $capture 'summary.txt')).Contains('ERROR: live reservation cleanup or its flush failed')) 'reservation cleanup failure remains visible and prevents a successful result'
        } else {
            Assert (-not (Test-Path (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/.live-active'))) "standalone $scenario releases its active lock after completion"
        }
        if ($scenario -eq 'sourced') {
            Assert ($trace.Contains('DUPLICATE:1') -and $outputText.Contains('[RGI] ERROR! A capture is already running or awaiting inspection')) 'a second sourced launch refuses an active capture'
            Assert (-not (Test-Path (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/02'))) 'a refused duplicate does not reserve another slot'
            Assert ($trace.Contains('DUPLICATE-WORKER:1')) 'a second worker cannot reopen the reserved capture'
            Assert ($trace.Contains('WORKER-SURVIVED-HUP')) 'the detached worker survives hangup after arming'
        }
    } else {
        Assert ($trace.Contains('RETURNED:1')) "standalone $scenario propagates failure"
        if ($scenario -ne 'worker-launch-fails') {
            Assert (-not (Test-Path (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/01/RESULT'))) "standalone $scenario claims no completed capture"
        }
        Assert (-not $outputText.Contains('armed. Return to CarPlay')) "standalone $scenario does not claim successful arming"
        if ($scenario -in @('sd-rw-fails','invalid-argument')) {
            Assert (-not $trace.Contains('on -f')) "standalone $scenario does not dispatch"
        }
        if ($scenario -eq 'sd-rw-fails') {
            Assert ($outputText.Contains('[RGI] ERROR! Could not make the SD card writable: mount: simulated failure.')) 'standalone puts the mount error inside its [RGI] line'
        }
        if ($scenario -eq 'slots-full') {
            Assert ($outputText.Contains('[RGI] ERROR! All 9 capture slots are full')) 'standalone explains full slots'
            Assert (-not (Test-Path (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/.live-active'))) 'full capture slots do not leave a reservation lock'
        }
        if ($scenario -eq 'worker-launch-fails') {
            Assert ((Get-Content -Raw (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/01/RESULT')).Trim() -eq '1') 'a failed detached launch records failure'
            Assert (-not (Test-Path (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/.live-active'))) 'a failed detached launch releases its reservation lock'
        }
        if ($scenario -eq 'worker-no-ack') {
            Assert ((Test-Path (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/.live-active')) -and
                -not (Test-Path (Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/01/RESULT'))) 'an unacknowledged worker retains its lock without claiming completion'
        }
    }
}
# Audio diagnostics: success, empty buffers, missing tools, errors and a hung
# reader. These run the shared worker directly, just as the package invokes it.
$audioScenarios = @('normal','empty','missing','fails','hangs','live-normal','live-fails','live-hangs')
$audioBody = {
    param($scenario)
    $root = Join-Path $testRoot "audio-$scenario"
    foreach ($dir in @('sd/mod/carplay-rgi-runtime-logs','tmp')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $root $dir) | Out-Null
    }
    $p = Posix $root
    $collector = Join-Path $root 'sd/mod/command.sh'
    WriteCollector $root (Join-Path $PSScriptRoot '../../deploy/mib/collect-logs.sh') $collector
    WriteText (Join-Path $root 'tmp/carplay_java.log') "retained-java-log`n"
    $runner = Join-Path $root 'audio-test.sh'
    $mode = if ($scenario.StartsWith('live-')) { 'live' } else { 'manual' }
    WriteText $runner "AUDIO_SCENARIO='$scenario'`nexport AUDIO_SCENARIO`n/bin/sh '$p/sd/mod/command.sh' $mode`n"
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $output = & $sh -c 'PATH=/usr/bin:/bin:$PATH; bash "$1"' sh (Posix $runner) 2>&1
    $rc = $LASTEXITCODE
    $elapsed = $clock.Elapsed.TotalSeconds
    WriteText (Join-Path $root 'audio-output.txt') ($output -join "`n")
    $expected = if ($scenario -in @('fails','hangs','live-fails','live-hangs')) { 1 } else { 0 }
    Assert ($rc -eq $expected) "audio $scenario returns the correct collection status"
    $capture = Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/01'
    $summary = Get-Content -Raw (Join-Path $capture 'summary.txt')
    $trace = Get-Content -Raw (Join-Path $root 'trace.txt')
    if ($mode -eq 'live') {
        $snapshotChecks = foreach ($round in @('01','02','03','04','05','06')) {
            (Get-Content -Raw (Join-Path $capture "sloginfo-live-$round.txt")).Contains("system snapshot $([int]$round)")
            $summary.Contains("CAPTURED sloginfo-live-$round.txt (")
        }
        Assert ($snapshotChecks -notcontains $false) "$scenario retains and checksums all six distinct system snapshots"
        $sampleChecks = foreach ($round in @('01','02','03','04','05','06')) {
            foreach ($process in @('io-audio','audio_service','maneuver_render')) {
                (Get-Content -Raw (Join-Path $capture "live-$round-$process-ttimes.txt")).Contains("-p $process ttimes")
                (Get-Content -Raw (Join-Path $capture "live-$round-$process-sched.txt")).Contains("-p $process sched")
            }
        }
        Assert ($sampleChecks -notcontains $false) "$scenario preserves all six rounds of audio and renderer samples"
        Assert (-not $trace.Contains('probe dmdt') -and $summary.Contains('SKIPPED live dmdt queries')) "$scenario skips disruptive diagnostics and explains the omissions"
    } else {
        Assert (-not (Test-Path (Join-Path $capture 'sloginfo-live-01.txt'))) "$scenario snapshot does not start live observation"
    }
    Assert ((Get-Content -Raw (Join-Path $capture 'carplay_java.log')) -eq "retained-java-log`n") "audio $scenario preserves the existing runtime capture"
    if ($scenario -eq 'missing') {
        Assert ($summary.Contains('MMX system/audio messages unavailable')) 'missing audio tools are explicitly reported'
        Assert (-not (Test-Path (Join-Path $capture 'sloginfo-mmx.txt'))) 'missing tool produces no misleading empty audio log'
    } else {
        Assert (-not (Test-Path (Join-Path $capture 'sloginfo-rcc.txt')) -and -not $trace.Contains('on -f rcc')) "audio $scenario reads the shared system log once"
        foreach ($process in @('io-audio','audio_service','maneuver_render')) {
            Assert ((Get-Content -Raw (Join-Path $capture "$process-sched.txt")).Contains("-p $process sched")) "audio $scenario captures $process scheduling"
        }
        Assert ($summary.Contains('CAPTURED sloginfo-mmx.txt (')) "audio $scenario records a checksum even for empty or partial output"
    }
    switch ($scenario) {
        normal {
            $slog = Get-Content -Raw (Join-Path $capture 'sloginfo-mmx.txt')
            Assert ($slog.Contains('00:05:12.345 io-audio: simulated underrun')) 'MMX log preserves audio events and millisecond timestamps'
            Assert ($slog.Contains('vendor event without audio keywords')) 'system log is not filtered by audio keywords'
            $dmdtChecks = foreach ($query in @('gc','gs','gd')) {
                (Get-Content -Raw (Join-Path $capture "dmdt-$query.txt")).Contains("simulated dmdt $query")
            }
            Assert ($dmdtChecks -notcontains $false) 'manual snapshot retains all three dmdt queries'
        }
        empty {
            Assert ((Get-Item (Join-Path $capture 'sloginfo-mmx.txt')).Length -eq 0) 'an empty system buffer is allowed'
            Assert ($summary.Contains('PROBE sloginfo-mmx.txt exit status: 0')) 'empty buffer is distinguished from failed capture'
        }
        fails {
            Assert ($summary.Contains('PROBE sloginfo-mmx.txt exit status: 7') -and $summary.Contains('PARTIAL:')) 'audio command failure is visible and yields a partial capture'
        }
        hangs {
            Assert ($summary.Contains('sloginfo-mmx.txt timed out after 5 s') -and $summary.Contains('PARTIAL:')) 'hung audio reader is bounded and reported'
            Assert ((Get-Content -Raw (Join-Path $capture 'sloginfo-mmx.txt')).Contains('partial audio diagnostic')) 'timeout retains diagnostic output already written'
            Assert ($elapsed -lt 20) 'hung audio reader does not hold collection for its 30-second hang'
            $probePid = (Get-Content -Raw (Join-Path $root 'probe.pid')).Trim()
            & $sh -c 'kill -0 "$1" 2>/dev/null' sh $probePid
            Assert ($LASTEXITCODE -ne 0) 'timed-out audio probe no longer runs'
        }
        live-normal {
            Assert ($summary.Contains('SUCCESS:')) 'six-round live observation completes successfully'
        }
        live-fails {
            Assert ($summary.Contains('PROBE sloginfo-live-03.txt exit status: 7') -and $summary.Contains('PARTIAL:')) 'failed live snapshot yields a partial result'
            Assert ((Get-Content -Raw (Join-Path $capture 'sloginfo-live-03.txt')).Contains('partial audio diagnostic') -and
                $summary.Contains('PROBE sloginfo-live-06.txt exit status: 0')) 'failed live snapshot preserves its output and later rounds continue'
        }
        live-hangs {
            Assert ($summary.Contains('sloginfo-live-03.txt timed out after 5 s') -and $summary.Contains('PARTIAL:')) 'hung live snapshot is bounded and yields a partial result'
            Assert ((Get-Content -Raw (Join-Path $capture 'sloginfo-live-03.txt')).Contains('partial audio diagnostic') -and
                $summary.Contains('PROBE sloginfo-live-06.txt exit status: 0')) 'hung live snapshot preserves its output and later rounds continue'
            Assert ($elapsed -lt 30) 'hung live snapshot does not hold observation for its full hang'
            $probePid = (Get-Content -Raw (Join-Path $root 'probe.pid')).Trim()
            & $sh -c 'kill -0 "$1" 2>/dev/null' sh $probePid
            Assert ($LASTEXITCODE -ne 0) 'timed-out live snapshot probe no longer runs'
        }
    }
}
# Interrupt a bounded query after the first system snapshot. The worker must
# return nonzero, retain partial evidence and reap its own active probe.
$interruptBody = {
$root = Join-Path $testRoot 'live-interrupted'
foreach ($dir in @('sd/mod','tmp')) { New-Item -ItemType Directory -Force -Path (Join-Path $root $dir) | Out-Null }
$p = Posix $root
$collector = Join-Path $root 'sd/mod/command.sh'
WriteCollector $root (Join-Path $PSScriptRoot '../../deploy/mib/collect-logs.sh') $collector
$runner = Join-Path $root 'interrupt-test.sh'
WriteText $runner "AUDIO_SCENARIO=live-interrupt`nexport AUDIO_SCENARIO`n/bin/sh '$p/sd/mod/command.sh' live`n"
$output = & $sh -c 'PATH=/usr/bin:/bin:$PATH; bash "$1"' sh (Posix $runner) 2>&1
Assert ($LASTEXITCODE -eq 1) 'interrupted live observation returns failure'
WriteText (Join-Path $root 'interrupt-output.txt') ($output -join "`n")
$readerPid = (Get-Content -Raw (Join-Path $root 'active-probe.pid')).Trim()
& $sh -c 'kill -0 "$1" 2>/dev/null' sh $readerPid
Assert ($LASTEXITCODE -ne 0) 'interruption reaps the active query'
$capture = Join-Path $root 'sd/mod/carplay-rgi-runtime-logs/01'
Assert ((Get-Content -Raw (Join-Path $capture 'sloginfo-live-01.txt')).Contains('system snapshot 1') -and
    (Get-Content -Raw (Join-Path $capture 'live-01-io-audio-sched.txt')).Contains('partial thread diagnostic')) 'interruption preserves the preceding snapshot and partial active query output'
}

# Every scenario is a unit with its own fixture under $testRoot, so units can run in
# parallel. Phase 2 holds the scenarios that assert elapsed time; they run after the
# others, together, so a loaded machine cannot fail them.
$units = New-Object System.Collections.Generic.List[object]
function Add-Unit([string]$Name, [scriptblock]$Body, $Argument = $null, [int]$Phase = 1) {
    $units.Add([pscustomobject]@{ Name = $Name; Body = $Body; Argument = $Argument; Phase = $Phase })
}
if (-not $DispatcherOnly) {
    Add-Unit 'normal' $normalBody
    foreach ($case in @('shader-cache','shader-cache-foreign','shader-cache-stuck')) { Add-Unit $case $shaderCacheBody $case }
    foreach ($case in @('upgrade','interrupted-upgrade','unowned-monitor')) { Add-Unit $case $monitorUpgradeBody $case }
    foreach ($fault in @('FAIL_SYNC=1','FAIL_CP=1')) { Add-Unit $fault.Replace('=','-') $installFaultBody $fault }
    Add-Unit 'payload-corruption' $payloadCorruptionBody
    Add-Unit 'conflicting-navigation-patch' $navigationConflictBody
    $plan = Get-Content -Raw -LiteralPath (Join-Path $PackageDirectory 'sdcard/mod/carplay-rgi/meta/deployment-plan.json') | ConvertFrom-Json
    foreach ($n in 4..(3 + @($plan.files).Count)) { Add-Unit "rename-$n" $renameBody $n }
    Add-Unit 'unknown-modification' $unknownModificationBody
    foreach ($n in 1..2) { Add-Unit "rollback-rename-$n" $rollbackRenameBody $n }
    Add-Unit 'settle-fails' $settleFailsBody
    Add-Unit 'log-lost' $logLostBody
    foreach ($case in @('mismatch','cksum-differs','size-differs','missing','changed')) { Add-Unit "hmi-library-$case" $hmiLibraryBody $case }
}
foreach ($scenario in $dispatcherScenarios) {
    Add-Unit "dispatcher-$scenario" $dispatcherBody $scenario $(if ($scenario -eq 'rollback-collector-hangs') { 2 } else { 1 })
}
foreach ($scenario in $standaloneScenarios) { Add-Unit "standalone-$scenario" $standaloneBody $scenario }
foreach ($scenario in $audioScenarios) {
    Add-Unit "audio-$scenario" $audioBody $scenario $(if ($scenario -in @('hangs','live-hangs')) { 2 } else { 1 })
}
Add-Unit 'live-interrupted' $interruptBody

function Invoke-Unit($entry) {
    if ($null -eq $entry.Argument) { & $entry.Body } else { & $entry.Body $entry.Argument }
}

# Child mode: one unit, in its own process, reporting its own checks.
if ($Unit) {
    $selected = @($units | Where-Object { $_.Name -ceq $Unit })
    if ($selected.Count -ne 1) { throw "Unknown installer test unit: $Unit" }
    Invoke-Unit $selected[0]
    Write-Host "UNIT-CHECKS: $script:checks"
    exit 0
}

# The source is identical in every fixture: check this static invariant once.
Assert (-not $dispatcherTemplate.Contains('/tmp/carplay-rgi-install.lock')) 'dispatcher does not lock on QNX RAM filesystem'

if ($Sequential) {
    foreach ($item in $units) { Invoke-Unit $item }
    Write-Host "All $script:checks assertions passed. Fixtures: $testRoot"
    return
}

if ($ThrottleLimit -lt 1) { $ThrottleLimit = [Math]::Max(2, [Environment]::ProcessorCount) }
$pwshPath = (Get-Process -Id $PID).Path
function Quote-Literal([string]$Text) { "'" + $Text.Replace("'", "''") + "'" }
$unitCall = '& ' + (Quote-Literal $PSCommandPath) + ' -PackageDirectory ' + (Quote-Literal $PackageDirectory) +
    ' -TestRoot ' + (Quote-Literal $testRoot) + ' -TestRootParent ' + (Quote-Literal $TestRootParent)
if ($DispatcherOnly) { $unitCall += ' -DispatcherOnly' }
if ($CollectorScript) { $unitCall += ' -CollectorScript ' + (Quote-Literal $CollectorScript) }
if ($DispatcherScript) { $unitCall += ' -DispatcherScript ' + (Quote-Literal $DispatcherScript) }
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
# The slowest units start first so they do not finish last.
$slowFirst = @('audio-live-fails','audio-live-normal','dispatcher-two-runs','standalone-sourced','standalone-executed',
    'normal','upgrade','dispatcher-action-readback-fails','rollback-rename-1','rollback-rename-2','audio-fails')
$results = @{}
foreach ($phase in @(1, 2)) {
    $batch = @($units | Where-Object { $_.Phase -eq $phase } |
        Sort-Object @{ Expression = { $i = [Array]::IndexOf($slowFirst, $_.Name); if ($i -lt 0) { 999 } else { $i } } })
    # Each unit runs through Invoke-IsolatedProcess, in its own hidden console.  Units run Git
    # Bash, and Git Bash trees of concurrent units on one console can deadlock in the MSYS2
    # runtime (msys-2.0.dll 3.6.10): on 27 September a unit's bash waited for good on the shared
    # console (see Build-Snapshot's native steps).  The unit writes every stream to its own
    # file, read after it exits (inside a package build asynchronous reads came back empty for
    # 10 units).  A unit still running after 10 minutes (the slowest takes ~25 s) is killed and
    # fails by name with the output it had written.
    $batch | ForEach-Object -ThrottleLimit $ThrottleLimit -Parallel {
        . "$using:PSScriptRoot/../PackageTools.ps1"
        $unitName = $_.Name
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $outFile = Join-Path $using:testRoot ($unitName + '.unit-output.txt')
        $out = "'" + $outFile.Replace("'", "''") + "'"
        $command = 'try { ' + $using:unitCall + " -Unit '" + $unitName + "' *> " + $out + '; exit $LASTEXITCODE } ' +
            'catch { $_ | Out-String | Add-Content -LiteralPath ' + $out + '; exit 1 }'
        $run = Invoke-IsolatedProcess -Exe $using:pwshPath -OutputFile $outFile -TimeoutMs (10 * 60 * 1000) `
            -Arguments @('-NoProfile', '-NonInteractive', '-Command', $command)
        $output = $run.Output
        if ($run.TimedOut) { $output += 'Installer test unit timed out after 10 minutes and was killed.' }
        [pscustomobject]@{ Name = $unitName; ExitCode = $run.ExitCode; Output = $output; Seconds = $clock.Elapsed.TotalSeconds }
    } | ForEach-Object { $results[$_.Name] = $_ }
}

# Report in the fixed unit order, one block per unit.
$failed = New-Object System.Collections.Generic.List[string]
foreach ($item in $units) {
    $result = $results[$item.Name]
    Write-Host ('--- {0} ({1:N1} s)' -f $item.Name, $result.Seconds)
    $checksLine = @($result.Output | Where-Object { $_ -match '^UNIT-CHECKS: \d+$' })
    foreach ($line in $result.Output) { if ($line -notmatch '^UNIT-CHECKS: ') { Write-Host $line } }
    if ($result.ExitCode -ne 0 -or $checksLine.Count -ne 1) { $failed.Add($item.Name); continue }
    $unitChecks = [int]($checksLine[0] -replace '^UNIT-CHECKS: ', '')
    # A unit that asserted nothing (for example an unmatched scenario) has not passed.
    if ($unitChecks -lt 1) { $failed.Add("$($item.Name) (no assertions)"); continue }
    $script:checks += $unitChecks
}
if ($failed.Count) { throw "Installer test units failed: $($failed -join ', '). Fixtures: $testRoot" }
Write-Host "All $script:checks assertions passed in $($units.Count) parallel units. Fixtures: $testRoot"
