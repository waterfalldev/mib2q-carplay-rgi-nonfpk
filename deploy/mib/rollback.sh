#!/bin/sh

# Preserve MMX resolution and append upstream's QNX utility locations.
PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
export PATH

PKG=/net/mmx/fs/sda0/mod/carplay-rgi
LOG=/net/mmx/fs/sda0/mod/carplay-rgi-rollback.log
HU_BACKUP=/mnt/app/root/carplay-rgi-backup
HU_OWNER=$HU_BACKUP/owner-marker.txt
HU_SMARTPHONE=$HU_BACKUP/smartphone_integrator.before.json
HU_DIO=$HU_BACKUP/dio_manager.before.json
PKG_OWNER=$PKG/installer/owner-marker.txt
TARGET_SMARTPHONE=/mnt/system/etc/eso/production/smartphone_integrator.json
TARGET_DIO=/mnt/system/etc/eso/production/dio_manager.json
STATE=/mnt/app/root/hooks/carplay_rgi_install.state
TMP_SMARTPHONE=$TARGET_SMARTPHONE.carplay-rgi-rollback.$$
TMP_DIO=$TARGET_DIO.carplay-rgi-rollback.$$
PROBE_APP=/mnt/app/root/carplay-rgi-probe.$$
PROBE_SYS=/mnt/system/etc/eso/production/carplay-rgi-probe.$$
PROBE_MODE_SMARTPHONE=$TARGET_SMARTPHONE.carplay-rgi-probe.$$
PROBE_MODE_DIO=$TARGET_DIO.carplay-rgi-probe.$$
# The renderer's GL program-binary cache (common/gl_program_cache.h GLPC_DIR).
GL_CACHE=/mnt/persist/var/app/luka_carplay_maneuver

# Detail goes to the log on the SD card only; the M.I.B. screen gets [RGI] lines.
log() {
    echo "$*" >> "$LOG" || {
        echo "[RGI] ERROR! Cannot write $LOG"
        exit 1
    }
}

cleanup_temps() {
    rm -f "$TMP_SMARTPHONE" "$TMP_DIO"
    rm -f "$PROBE_APP" "$PROBE_SYS" "$PROBE_MODE_SMARTPHONE" "$PROBE_MODE_DIO"
}

die() {
    log "ERROR: $*"
    echo "[RGI] ERROR! $*"
    cleanup_temps
    log "No reboot was performed."
    exit 1
}

@@SHARED_HELPERS@@

# Reports what an unrecognised file actually is, once every ownership identity
# has been ruled out.
log_identity() {
    identity_key=$1
    identity_path=$2
    identity_line=`cksum "$identity_path" 2>> "$LOG"` || {
        log "UNOWNED $identity_key: cannot read $identity_path"
        return 0
    }
    set -- $identity_line
    log "UNOWNED $identity_key: $identity_path is $1:$2"
}

validate_state_record() {
    state_key=$1
    count=`grep -c "^${state_key}=" "$STATE" 2>> "$LOG"`
    [ "$count" = "1" ] || return 1
    line=`grep "^${state_key}=" "$STATE" 2>> "$LOG"` || return 1
    identity=${line#*=}
    state_crc=${identity%%:*}
    state_size=${identity#*:}
    case "$state_crc" in ''|*[!0-9]*) return 1 ;; esac
    case "$state_size" in ''|*[!0-9]*) return 1 ;; esac
    return 0
}

check_state_target() {
    state_key=$1
    target_path=$2
    validate_state_record "$state_key" || return 1
    line=`grep "^${state_key}=" "$STATE" 2>> "$LOG"` || return 1
    identity=${line#*=}
    state_crc=${identity%%:*}
    state_size=${identity#*:}
    check_file "$target_path" "$state_crc" "$state_size"
}

check_owned_path() {
    state_key=$1
    target_path=$2
    package_crc=$3
    package_size=$4
    [ ! -e "$target_path" ] && [ ! -L "$target_path" ] && return 0
    check_file "$target_path" "$package_crc" "$package_size" && return 0
    [ -f "$STATE" ] && check_state_target "$state_key" "$target_path" && return 0
    log_identity "$state_key" "$target_path"
    return 1
}

check_owned_config() {
    state_key=$1
    target_path=$2
    package_crc=$3
    package_size=$4
    stock_crc=$5
    stock_size=$6
    check_file "$target_path" "$stock_crc" "$stock_size" && return 0
    check_file "$target_path" "$package_crc" "$package_size" && return 0
    [ -f "$STATE" ] && check_state_target "$state_key" "$target_path" && return 0
    log_identity "$state_key" "$target_path"
    return 1
}

# The renderer writes its shader cache to the persist partition at runtime, so no
# install record covers it. Only the cache's own names are removed - <16 hex>.bin and
# the .bin.<pid> of an interrupted write - never recursively, then the directory if
# empty. The files are inert without the renderer: a failure here is a warning, not
# a failed rollback.
remove_gl_cache() {
    [ ! -e "$GL_CACHE" ] && [ ! -L "$GL_CACHE" ] && return 0
    if [ -L "$GL_CACHE" ] || [ ! -d "$GL_CACHE" ]; then
        log "NOTE: $GL_CACHE is not a directory and was left in place."
        return 0
    fi
    for cache_file in "$GL_CACHE"/*; do
        [ -e "$cache_file" ] || [ -L "$cache_file" ] || continue
        case "${cache_file##*/}" in
            [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f].bin|\
            [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f].bin.[0-9]*)
                if [ -f "$cache_file" ] && [ ! -L "$cache_file" ] &&
                        rm -f "$cache_file" 2>> "$LOG" && [ ! -e "$cache_file" ]; then
                    log "Removed renderer shader cache file $cache_file"
                else
                    log "WARNING: could not remove renderer shader cache file $cache_file"
                fi
                ;;
        esac
    done
    if rmdir "$GL_CACHE" 2>> "$LOG"; then
        log "Removed renderer shader cache directory $GL_CACHE"
    else
        log "WARNING: $GL_CACHE was left in place; it holds files this package did not create or could not remove."
    fi
    return 0
}

[ -f "$LOG" ] || {
    echo "[RGI] ERROR! Missing rollback log file $LOG; copy the complete package."
    exit 1
}
cat /dev/null > "$LOG" || exit 1
# Raw utility errors belong in the log too, not on the M.I.B. screen.
exec 2>> "$LOG"
log "CarPlay-RGI @@MU@@ rollback started."
log "Package: @@PACKAGE_TAG@@ / @@PACKAGE_COMMIT@@"
trap 'cleanup_temps; log "Interrupted; no reboot was performed"; echo "[RGI] ERROR! Interrupted"; exit 1' 1 2 3 15

check_file "$PKG_OWNER" "@@OWNER_CKSUM@@" "@@OWNER_BYTES@@" "package owner marker" || die "Package owner marker failed validation"
check_file "$PKG/rollback/smartphone_integrator.stock.json" "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "package rollback smartphone" || die "Package rollback smartphone failed validation"
check_file "$PKG/rollback/dio_manager.stock.json" "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "package rollback dio" || die "Package rollback dio failed validation"
check_file "$HU_OWNER" "@@OWNER_CKSUM@@" "@@OWNER_BYTES@@" "HU owner marker" || die "Rollback refused: installer-owned HU marker is missing or invalid"
check_file "$HU_SMARTPHONE" "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "HU smartphone backup" || die "Rollback refused: fresh HU smartphone backup is invalid"
check_file "$HU_DIO" "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "HU dio backup" || die "Rollback refused: fresh HU dio backup is invalid"
[ ! -e "$STATE" ] || [ -f "$STATE" ] || die "Rollback refused: managed state path is not a regular file"
if [ -f "$STATE" ]; then
    [ "`grep -c '^CARPLAY_RGI_STATE_V1$' "$STATE" 2>> "$LOG"`" = "1" ] || die "Managed state header is invalid"
@@ROLLBACK_STATE_RECORD_CHECKS@@
fi
@@ROLLBACK_OWNERSHIP_CHECKS@@

probe_writable "$PROBE_APP" || die "Refusing to continue: /mnt/app is not writable"
probe_writable "$PROBE_SYS" || die "Refusing to continue: /mnt/system is not writable"
probe_preserve "$TARGET_SMARTPHONE" "$PROBE_MODE_SMARTPHONE" || die "Refusing to continue: this unit cannot copy smartphone_integrator.json while keeping its permissions"
probe_preserve "$TARGET_DIO" "$PROBE_MODE_DIO" || die "Refusing to continue: this unit cannot copy dio_manager.json while keeping its permissions"
log "Both target partitions accepted a write-and-remove probe and a mode-preserving copy."

stage_config "$HU_SMARTPHONE" "$TMP_SMARTPHONE" "$TARGET_SMARTPHONE" "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "staged smartphone" || die "Could not stage stock smartphone config"
stage_config "$HU_DIO" "$TMP_DIO" "$TARGET_DIO" "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "staged dio" || die "Could not stage stock dio config"
sync || die "Could not flush staged stock configs; restore has not started"
mv -f "$TMP_SMARTPHONE" "$TARGET_SMARTPHONE" || die "Could not restore stock smartphone config"
mv -f "$TMP_DIO" "$TARGET_DIO" || die "Could not restore stock dio config"
sync || die "Filesystem flush failed; preserve the log and do not reboot"
check_file "$TARGET_SMARTPHONE" "@@STOCK_SMARTPHONE_CKSUM@@" "@@STOCK_SMARTPHONE_BYTES@@" "restored smartphone" || die "Restored smartphone config failed validation"
check_file "$TARGET_DIO" "@@STOCK_DIO_CKSUM@@" "@@STOCK_DIO_BYTES@@" "restored dio" || die "Restored dio config failed validation"

@@ROLLBACK_REMOVALS@@
@@ROLLBACK_ABSENCE_CHECKS@@
rm -f "$HU_OWNER" "$HU_SMARTPHONE" "$HU_DIO" || die "Could not remove installer-owned HU backup files"
[ ! -e "$HU_OWNER" ] || die "Owned path still exists after removal: $HU_OWNER"
[ ! -e "$HU_SMARTPHONE" ] || die "Owned path still exists after removal: $HU_SMARTPHONE"
[ ! -e "$HU_DIO" ] || die "Owned path still exists after removal: $HU_DIO"
# The rollback itself is complete at this point. Tidying the now-unused backup
# directory is the last, optional step: nothing recursive is ever deleted, so if
# something unrelated is in there the directory simply stays. That is a note,
# not a failure - reporting it as an error would tell the operator the car is
# still modified when it is not.
rmdir "$HU_BACKUP" 2>> "$LOG" ||
    log "NOTE: $HU_BACKUP still holds files this package did not create and was left in place."
remove_gl_cache
sync || die "Filesystem flush failed; preserve the log and do not reboot"
log "Rollback verified; allowing five seconds for persistent storage to settle."
sleep 5 || die "Storage settling delay failed"
sync || die "Filesystem flush failed; preserve the log and do not reboot"
log "SUCCESS: stock configs restored and installer-owned files removed."
log "No processes were stopped and no reboot was performed. Review this log, then reboot the unit manually."
exit 0
