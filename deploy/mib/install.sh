#!/bin/sh

# Preserve MMX resolution and append upstream's QNX utility locations.
PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
export PATH

PKG=/net/mmx/fs/sda0/mod/carplay-rgi
LOG=/net/mmx/fs/sda0/mod/carplay-rgi-install.log
SD_BACKUP=/net/mmx/fs/sda0/mod/carplay-rgi-backup
SD_SMARTPHONE=$SD_BACKUP/smartphone_integrator.before.json
SD_DIO=$SD_BACKUP/dio_manager.before.json
SD_METADATA=$SD_BACKUP/metadata.txt
HU_BACKUP=/mnt/app/root/carplay-rgi-backup
HU_OWNER=$HU_BACKUP/owner-marker.txt
HU_SMARTPHONE=$HU_BACKUP/smartphone_integrator.before.json
HU_DIO=$HU_BACKUP/dio_manager.before.json
PKG_OWNER=$PKG/installer/owner-marker.txt
TARGET_SMARTPHONE=/mnt/system/etc/eso/production/smartphone_integrator.json
TARGET_DIO=/mnt/system/etc/eso/production/dio_manager.json
STATE=/mnt/app/root/hooks/carplay_rgi_install.state
@@TEMP_DEFINITIONS@@
TMP_HU_OWNER=$HU_BACKUP/owner-marker.txt.carplay-rgi-new.$$
TMP_HU_SMARTPHONE=$HU_BACKUP/smartphone_integrator.before.json.carplay-rgi-new.$$
TMP_HU_DIO=$HU_BACKUP/dio_manager.before.json.carplay-rgi-new.$$
PROBE_APP=/mnt/app/root/carplay-rgi-probe.$$
PROBE_SYS=/mnt/system/etc/eso/production/carplay-rgi-probe.$$
PROBE_MODE_SMARTPHONE=$TARGET_SMARTPHONE.carplay-rgi-probe.$$
PROBE_MODE_DIO=$TARGET_DIO.carplay-rgi-probe.$$

log() {
    echo "$*"
    echo "$*" >> "$LOG" || {
        echo "CarPlay-RGI ERROR: cannot append to $LOG; stopping."
        exit 1
    }
}

cleanup_temps() {
    @@CLEANUP_TEMPS@@
    rm -f "$TMP_HU_OWNER" "$TMP_HU_SMARTPHONE" "$TMP_HU_DIO"
    rm -f "$PROBE_APP" "$PROBE_SYS" "$PROBE_MODE_SMARTPHONE" "$PROBE_MODE_DIO"
}

die() {
    log "ERROR: $*"
    cleanup_temps
    log "No reboot was performed. If a commit had started, run the armed rollback before retrying."
    exit 1
}

@@SHARED_HELPERS@@

# New files, so an explicit mode is correct here: nothing is being inherited.
stage_file() {
    source_path=$1
    temp_path=$2
    file_mode=$3
    expected_crc=$4
    expected_size=$5
    description=$6

    rm -f "$temp_path" || return 1
    cat "$source_path" 2>> "$LOG" > "$temp_path" || return 1
    chmod "$file_mode" "$temp_path" 2>> "$LOG" || return 1
    check_file "$temp_path" "$expected_crc" "$expected_size" "$description" || return 1
    return 0
}

check_state_target() {
    state_key=$1
    target_path=$2
    description=$3
    count=`grep -c "^${state_key}=" "$STATE" 2>> "$LOG"`
    [ "$count" = "1" ] || return 1
    line=`grep "^${state_key}=" "$STATE" 2>> "$LOG"` || return 1
    identity=${line#*=}
    expected_crc=${identity%%:*}
    expected_size=${identity#*:}
    case "$expected_crc" in ''|*[!0-9]*) return 1 ;; esac
    case "$expected_size" in ''|*[!0-9]*) return 1 ;; esac
    check_file "$target_path" "$expected_crc" "$expected_size" "$description"
}

prepare_sd_backup() {
    source_path=$1
    backup_path=$2
    expected_crc=$3
    expected_size=$4
    description=$5
    [ -f "$backup_path" ] || return 1
    identity=`cksum "$backup_path" 2>> "$LOG"` || return 1
    set -- $identity
    if [ "$2" = "0" ]; then
        # The empty placeholder is only ever filled from a source that has
        # already been proved to be the stock file, so a refused run cannot
        # leave non-stock content behind in the SD backup.
        check_file "$source_path" "$expected_crc" "$expected_size" "$description source" || return 1
        cat "$source_path" 2>> "$LOG" > "$backup_path" || return 1
    fi
    check_file "$backup_path" "$expected_crc" "$expected_size" "$description"
}

reset_first_backup() {
    rm -f "$HU_OWNER" "$HU_SMARTPHONE" "$HU_DIO"
    rm -f "$TMP_HU_OWNER" "$TMP_HU_SMARTPHONE" "$TMP_HU_DIO"
    rmdir "$HU_BACKUP" 2>> "$LOG"
}

[ -f "$LOG" ] || {
    echo "Missing pre-created install log: $LOG"
    exit 1
}
cat /dev/null > "$LOG" || exit 1
log "CarPlay-RGI @@MU@@ install/update started."
log "Package: @@PACKAGE_TAG@@ / @@PACKAGE_COMMIT@@"
trap 'cleanup_temps; log "Interrupted; no reboot was performed"; exit 1' 1 2 3 15

[ -d "$PKG" ] || die "Package directory is missing"
[ -f "$SD_SMARTPHONE" ] || die "Pre-created SD smartphone backup file is missing"
[ -f "$SD_DIO" ] || die "Pre-created SD dio backup file is missing"
[ -f "$SD_METADATA" ] || die "Pre-created SD metadata file is missing"
check_file "$PKG_OWNER" "@@OWNER_CKSUM@@" "@@OWNER_BYTES@@" "owner marker" || die "Package owner marker failed validation"
check_file "$PKG/rollback/smartphone_integrator.stock.json" "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "rollback smartphone" || die "Package rollback smartphone failed validation"
check_file "$PKG/rollback/dio_manager.stock.json" "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "rollback dio" || die "Package rollback dio failed validation"
@@PAYLOAD_CHECKS@@
check_file "$PKG/installer/install-state.txt" "@@STATE_CKSUM@@" "@@STATE_BYTES@@" "install state" || die "Package install state failed validation"
log "All package sources passed POSIX cksum and byte-count validation."

# These independently installed navigation patches conflict with CarPlay app
# state. They are not ours to delete or silently adopt.
for conflicting_jar in NavActiveIgnore.jar navignore_audi.jar navignore_vw.jar; do
    conflicting_path=/mnt/app/eso/hmi/lsd/jars/$conflicting_jar
    [ ! -e "$conflicting_path" ] && [ ! -L "$conflicting_path" ] ||
        die "Conflicting navigation patch: $conflicting_jar. Remove it with its original installer before retrying."
done

if [ -f "$STATE" ]; then
    MODE=managed-update
    [ "`grep -c '^CARPLAY_RGI_STATE_V1$' "$STATE" 2>> "$LOG"`" = "1" ] || die "Managed state header is invalid"
    check_file "$HU_OWNER" "@@OWNER_CKSUM@@" "@@OWNER_BYTES@@" "HU owner marker" || die "HU owner marker is missing or invalid"
    check_file "$HU_SMARTPHONE" "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "HU smartphone backup" || die "HU smartphone backup is invalid"
    check_file "$HU_DIO" "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "HU dio backup" || die "HU dio backup is invalid"
@@MANAGED_STATE_CHECKS@@
else
    MODE=first-install
@@FIRST_ABSENCE_CHECKS@@
    check_file "$TARGET_SMARTPHONE" "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "stock smartphone" || die "Stock smartphone_integrator.json does not match the verified @@MU@@ baseline"
    check_file "$TARGET_DIO" "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "stock dio" || die "Stock dio_manager.json does not match the verified @@MU@@ baseline"
fi
log "Validation mode: $MODE"

probe_writable "$PROBE_APP" || die "Refusing to continue: /mnt/app is not writable"
probe_writable "$PROBE_SYS" || die "Refusing to continue: /mnt/system is not writable"
probe_preserve "$TARGET_SMARTPHONE" "$PROBE_MODE_SMARTPHONE" || die "Refusing to continue: this unit cannot copy smartphone_integrator.json while keeping its permissions"
probe_preserve "$TARGET_DIO" "$PROBE_MODE_DIO" || die "Refusing to continue: this unit cannot copy dio_manager.json while keeping its permissions"
log "Both target partitions accepted a write-and-remove probe and a mode-preserving copy."

# On a first install the live configs have just been proved to be stock, so they
# are the backup source. On a managed update they are the patched files, and the
# on-unit backup - already re-verified against the stock identity above - is the
# only correct source. Taking the live file there would populate a freshly
# extracted overlay's empty SD placeholder with patched content.
if [ "$MODE" = "first-install" ]; then
    SD_SOURCE_SMARTPHONE=$TARGET_SMARTPHONE
    SD_SOURCE_DIO=$TARGET_DIO
else
    SD_SOURCE_SMARTPHONE=$HU_SMARTPHONE
    SD_SOURCE_DIO=$HU_DIO
fi

prepare_sd_backup "$SD_SOURCE_SMARTPHONE" "$SD_SMARTPHONE" "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "SD smartphone backup" || die "Could not create or validate SD smartphone backup"
prepare_sd_backup "$SD_SOURCE_DIO" "$SD_DIO" "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "SD dio backup" || die "Could not create or validate SD dio backup"
{
    echo "--- $MODE"
    ls -l "$TARGET_SMARTPHONE" "$TARGET_DIO"
} >> "$SD_METADATA" 2>> "$LOG" || die "Could not record configuration metadata on SD"
sync || die "Filesystem flush failed; preserve the log and do not reboot"

if [ "$MODE" = "first-install" ]; then
    mkdir -p "$HU_BACKUP" || die "Could not create HU backup directory"
    stage_file "$TARGET_SMARTPHONE" "$TMP_HU_SMARTPHONE" 644 "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "HU smartphone backup" || {
        reset_first_backup
        die "Could not stage HU smartphone backup"
    }
    stage_file "$TARGET_DIO" "$TMP_HU_DIO" 644 "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "HU dio backup" || {
        reset_first_backup
        die "Could not stage HU dio backup"
    }
    stage_file "$PKG_OWNER" "$TMP_HU_OWNER" 644 "@@OWNER_CKSUM@@" "@@OWNER_BYTES@@" "HU owner marker" || {
        reset_first_backup
        die "Could not stage HU owner marker"
    }
    mv -f "$TMP_HU_SMARTPHONE" "$HU_SMARTPHONE" || {
        reset_first_backup
        die "Could not commit HU smartphone backup"
    }
    mv -f "$TMP_HU_DIO" "$HU_DIO" || {
        reset_first_backup
        die "Could not commit HU dio backup"
    }
    mv -f "$TMP_HU_OWNER" "$HU_OWNER" || {
        reset_first_backup
        die "Could not commit HU owner marker"
    }
    sync || die "Filesystem flush failed; preserve the log and do not reboot"
    check_file "$HU_SMARTPHONE" "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "HU smartphone backup" || {
        reset_first_backup
        die "Committed HU smartphone backup failed validation"
    }
    check_file "$HU_DIO" "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "HU dio backup" || {
        reset_first_backup
        die "Committed HU dio backup failed validation"
    }
    check_file "$HU_OWNER" "@@OWNER_CKSUM@@" "@@OWNER_BYTES@@" "HU owner marker" || {
        reset_first_backup
        die "Committed HU owner marker failed validation"
    }
fi

mkdir -p /mnt/app/root/hooks /mnt/app/eso/hmi/lsd/jars || die "Could not create target directories"
@@STAGE_CALLS@@
stage_file "$PKG/installer/install-state.txt" "$TMP_STATE" 644 "@@STATE_CKSUM@@" "@@STATE_BYTES@@" "STATE" || die "Could not stage managed state"
sync || die "Could not flush staged files; no payload commit started"
log "All destinations staged and verified; starting commit."

@@RUNTIME_COMMITS@@
sync || die "Filesystem flush failed; preserve the log and do not reboot"
@@CONFIG_COMMITS@@
sync || die "Filesystem flush failed; preserve the log and do not reboot"
mv -f "$TMP_STATE" "$STATE" || die "Could not commit managed state; use rollback before retrying"
sync || die "Filesystem flush failed; preserve the log and do not reboot"

@@TARGET_VERIFICATIONS@@
check_file "$STATE" "@@STATE_CKSUM@@" "@@STATE_BYTES@@" "STATE" || die "Installed managed state failed validation"
log "All installed files verified; allowing five seconds for persistent storage to settle."
sync || die "Filesystem flush failed; preserve the log and do not reboot"
sleep 5 || die "Storage settling delay failed"
sync || die "Filesystem flush failed; preserve the log and do not reboot"
log "SUCCESS: all installed files match the prepared package."
log "No processes were stopped and no reboot was performed. Review this log, then reboot the unit manually."
exit 0
