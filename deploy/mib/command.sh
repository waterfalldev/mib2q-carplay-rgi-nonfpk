#!/bin/sh

CPRGI_PKG=/net/mmx/fs/sda0/mod/carplay-rgi
CPRGI_ACTION_FILE=$CPRGI_PKG/ACTION
CPRGI_ACTION=invalid
CPRGI_RC=1
CPRGI_APP_RW=0
CPRGI_SYS_RW=0

if [ -f "$CPRGI_ACTION_FILE" ]; then
    CPRGI_ACTION=`cat "$CPRGI_ACTION_FILE" 2>/dev/null`
fi

# The M.I.B. screen gets short [RGI] lines only; install.sh, rollback.sh and the
# log collector keep their full detail in their logs on the SD card.
case "$CPRGI_ACTION" in
install) echo "[RGI] Starting Install..." ;;
rollback) echo "[RGI] Starting Rollback..." ;;
esac

case "$CPRGI_ACTION" in
install|rollback)
    # QNX /tmp may be procnto shared memory: it does not support directories.
    # Remount only the SD first, then acquire on its real filesystem from MMX.
    # A losing invocation must not remount either firmware partition.
    # Tool error text goes into the [RGI] line, never onto the screen by itself.
    if ! CPRGI_MOUNT_ERROR=`mount -uw /net/mmx/fs/sda0 2>&1`; then
        echo "[RGI] ERROR! Could not make the SD card writable${CPRGI_MOUNT_ERROR:+: $CPRGI_MOUNT_ERROR}."
        echo "[RGI] Failed: 1"
        return 1
    fi
    if ! on -f mmx /bin/sh -c '
        PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
        export PATH
        for utility in cat chmod cksum cp grep ls mkdir mv rm rmdir sleep sync; do
            command -v "$utility" >/dev/null 2>&1 || {
                echo "[RGI] ERROR! Missing MMX tool: $utility"
                exit 1
            }
        done
        lock=/net/mmx/fs/sda0/mod/carplay-rgi-install.lock
        if lock_error=`mkdir "$lock" 2>&1`; then
            exit 0
        fi
        if [ -e "$lock" ] || [ -L "$lock" ]; then
            echo "[RGI] ERROR! Another action holds the SD lock (mod/carplay-rgi-install.lock). Do not remove it while an action runs."
        else
            echo "[RGI] ERROR! Could not create the SD lock: $lock_error"
        fi
        exit 1
    '; then
        echo "[RGI] Failed: 1"
        return 1
    fi
    # Another completed invocation may have changed ACTION while this caller
    # was acquiring the lock. Never run an action selected from stale contents.
    if [ "`cat "$CPRGI_ACTION_FILE" 2>/dev/null`" != "$CPRGI_ACTION" ]; then
        echo "[RGI] ERROR! ACTION changed while starting. Run Individual Script again."
        CPRGI_RELEASE_ERROR=`on -f mmx /bin/sh -c '
            PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
            export PATH
            rmdir /net/mmx/fs/sda0/mod/carplay-rgi-install.lock
        ' 2>&1` || echo "[RGI] ERROR! Could not release the SD lock${CPRGI_RELEASE_ERROR:+: $CPRGI_RELEASE_ERROR}."
        echo "[RGI] Failed: 1"
        return 1
    fi
    # A rollback captures the volatile /tmp logs to the SD first, while the
    # installed build and its state are still in place: the reboot after a
    # rollback loses them. The collector only reads the unit and writes the SD,
    # so it runs before any firmware remount. Best effort: the rollback always
    # proceeds, and the capture result is reported either way. Bounded to 60 s
    # (QNX has no timeout(1)): a hung probe must never keep the rollback from
    # running. Its output goes to a file, not this pipe, so a hung child left
    # behind cannot hold the dispatcher open either. The file keeps the detail.
    if [ "$CPRGI_ACTION" = "rollback" ]; then
        echo "[RGI] Saving logs..."
        on -f mmx /bin/sh -c '
            PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
            export PATH
            out=/net/mmx/fs/sda0/mod/carplay-rgi-collect.out
            # Shell notices (such as a killed collector) go to the capture file, not the screen.
            exec 2>> "$out"
            /bin/sh /net/mmx/fs/sda0/mod/carplay-rgi/installer/collect-logs.sh before-rollback > "$out" 2>&1 &
            collector=$!
            for ten in 1 2 3 4 5 6; do
                for second in 1 2 3 4 5 6 7 8 9 10; do
                    if ! kill -0 "$collector" 2>/dev/null; then
                        wait "$collector"
                        exit $?
                    fi
                    sleep 1
                done
            done
            # TERM first, so the collector stops its own running probe on the way out.
            kill "$collector" 2>/dev/null
            sleep 2
            kill -9 "$collector" 2>/dev/null
            echo "Log capture did not finish within 60 s and was stopped." >> "$out"
            exit 2
        '
        case $? in
        0) echo "[RGI] Logs saved to mod/carplay-rgi-runtime-logs" ;;
        2) echo "[RGI] ERROR! Log capture timed out after 60 s. Continuing with rollback." ;;
        # Any other failure may have saved nothing at all (for example, every slot full).
        *) echo "[RGI] ERROR! Logs not saved or incomplete (see mod/carplay-rgi-collect.out). Continuing with rollback." ;;
        esac
    fi
    # /mnt/app and /mnt/system are read-only in normal operation. Each one is
    # only put back read-write for the length of the action and is restored
    # below, including when a later step refuses. The SD card is deliberately
    # left writable: the launcher writes its own log after this script returns.
    if ! CPRGI_MOUNT_ERROR=`mount -uw /net/mmx/mnt/app 2>&1`; then
        echo "[RGI] ERROR! Could not make /mnt/app writable${CPRGI_MOUNT_ERROR:+: $CPRGI_MOUNT_ERROR}."
    else
        CPRGI_APP_RW=1

        if ! CPRGI_MOUNT_ERROR=`mount -uw /net/mmx/mnt/system 2>&1`; then
            echo "[RGI] ERROR! Could not make /mnt/system writable${CPRGI_MOUNT_ERROR:+: $CPRGI_MOUNT_ERROR}."
        else
            CPRGI_SYS_RW=1

            if [ "$CPRGI_ACTION" = "install" ]; then
                on -f mmx /bin/sh /net/mmx/fs/sda0/mod/carplay-rgi/installer/install.sh
                CPRGI_RC=$?
            else
                on -f mmx /bin/sh /net/mmx/fs/sda0/mod/carplay-rgi/installer/rollback.sh
                CPRGI_RC=$?
            fi
        fi
    fi

    if [ "$CPRGI_SYS_RW" = "1" ]; then
        if ! CPRGI_MOUNT_ERROR=`mount -ur /net/mmx/mnt/system 2>&1`; then
            echo "[RGI] ERROR! /mnt/system could not be made read-only again${CPRGI_MOUNT_ERROR:+: $CPRGI_MOUNT_ERROR}. Keep the logs; do not reboot to bypass this."
            CPRGI_RC=1
        fi
    fi

    if [ "$CPRGI_APP_RW" = "1" ]; then
        if ! CPRGI_MOUNT_ERROR=`mount -ur /net/mmx/mnt/app 2>&1`; then
            echo "[RGI] ERROR! /mnt/app could not be made read-only again${CPRGI_MOUNT_ERROR:+: $CPRGI_MOUNT_ERROR}. Keep the logs; do not reboot to bypass this."
            CPRGI_RC=1
        fi
    fi
    if ! CPRGI_RELEASE_ERROR=`on -f mmx /bin/sh -c '
        PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
        export PATH
        rmdir /net/mmx/fs/sda0/mod/carplay-rgi-install.lock
    ' 2>&1`; then
        echo "[RGI] ERROR! Could not release the SD lock${CPRGI_RELEASE_ERROR:+: $CPRGI_RELEASE_ERROR}."
        CPRGI_RC=1
    fi
    ;;
*)
    echo "[RGI] ERROR! ACTION must contain exactly install or rollback."
    ;;
esac

if [ "$CPRGI_RC" = "0" ]; then
    echo "[RGI] Success: 0"
else
    echo "[RGI] Failed: $CPRGI_RC"
fi
# Only a fully completed install with a reported result 0 may change ACTION.
# This follow-up setting does not rewrite the already reported install result.
# Rollback and any install/cleanup failure leave ACTION unchanged.
    if [ "$CPRGI_ACTION" = "install" ] && [ "$CPRGI_RC" = "0" ]; then
        if on -f mmx /bin/sh -c '
            PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
            export PATH
            action=/net/mmx/fs/sda0/mod/carplay-rgi/ACTION
            next=$action.next.$$
            [ -f "$action" ] && [ ! -L "$action" ] || exit 1
            [ ! -e "$next" ] && [ ! -L "$next" ] || exit 1
            trap "rm -f \"$next\"" 0
            trap "exit 1" 1 2 3 15
            echo rollback > "$next" || exit 1
            chmod 644 "$next" || exit 1
            identity=`cksum "$next"` || exit 1
            set -- $identity
            expected_crc=$1
            [ "$2" = "9" ] && [ "`cat "$next"`" = "rollback" ] || exit 1
            mv -f "$next" "$action" || exit 1
            sync || exit 1
            sleep 5 || exit 1
            sync || exit 1
            identity=`cksum "$action"` || exit 1
            set -- $identity
            [ "$1:$2" = "$expected_crc:9" ] || exit 1
        '; then
            echo "[RGI] Next run: Rollback (ACTION is now rollback)"
        else
            echo "[RGI] ERROR! Installed, but ACTION=rollback could not be saved. Check ACTION on the SD card before the next run."
        fi
    fi

# M.I.B. sources this file; never exit its caller on a failed action.
return "$CPRGI_RC"
