# Internal staging step; invoked by Build-Snapshot.ps1.
    # Construct staged SD-card tree

    Write-Step 'Creating SD-card staging tree'

    $SdRoot = Join-Path $ReleaseDirectory 'sdcard'

    $PackageRoot = Join-Path `
        $SdRoot `
        'mod\carplay-rgi'

    $HooksDirectory = Join-Path $PackageRoot 'payload\hooks'
    $JarsDirectory = Join-Path $PackageRoot 'payload\jars'
    $ConfigDirectory = Join-Path $PackageRoot 'payload\config'
    $RollbackDirectory = Join-Path $PackageRoot 'rollback'
    $InstallerDirectory = Join-Path $PackageRoot 'installer'
    $MetaDirectory = Join-Path $PackageRoot 'meta'

    New-Item `
        -ItemType Directory `
        -Force `
        -Path `
            $HooksDirectory,
            $JarsDirectory,
            $ConfigDirectory,
            $RollbackDirectory,
            $InstallerDirectory,
            $MetaDirectory |
        Out-Null

    . "$PSScriptRoot/Payloads.ps1"
    foreach ($target in @($InstallerTargets | Where-Object { $_.Action -eq 'add' })) {
        $inputPath = if ($target.Key -eq 'JAR') { $BuiltJavaJar } else { Join-Path $WorkTree $target.RepositorySource }
        $destination = Join-Path $PackageRoot $target.Source
        Copy-Item -LiteralPath $inputPath -Destination $destination
        if ($destination.EndsWith('.sh')) { Assert-LfOnly -Path $destination -Description $target.Key }
    }
    Set-Guard 'qnxTextLf'

    $AtlasProvenance = [ordered]@{
        source = 'pinned source tree'
        repositoryPath = 'maneuver_render/resources/flag_atlas.rgba'
        commit = $Commit
        sha256 = Get-Sha256 $FlagAtlasSource
    }

    # Patch copies of the stock config files

    Write-Step 'Generating patched configuration copies'

    $PatchedSmartphone =
        Join-Path $ConfigDirectory 'smartphone_integrator.json'

    $PatchedDio =
        Join-Path $ConfigDirectory 'dio_manager.json'

    Set-CarPlayChildBlock `
        -StockPath $StockSmartphone `
        -ChildPath $CarPlayChildPath `
        -Destination $PatchedSmartphone

    Add-MessageIds `
        -StockPath $StockDio `
        -Destination $PatchedDio `
        -SentIds $RgiSentIds `
        -ReceivedIds $RgiReceivedIds

    # The upstream child was already checked for the required launcher paths.
    # Validate exact child equality, forbidden preload, message counts and the
    # unchanged remainder of both stock documents in one place.
    Assert-ConfigPatchScope `
        -StockSmartphone $StockSmartphone `
        -PatchedSmartphone $PatchedSmartphone `
        -UpstreamChild $CarPlayChildPath `
        -StockDio $StockDio `
        -PatchedDio $PatchedDio `
        -SentIds $RgiSentIds `
        -ReceivedIds $RgiReceivedIds

    Set-Guard 'configPatchScope'

    # Keep exact originals with the package for future rollback.
    Copy-Item `
        -LiteralPath $StockSmartphone `
        -Destination (Join-Path $RollbackDirectory 'smartphone_integrator.stock.json')

    Copy-Item `
        -LiteralPath $StockDio `
        -Destination (Join-Path $RollbackDirectory 'dio_manager.stock.json')

    # Keep upstream child definition for audit/reference.
    Copy-Item `
        -LiteralPath $CarPlayChildPath `
        -Destination (Join-Path $MetaDirectory 'upstream_carplay_child.json')

    $RollbackSmartphone =
        Join-Path $RollbackDirectory 'smartphone_integrator.stock.json'
    $RollbackDio =
        Join-Path $RollbackDirectory 'dio_manager.stock.json'

    Assert-Sha256 `
        -Path $RollbackSmartphone `
        -Expected $ExpectedStockSmartphoneSha256 `
        -Description 'rollback smartphone_integrator.json'
    Assert-Sha256 `
        -Path $RollbackDio `
        -Expected $ExpectedStockDioSha256 `
        -Description 'rollback dio_manager.json'

    $ConfigPatchPlan = [ordered]@{
        schemaVersion = 1
        smartphoneIntegrator = [ordered]@{
            source = [ordered]@{
                bytes = $ExpectedStockSmartphoneSize
                posixCksum = $ExpectedStockSmartphoneCksum
                sha256 = $ExpectedStockSmartphoneSha256
            }
            prepared = [ordered]@{
                bytes = (Get-Item -LiteralPath $PatchedSmartphone).Length
                posixCksum = Get-PosixCksum $PatchedSmartphone
                sha256 = Get-Sha256 $PatchedSmartphone
            }
            allowedChange = 'children.carplay replaced exactly with pinned upstream carplay_child.json'
        }
        dioManager = [ordered]@{
            source = [ordered]@{
                bytes = $ExpectedStockDioSize
                posixCksum = $ExpectedStockDioCksum
                sha256 = $ExpectedStockDioSha256
            }
            prepared = [ordered]@{
                bytes = (Get-Item -LiteralPath $PatchedDio).Length
                posixCksum = Get-PosixCksum $PatchedDio
                sha256 = Get-Sha256 $PatchedDio
            }
            allowedChanges = [ordered]@{
                MessagesSentByAccessory = $RgiSentIds
                MessagesReceivedFromDevice = $RgiReceivedIds
            }
        }
    }

    Write-LfUtf8 `
        -Path (Join-Path $MetaDirectory 'config-patches.json') `
        -Content (($ConfigPatchPlan | ConvertTo-Json -Depth 10) + "`n")

    # One ordered table drives the installer, its previous-state validation,
    # the human/machine-readable deployment plans and rollback removal list.
    # Keeping these identities together prevents a new upstream payload from
    # being copied without also being checksum-gated and documented.

    foreach ($target in $InstallerTargets) {
        $sourcePath = Join-Path $PackageRoot $target.Source
        $target.Cksum = Get-PosixCksum $sourcePath
        $target.Bytes = (Get-Item -LiteralPath $sourcePath).Length
    }

    $StateSource = Join-Path $InstallerDirectory 'install-state.txt'
    $StateDestination = '/mnt/app/root/hooks/carplay_rgi_install.state'
    $OwnerMarkerSource = Join-Path $InstallerDirectory 'owner-marker.txt'

    Write-LfUtf8 `
        -Path $OwnerMarkerSource `
        -Content @"
CARPLAY_RGI_OWNER_V1
FIRMWARE=$FirmwareTrain
MU=$FirmwareName
"@

    $OwnerMarkerCksum = Get-PosixCksum $OwnerMarkerSource
    $OwnerMarkerBytes = (Get-Item -LiteralPath $OwnerMarkerSource).Length

    $StateLines = @(
        'CARPLAY_RGI_STATE_V1'
        ('PACKAGE_TAG=' + $SafeTag)
        ('PACKAGE_COMMIT=' + $Commit)
    )

    $StateLines += @(
        $InstallerTargets |
            ForEach-Object {
                $_.Key + '=' + $_.Cksum + ':' + $_.Bytes
            }
    )

    Write-LfUtf8 `
        -Path $StateSource `
        -Content (($StateLines -join "`n") + "`n")

    $StateCksum = Get-PosixCksum $StateSource
    $StateBytes = (Get-Item -LiteralPath $StateSource).Length

    $InstallFilePlan = @(
        $InstallerTargets |
            ForEach-Object {
                $entry = [ordered]@{
                    source = $_.Source
                    destination = $_.Destination
                    action = $_.Action
                    mode = $_.Mode
                    posixCksum = $_.Cksum
                    bytes = $_.Bytes
                }

                if ($_.Contains('RollbackSource')) {
                    $entry.rollbackSource = $_.RollbackSource
                }

                $entry
            }
    )

    $InstallFilePlan += [ordered]@{
        source = 'installer/install-state.txt'
        destination = $StateDestination
        action = 'add-managed-state'
        mode = '644'
        posixCksum = $StateCksum
        bytes = $StateBytes
    }

    $IntroducedPaths = @(
        $InstallerTargets |
            Where-Object { $_.Action -eq 'add' } |
            ForEach-Object { $_.Destination }
    ) + @($StateDestination)

    $DeploymentPlan = [ordered]@{
        schemaVersion = 2
        purpose = 'authoritative file plan used to generate the guarded car-side installer'
        releaseTag = $Tag
        sourceLabel = $Tag
        commit = $Commit
        supportsManagedUpdate = $true
        files = $InstallFilePlan
        stateFile = $StateDestination
        onUnitBackupDirectory = '/mnt/app/root/carplay-rgi-backup'
        requirements = @(
            'refuse a first install unless both verified stock configs match'
            'refuse a managed update unless every installed file matches its prior state'
            'validate every package source with POSIX cksum and byte count before any HU write'
            'prove both target partitions are writable with a write-and-remove probe before any HU write'
            'make and validate fresh on-unit and SD backups before a first write'
            'take the SD stock backup from the on-unit backup during a managed update, never from the live patched file'
            'stage files beside their destinations, verify, then rename into place'
            'inherit the permission bits of any file that already existed on the unit instead of asserting a mode'
            'commit configuration files after runtime files and commit managed state last'
            'verify every destination, sync, wait at least five seconds, sync again, and never reboot automatically'
            'leave /mnt/app and /mnt/system read-only again once the action has finished'
            'only after reporting install result 0 including read-only restoration and lock release, save and verify ACTION=rollback'
        )
    }

    Write-LfUtf8 `
        -Path (Join-Path $MetaDirectory 'deployment-plan.json') `
        -Content (($DeploymentPlan | ConvertTo-Json -Depth 10) + "`n")

    $RollbackPlan = [ordered]@{
        schemaVersion = 2
        scope = "owned $FirmwareName CarPlay-RGI installation, including interrupted first install"
        entryPoint = 'installer/rollback.sh'
        ownershipMarker = '/mnt/app/root/carplay-rgi-backup/owner-marker.txt'
        restores = @(
            [ordered]@{
                source = '/mnt/app/root/carplay-rgi-backup/smartphone_integrator.before.json'
                packageRecoverySource = 'rollback/smartphone_integrator.stock.json'
                destination = '/mnt/system/etc/eso/production/smartphone_integrator.json'
                bytes = $ExpectedStockSmartphoneSize
                posixCksum = $ExpectedStockSmartphoneCksum
                sha256 = $ExpectedStockSmartphoneSha256
            }
            [ordered]@{
                source = '/mnt/app/root/carplay-rgi-backup/dio_manager.before.json'
                packageRecoverySource = 'rollback/dio_manager.stock.json'
                destination = '/mnt/system/etc/eso/production/dio_manager.json'
                bytes = $ExpectedStockDioSize
                posixCksum = $ExpectedStockDioCksum
                sha256 = $ExpectedStockDioSha256
            }
        )
        removesIntroducedPaths = $IntroducedPaths
        safety = @(
            'Rollback requires the exact installer-owned marker and both verified fresh HU backups.'
            'Only explicit, package-owned file paths are removed; no recursive delete is used.'
            'Stock configs are staged and verified before replacement, keeping the permission bits the live file already had.'
            'Both target partitions are proved writable before anything is restored or removed.'
            'No processes are stopped and no reboot is performed automatically.'
            'A non-empty on-unit backup directory is left in place and reported as a note, not as a failure.'
        )
    }

    Write-LfUtf8 `
        -Path (Join-Path $RollbackDirectory 'rollback-plan.json') `
        -Content (($RollbackPlan | ConvertTo-Json -Depth 10) + "`n")

    $RollbackText = @"
$FirmwareName CARPLAY-RGI ROLLBACK MATERIALS
====================================

These are byte-exact, checksum-pinned stock configuration files. The generated
rollback script uses the fresh on-unit copies made immediately before the first
install, after confirming they still match this pinned $FirmwareName baseline.

A successful install arms rollback automatically; -ArmRollback can also arm it. It refuses to remove anything
without the exact installer-owned marker and valid on-unit backups.
"@

    Write-LfUtf8 `
        -Path (Join-Path $RollbackDirectory 'README.txt') `
        -Content $RollbackText

    # Generate shell fragments from the target table instead of maintaining a
    # second hand-written path/checksum list inside the installer templates.
    $PayloadChecks = @(
        $InstallerTargets |
            ForEach-Object {
                'check_file "$PKG/{0}" "{1}" "{2}" "{3}" || die "Package payload failed validation: {3}"' -f `
                    $_.Source, $_.Cksum, $_.Bytes, $_.Key
            }
    ) -join "`n"

    $TempDefinitions = @(
        $InstallerTargets |
            ForEach-Object {
                'TMP_{0}="{1}.carplay-rgi-new.$$"' -f $_.Key, $_.Destination
            }
    ) + @('TMP_STATE="' + $StateDestination + '.carplay-rgi-new.$$"')
    $TempDefinitions = $TempDefinitions -join "`n"

    $TempReferences = @(
        $InstallerTargets |
            ForEach-Object { '"$TMP_' + $_.Key + '"' }
    ) + @('"$TMP_STATE"')
    $CleanupTemps = 'rm -f ' + ($TempReferences -join ' ')

    $FirstAbsenceChecks = @(
        $InstallerTargets |
            Where-Object { $_.Action -eq 'add' } |
            ForEach-Object {
                '[ ! -e "{0}" ] && [ ! -L "{0}" ] || die "Refusing first install: {1} already exists"' -f `
                    $_.Destination, $_.Key
            }
    ) + @(
        '[ ! -e "$STATE" ] && [ ! -L "$STATE" ] || die "Refusing first install: managed state path already exists"'
        '[ ! -e "$HU_BACKUP" ] && [ ! -L "$HU_BACKUP" ] || die "Refusing first install: HU backup directory already exists"'
    )
    $FirstAbsenceChecks = $FirstAbsenceChecks -join "`n"

    $ManagedStateChecks = @(
        $InstallerTargets |
            ForEach-Object {
                if ($_.Contains('AllowMissingPreviousState')) {
                    '(missing_state_record "{0}" && [ ! -e "{1}" ] && [ ! -L "{1}" ]) || check_state_target "{0}" "{1}" "{0}" || die "Installed {0} is not absent or recorded in the prior state"' -f $_.Key, $_.Destination
                } else {
                'check_state_target "{0}" "{1}" "{2}" || die "Installed {2} does not match the recorded prior state"' -f `
                    $_.Key, $_.Destination, $_.Key
                }
            }
    ) -join "`n"

    # New files get an explicit mode; the two pre-existing configs inherit the
    # mode of the live file they replace, so nothing here invents permissions
    # for a file whose stock mode was never recorded off the car.
    $StageCalls = @(
        $InstallerTargets |
            ForEach-Object {
                if ($_.Action -eq 'replace-verified-stock') {
                    'stage_config "$PKG/{0}" "$TMP_{1}" "{2}" "{3}" "{4}" "{1}" || die "Could not stage {1}"' -f `
                        $_.Source, $_.Key, $_.Destination, $_.Cksum, $_.Bytes
                }
                else {
                    'stage_file "$PKG/{0}" "$TMP_{1}" "{2}" "{3}" "{4}" "{1}" || die "Could not stage {1}"' -f `
                        $_.Source, $_.Key, $_.Mode, $_.Cksum, $_.Bytes
                }
            }
    ) -join "`n"

    $RuntimeCommits = @(
        $InstallerTargets |
            Where-Object { $_.Action -eq 'add' } |
            ForEach-Object {
                'mv -f "$TMP_{0}" "{1}" || die "Could not commit {0}; use rollback before retrying"' -f `
                    $_.Key, $_.Destination
            }
    ) -join "`n"

    $ConfigCommits = @(
        $InstallerTargets |
            Where-Object { $_.Action -eq 'replace-verified-stock' } |
            ForEach-Object {
                'mv -f "$TMP_{0}" "{1}" || die "Could not commit {0}; use rollback before retrying"' -f `
                    $_.Key, $_.Destination
            }
    ) -join "`n"

    $TargetVerifications = @(
        $InstallerTargets |
            ForEach-Object {
                'check_file "{0}" "{1}" "{2}" "{3}" || die "Installed destination failed validation: {3}"' -f `
                    $_.Destination, $_.Cksum, $_.Bytes, $_.Key
            }
    ) -join "`n"

    $RollbackRemovals = @(
        $IntroducedPaths |
            ForEach-Object {
                'rm -f "{0}" || die "Could not remove owned path: {0}"' -f $_
            }
    ) -join "`n"

    $RollbackAbsenceChecks = @(
        $IntroducedPaths |
            ForEach-Object {
                '[ ! -e "{0}" ] && [ ! -L "{0}" ] || die "Owned path still exists after removal: {0}"' -f $_
            }
    ) -join "`n"

    $RollbackStateRecordChecks = @(
        $InstallerTargets |
            ForEach-Object {
                if ($_.Contains('AllowMissingPreviousState')) {
                    'validate_state_record "{0}" || (missing_state_record "{0}" && check_owned_path "{0}" "{1}" "{2}" "{3}") || die "Managed state record is invalid: {0}"' -f $_.Key, $_.Destination, $_.Cksum, $_.Bytes
                } else {
                'validate_state_record "{0}" || die "Managed state record is invalid: {0}"' -f $_.Key
                }
            }
    ) -join "`n"

    $RollbackOwnershipChecks = @(
        $InstallerTargets |
            ForEach-Object {
                if ($_.Action -eq 'add') {
                    'check_owned_path "{0}" "{1}" "{2}" "{3}" || die "Rollback refused: {0} is not absent or installer-owned"' -f `
                        $_.Key, $_.Destination, $_.Cksum, $_.Bytes
                }
                elseif ($_.Key -eq 'SMARTPHONE') {
                    'check_owned_config "{0}" "{1}" "{2}" "{3}" "{4}" "{5}" || die "Rollback refused: {0} is not stock or installer-owned"' -f `
                        $_.Key, $_.Destination, $_.Cksum, $_.Bytes,
                        $ExpectedStockSmartphoneCksum, $ExpectedStockSmartphoneSize
                }
                else {
                    'check_owned_config "{0}" "{1}" "{2}" "{3}" "{4}" "{5}" || die "Rollback refused: {0} is not stock or installer-owned"' -f `
                        $_.Key, $_.Destination, $_.Cksum, $_.Bytes,
                        $ExpectedStockDioCksum, $ExpectedStockDioSize
                }
            }
    ) -join "`n"

    # Readable PC-side templates; the generated vehicle scripts are self-contained.
    $SharedShellHelpers = $ShellTemplates['common.sh']

    $CommandTemplate = $ShellTemplates['command.sh']

    $InstallTemplate = $ShellTemplates['install.sh']

    $RollbackTemplate = $ShellTemplates['rollback.sh']

    $InstallTemplateValues = [ordered]@{
        PACKAGE_TAG = $SafeTag
        MU = $FirmwareName
        PACKAGE_COMMIT = $Commit
        SHARED_HELPERS = $SharedShellHelpers
        TEMP_DEFINITIONS = $TempDefinitions
        CLEANUP_TEMPS = $CleanupTemps
        OWNER_CKSUM = $OwnerMarkerCksum
        OWNER_BYTES = $OwnerMarkerBytes
        STOCK_SMARTPHONE_CKSUM = $ExpectedStockSmartphoneCksum
        STOCK_SMARTPHONE_BYTES = $ExpectedStockSmartphoneSize
        STOCK_DIO_CKSUM = $ExpectedStockDioCksum
        STOCK_DIO_BYTES = $ExpectedStockDioSize
        LSD_JXE_CKSUM = $ExpectedLsdJxeCksum
        LSD_JXE_BYTES = $ExpectedLsdJxeBytes
        PAYLOAD_CHECKS = $PayloadChecks
        STATE_CKSUM = $StateCksum
        STATE_BYTES = $StateBytes
        MANAGED_STATE_CHECKS = $ManagedStateChecks
        FIRST_ABSENCE_CHECKS = $FirstAbsenceChecks
        STAGE_CALLS = $StageCalls
        RUNTIME_COMMITS = $RuntimeCommits
        CONFIG_COMMITS = $ConfigCommits
        TARGET_VERIFICATIONS = $TargetVerifications
    }

    $RollbackTemplateValues = [ordered]@{
        PACKAGE_TAG = $SafeTag
        MU = $FirmwareName
        PACKAGE_COMMIT = $Commit
        SHARED_HELPERS = $SharedShellHelpers
        OWNER_CKSUM = $OwnerMarkerCksum
        OWNER_BYTES = $OwnerMarkerBytes
        STOCK_SMARTPHONE_CKSUM = $ExpectedStockSmartphoneCksum
        STOCK_SMARTPHONE_BYTES = $ExpectedStockSmartphoneSize
        STOCK_DIO_CKSUM = $ExpectedStockDioCksum
        STOCK_DIO_BYTES = $ExpectedStockDioSize
        ROLLBACK_STATE_RECORD_CHECKS = $RollbackStateRecordChecks
        ROLLBACK_OWNERSHIP_CHECKS = $RollbackOwnershipChecks
        ROLLBACK_REMOVALS = $RollbackRemovals
        ROLLBACK_ABSENCE_CHECKS = $RollbackAbsenceChecks
    }

    $InstallerPath = Join-Path $InstallerDirectory 'install.sh'
    $RollbackPath = Join-Path $InstallerDirectory 'rollback.sh'
    $PackagedCommandPath = Join-Path $InstallerDirectory 'command.sh'
    $CollectLogsPath = Join-Path $InstallerDirectory 'collect-logs.sh'
    $MibCommandPath = Join-Path $SdRoot 'mod\command.sh'

    Write-LfUtf8 `
        -Path $PackagedCommandPath `
        -Content $CommandTemplate
    Write-LfUtf8 `
        -Path $InstallerPath `
        -Content (Expand-TokenTemplate -Template $InstallTemplate -Values $InstallTemplateValues)
    Write-LfUtf8 `
        -Path $RollbackPath `
        -Content (Expand-TokenTemplate -Template $RollbackTemplate -Values $RollbackTemplateValues)
    # Include the shared standalone collector unchanged. The dispatcher invokes it
    # on MMX before rollback; it stays on the SD, not in the installed payload.
    Write-LfUtf8 `
        -Path $CollectLogsPath `
        -Content $ShellTemplates['collect-logs.sh']
    Copy-Item `
        -LiteralPath $PackagedCommandPath `
        -Destination (Join-Path $SdRoot 'mod/custom.sh')
    Copy-Item -LiteralPath (Join-Path $WorkTree 'deploy/mib/entry.sh') -Destination $MibCommandPath

    $SdBackupDirectory = Join-Path $SdRoot 'mod\carplay-rgi-backup'
    New-Item -ItemType Directory -Force -Path $SdBackupDirectory | Out-Null

    # Logs and stock-backup files are pre-created. Only the MMX child creates
    # the SD lock directory; the sourced M.I.B. outer shell writes no SD files.
    # Declared once, so the set that gets created here is by construction the
    # same set that gets verified below: a placeholder added to only one of two
    # lists would otherwise surface as a refusal on the car rather than a
    # failure on the PC.
    $MutableSdPaths = @(
        (Join-Path $SdRoot 'mod\carplay-rgi-install.log')
        (Join-Path $SdRoot 'mod\carplay-rgi-rollback.log')
        (Join-Path $SdBackupDirectory 'smartphone_integrator.before.json')
        (Join-Path $SdBackupDirectory 'dio_manager.before.json')
        (Join-Path $SdBackupDirectory 'metadata.txt')
    )

    foreach ($mutableSdPath in $MutableSdPaths) {
        Write-LfUtf8 -Path $mutableSdPath -Content ''
    }

    $ArmedAction = 'install'

    if ($ArmRollback) {
        $ArmedAction = 'rollback'
    }

    # A single, always-present control file is safer for repeatable SD-card
    # overlays than mutually exclusive marker files. Extracting a later overlay
    # now replaces the previous action instead of leaving a stale marker behind.
    $ActionControlPath = Join-Path $PackageRoot 'ACTION'
    $ExpectedActionControlText = $ArmedAction + "`n"
    Write-LfUtf8 -Path $ActionControlPath -Content $ExpectedActionControlText

    if ([System.IO.File]::ReadAllText($ActionControlPath) -cne $ExpectedActionControlText) {
        throw 'M.I.B. ACTION control file was not written exactly as requested.'
    }

    foreach ($qnxScript in @($PackagedCommandPath, $InstallerPath, $RollbackPath, $CollectLogsPath)) {
        Assert-QnxInstallerScript -Path $qnxScript -GitSh $GitSh
    }

    if ((Get-Sha256 $PackagedCommandPath) -ne (Get-Sha256 (Join-Path $SdRoot 'mod/custom.sh'))) {
        throw 'M.I.B. custom.sh does not match the checksum-covered packaged dispatcher.'
    }
    Assert-QnxInstallerScript -Path $MibCommandPath -GitSh $GitSh

    $MibCommandText = [System.IO.File]::ReadAllText($PackagedCommandPath)

    if ($MibCommandText -notmatch '(?m)^\s*on -f mmx /bin/sh /net/mmx/fs/sda0/mod/carplay-rgi/installer/(install|rollback)\.sh\s*$') {
        throw 'M.I.B. command.sh does not dispatch the vehicle action through the MMX shell.'
    }

    if ($MibCommandText.Contains('$TEE')) {
        throw 'M.I.B. command.sh must not depend on the undefined M.I.B. $TEE helper.'
    }

    # Lock only on the SD filesystem, after its remount and before HU remounts.
    # QNX procnto /tmp does not support mkdir. Never regress to that scheme.
    $sdMountIndex = $MibCommandText.IndexOf('mount -uw /net/mmx/fs/sda0')
    $lockIndex = $MibCommandText.IndexOf('if lock_error=`mkdir "$lock" 2>&1`; then')
    $appMountIndex = $MibCommandText.IndexOf('mount -uw /net/mmx/mnt/app')
    if (
        $MibCommandText.Contains('/tmp/carplay-rgi-install.lock') -or
        -not $MibCommandText.Contains('lock=/net/mmx/fs/sda0/mod/carplay-rgi-install.lock') -or
        $sdMountIndex -lt 0 -or $lockIndex -le $sdMountIndex -or
        $appMountIndex -le $lockIndex
    ) {
        throw 'Dispatcher must acquire an SD-directory lock after the SD remount and before firmware remounts.'
    }

    # A rollback captures the volatile logs first: after the SD lock, before any
    # firmware remount, only for rollback, and bounded so a hung probe cannot keep
    # the rollback from running. The gate, the backgrounded call and its watchdog
    # must be one block: the collector's MMX script (no single quotes inside it)
    # directly under the rollback test.
    $collectBlock = [regex]::Match(
        $MibCommandText,
        '(?s)if \[ "\$CPRGI_ACTION" = "rollback" \]; then\s*\n\s*echo "\[RGI\] Saving logs\.\.\."\s*\n\s*on -f mmx /bin/sh -c ''([^'']*)''\s*\n\s*case \$\? in')
    $collectScript = $collectBlock.Groups[1].Value
    if (
        -not $collectBlock.Success -or
        $collectBlock.Index -le $lockIndex -or
        $collectBlock.Index -ge $appMountIndex -or
        -not $collectScript.Contains('/bin/sh /net/mmx/fs/sda0/mod/carplay-rgi/installer/collect-logs.sh before-rollback > "$out" 2>&1 &') -or
        -not $collectScript.Contains('kill -9 "$collector"') -or
        ([regex]::Matches($MibCommandText, [regex]::Escape('installer/collect-logs.sh'))).Count -ne 1
    ) {
        throw 'Dispatcher must run the log collector for rollback only, bounded, after the SD lock and before firmware remounts.'
    }
    Assert-FileExists $CollectLogsPath 'packaged log collector'

    if ($MibCommandText -match '(?m)(?:>|>>)\s*/net/mmx/fs/sda0/') {
        throw 'M.I.B. command.sh must not redirect SD-card output from the sourced outer shell.'
    }

    foreach ($mutableSdPath in $MutableSdPaths) {
        if (-not (Test-Path -LiteralPath $mutableSdPath -PathType Leaf)) {
            throw "Required pre-created mutable SD file is missing: $mutableSdPath"
        }

        if ((Get-Item -LiteralPath $mutableSdPath).Length -ne 0) {
            throw "Required mutable SD placeholder is not empty: $mutableSdPath"
        }
    }

    $PayloadCheckCount = ([regex]::Matches(
        [System.IO.File]::ReadAllText($InstallerPath),
        '(?m)^check_file "\$PKG/(payload/[^\"]+)"'
    )).Count

    if ($PayloadCheckCount -ne $InstallerTargets.Count) {
        throw "Installer contains $PayloadCheckCount payload checks for $($InstallerTargets.Count) targets."
    }

    # Both vehicle scripts have to prove, before they change anything, that each
    # partition accepts a write and that this unit can copy a configuration file
    # without losing its permissions. Every path a probe can create is also a
    # named variable so cleanup_temps can remove it if the script is interrupted.
    $ProbedDirectories = @(
        '/mnt/app/root'
        '/mnt/system/etc/eso/production'
    )

    $ProbeVariables = [ordered]@{
        PROBE_APP = '/mnt/app/root/carplay-rgi-probe.$$'
        PROBE_SYS = '/mnt/system/etc/eso/production/carplay-rgi-probe.$$'
        PROBE_MODE_SMARTPHONE = '$TARGET_SMARTPHONE.carplay-rgi-probe.$$'
        PROBE_MODE_DIO = '$TARGET_DIO.carplay-rgi-probe.$$'
    }

    $ProbeCalls = @(
        'probe_writable "$PROBE_APP" || die '
        'probe_writable "$PROBE_SYS" || die '
        'probe_preserve "$TARGET_SMARTPHONE" "$PROBE_MODE_SMARTPHONE" || die '
        'probe_preserve "$TARGET_DIO" "$PROBE_MODE_DIO" || die '
    )

    foreach ($vehicleScript in @($InstallerPath, $RollbackPath)) {
        $vehicleText = [System.IO.File]::ReadAllText($vehicleScript)
        $leafName = Split-Path -Leaf $vehicleScript
        if ($vehicleScript -eq $InstallerPath) { $InstallerText = $vehicleText }

        foreach ($probeVariable in $ProbeVariables.Keys) {
            $definitionPattern =
                '(?m)^' + $probeVariable + '=' +
                [regex]::Escape($ProbeVariables[$probeVariable]) + '$'

            if ($vehicleText -notmatch $definitionPattern) {
                throw "$leafName does not define $probeVariable as $($ProbeVariables[$probeVariable])."
            }

            # An interrupted run must not be able to leave a probe file behind.
            $cleanupPattern =
                '(?s)cleanup_temps\(\) \{.*?"\$' + $probeVariable + '".*?\n\}'

            if ($vehicleText -notmatch $cleanupPattern) {
                throw "$leafName does not remove $probeVariable in cleanup_temps."
            }
        }

        foreach ($probeCall in $ProbeCalls) {
            if (-not $vehicleText.Contains("`n" + $probeCall)) {
                throw "$leafName is missing the guarded pre-flight call: $probeCall"
            }
        }

        # The whole pre-flight has to come before the first change to either
        # partition, so an unusable mount is a refusal and not a half-install.
        $probeIndex = $vehicleText.IndexOf("`nprobe_writable `"`$PROBE_APP`"")
        $lastProbeIndex = $vehicleText.IndexOf("`nprobe_preserve `"`$TARGET_DIO`"")
        # Indented commits count too (the first-install HU backups).
        $firstCommit = [regex]::Match($vehicleText, '(?m)^[ \t]*mv -f ')
        $firstCommitIndex = if ($firstCommit.Success) { $firstCommit.Index } else { -1 }

        if (
            $probeIndex -lt 0 -or
            $lastProbeIndex -lt $probeIndex -or
            $firstCommitIndex -lt 0 -or
            $lastProbeIndex -gt $firstCommitIndex
        ) {
            throw "$leafName does not complete its pre-flight probes before its first commit."
        }

        if ($vehicleText -match '(?m)^\s*chmod\s+777\b') {
            throw "$leafName still forces mode 777 on a head-unit file."
        }
        if ($vehicleText -notmatch '(?m)^\s*cp -p "\$model_path" "\$temp_path"') {
            throw "$leafName does not inherit the live configuration file's mode."
        }
    }

    Set-Guard 'installerWritabilityPreflight' 'PASS (install/rollback prove both partitions writable and mode-preserving before any write)'

    # Install only onto the HMI library the JAR was linked against, checked before any
    # write. Rollback must never depend on it: a changed library cannot block removal.
    $hmiIdentityLine = "`nHMI_LIBRARY_IDENTITY=$ExpectedLsdJxeCksum`:$ExpectedLsdJxeBytes`n"
    $hmiCheckIndex = $InstallerText.IndexOf('if [ "$1:$2" != "$HMI_LIBRARY_IDENTITY" ]; then')
    $firstWriteIndex = $InstallerText.IndexOf("`nprobe_writable `"`$PROBE_APP`"")
    if (
        -not $InstallerText.Contains($hmiIdentityLine) -or
        -not $InstallerText.Contains("`nHMI_LIBRARY=/ifs/lsd.jxe`n") -or
        $hmiCheckIndex -lt 0 -or $firstWriteIndex -lt 0 -or $hmiCheckIndex -gt $firstWriteIndex -or
        ([System.IO.File]::ReadAllText($RollbackPath)).Contains('HMI_LIBRARY')
    ) {
        throw 'Installer must refuse a unit whose /ifs/lsd.jxe differs from the linked library before any write, and rollback must not check it.'
    }
    Set-Guard 'installerHmiLibraryIdentity' "PASS (install refuses unless /ifs/lsd.jxe is $ExpectedLsdJxeCksum`:$ExpectedLsdJxeBytes; rollback does not check it)"

    # Neither script may assert a permission mode for a file whose stock mode
    # was never read off this car. Both stage the configs from the live file.
    foreach ($configTarget in @(
        $InstallerTargets | Where-Object { $_.Action -eq 'replace-verified-stock' }
    )) {
        $configStagePattern =
            '(?m)^stage_config "\$PKG/' + [regex]::Escape($configTarget.Source) +
            '" "\$TMP_' + $configTarget.Key + '" "' +
            [regex]::Escape($configTarget.Destination) + '" '

        if ($InstallerText -notmatch $configStagePattern) {
            throw "Installer does not stage $($configTarget.Key) from its live on-unit file, so it would not preserve that file's mode."
        }

        if ($configTarget.Mode -ne 'preserve') {
            throw "Config target $($configTarget.Key) declares mode $($configTarget.Mode); the deployment plan must record that its mode is inherited."
        }
    }

    Set-Guard 'installerPreservesConfigModes' 'PASS (configs staged from the live file; no asserted mode)'

    # The SD card belongs to M.I.B.: its launcher writes its own log there after
    # command.sh returns, so this package must not force it back to read-only.
    if ($MibCommandText -match 'mount -ur /net/mmx/fs/sda0(?![\w/])') {
        throw 'M.I.B. command.sh must not remount the SD card read-only; the launcher still writes to it.'
    }

    foreach ($restoredMount in @('/net/mmx/mnt/app', '/net/mmx/mnt/system')) {
        if ($MibCommandText -notmatch ('mount -ur ' + [regex]::Escape($restoredMount) + '(?![\w/])')) {
            throw "M.I.B. command.sh does not restore $restoredMount to read-only."
        }
        $writeIndex = $MibCommandText.IndexOf('mount -uw ' + $restoredMount)
        $restoreIndex = $MibCommandText.IndexOf('mount -ur ' + $restoredMount)

        if ($writeIndex -lt 0 -or $restoreIndex -lt $writeIndex) {
            throw "M.I.B. command.sh does not restore $restoredMount after remounting it read-write."
        }
    }

    Set-Guard 'installerRestoresReadOnlyMounts' 'PASS (command.sh returns /mnt/app and /mnt/system to read-only)'

    $armRollbackIndex = $MibCommandText.IndexOf('if [ "$CPRGI_ACTION" = "install" ] && [ "$CPRGI_RC" = "0" ]; then')
    if (
        $armRollbackIndex -le $MibCommandText.IndexOf('mount -ur /net/mmx/mnt/app') -or
        $armRollbackIndex -le $MibCommandText.LastIndexOf('rmdir /net/mmx/fs/sda0/mod/carplay-rgi-install.lock') -or
        $armRollbackIndex -le $MibCommandText.IndexOf('echo "[RGI] Success: 0"') -or
        -not $MibCommandText.Contains('mv -f "$next" "$action" || exit 1') -or
        -not $MibCommandText.Contains('[ "$1:$2" = "$expected_crc:9" ] || exit 1')
    ) {
        throw 'Dispatcher must save and verify ACTION=rollback only after reporting install result 0, following restoration and lock release.'
    }

    Set-Guard 'installerPayloadCksums' ("PASS ($PayloadCheckCount payloads + state/rollback/owner)")
    Set-Guard 'installerQnxShellProfile' 'PASS (command/install/rollback/collect-logs; Git sh -n)'
    Set-Guard 'installerNoAutomaticReboot'
    Set-Guard 'installerEntryPoint' 'PASS (M.I.B. mod/command.sh matches packaged dispatcher)'
    Set-Guard 'installerMibSdExecution' 'PASS (sourced command; MMX dispatch; SD lock before firmware remounts; rollback captures logs first; no TEE/outer SD redirection)'
    Set-Guard 'installerActionControl' ("PASS (ACTION=$ArmedAction; LF; rollback selected only after reported install result 0; editable on the card)")
    Set-Guard 'rollbackPackage'
