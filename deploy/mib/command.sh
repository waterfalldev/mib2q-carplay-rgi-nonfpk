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

case "$CPRGI_ACTION" in
install|rollback)
    # QNX /tmp may be procnto shared memory: it does not support directories.
    # Remount only the SD first, then acquire on its real filesystem from MMX.
    # A losing invocation must not remount either firmware partition.
    if ! mount -uw /net/mmx/fs/sda0; then
        echo "CarPlay-RGI refused: could not remount the SD card read-write."
        return 1
    fi
    if ! on -f mmx /bin/sh -c '
        PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
        export PATH
        for utility in cat chmod cksum cp grep ls mkdir mv rm rmdir sleep sync; do
            command -v "$utility" >/dev/null 2>&1 || {
                echo "CarPlay-RGI refused: required MMX utility is missing: $utility"
                exit 1
            }
        done
        lock=/net/mmx/fs/sda0/mod/carplay-rgi-install.lock
        if mkdir "$lock" 2>&1; then
            exit 0
        fi
        if [ -e "$lock" ] || [ -L "$lock" ]; then
            echo "CarPlay-RGI refused: SD lock path exists: $lock. Do not remove it while an action is running."
        else
            echo "CarPlay-RGI refused: could not create SD lock: $lock. See the filesystem error above."
        fi
        exit 1
    '; then
        echo "CarPlay-RGI stopped before firmware remounts. Preserve the M.I.B. output."
        return 1
    fi
    # Another completed invocation may have changed ACTION while this caller
    # was acquiring the lock. Never run an action selected from stale contents.
    if [ "`cat "$CPRGI_ACTION_FILE" 2>/dev/null`" != "$CPRGI_ACTION" ]; then
        echo "CarPlay-RGI refused: ACTION changed while acquiring the lock. Run Individual Script again."
        on -f mmx /bin/sh -c '
            PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
            export PATH
            rmdir /net/mmx/fs/sda0/mod/carplay-rgi-install.lock
        ' || echo "CarPlay-RGI ERROR: could not release the action lock."
        return 1
    fi
    # A rollback captures the volatile /tmp logs to the SD first, while the
    # installed build and its state are still in place: the reboot after a
    # rollback loses them. The collector only reads the unit and writes the SD,
    # so it runs before any firmware remount. Best effort: the rollback always
    # proceeds, and the capture result is reported either way. Bounded to 60 s
    # (QNX has no timeout(1)): a hung probe must never keep the rollback from
    # running. Its output goes to a file, not this pipe, so a hung child left
    # behind cannot hold the dispatcher open either.
    if [ "$CPRGI_ACTION" = "rollback" ]; then
        if on -f mmx /bin/sh -c '
            PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
            export PATH
            out=/net/mmx/fs/sda0/mod/carplay-rgi-collect.out
            /bin/sh /net/mmx/fs/sda0/mod/carplay-rgi/installer/collect-logs.sh before-rollback > "$out" 2>&1 &
            collector=$!
            for ten in 1 2 3 4 5 6; do
                for second in 1 2 3 4 5 6 7 8 9 10; do
                    if ! kill -0 "$collector" 2>/dev/null; then
                        wait "$collector"
                        status=$?
                        cat "$out"
                        exit $status
                    fi
                    sleep 1
                done
            done
            kill -9 "$collector" 2>/dev/null
            cat "$out"
            echo "CarPlay-RGI log capture did not finish within 60 s and was stopped."
            exit 2
        '; then
            echo "CarPlay-RGI logs captured before rollback (mod/carplay-rgi-runtime-logs)."
        else
            echo "CarPlay-RGI WARNING: logs were not fully captured before rollback; see the lines above. The rollback continues."
        fi
    fi
    # /mnt/app and /mnt/system are read-only in normal operation. Each one is
    # only put back read-write for the length of the action and is restored
    # below, including when a later step refuses. The SD card is deliberately
    # left writable: the launcher writes its own log after this script returns.
    if ! mount -uw /net/mmx/mnt/app; then
        echo "CarPlay-RGI refused: could not remount /mnt/app read-write."
    else
        CPRGI_APP_RW=1

        if ! mount -uw /net/mmx/mnt/system; then
            echo "CarPlay-RGI refused: could not remount /mnt/system read-write."
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
        if ! mount -ur /net/mmx/mnt/system; then
            echo "CarPlay-RGI ERROR: /mnt/system could not be returned read-only. Preserve the log; do not reboot to bypass an incomplete action."
            CPRGI_RC=1
        fi
    fi

    if [ "$CPRGI_APP_RW" = "1" ]; then
        if ! mount -ur /net/mmx/mnt/app; then
            echo "CarPlay-RGI ERROR: /mnt/app could not be returned read-only. Preserve the log; do not reboot to bypass an incomplete action."
            CPRGI_RC=1
        fi
    fi
    if ! on -f mmx /bin/sh -c '
        PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
        export PATH
        rmdir /net/mmx/fs/sda0/mod/carplay-rgi-install.lock
    '; then
        echo "CarPlay-RGI ERROR: could not release the action lock."
        CPRGI_RC=1
    fi
    ;;
*)
    echo "CarPlay-RGI refused: ACTION must contain exactly install or rollback."
    ;;
esac

echo "CarPlay-RGI action: $CPRGI_ACTION; result: $CPRGI_RC"
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
            echo "CarPlay-RGI ACTION is now rollback. The next Individual Script run captures logs and uninstalls CarPlay-RGI."
        else
            echo "CarPlay-RGI WARNING: install result remains 0, but ACTION=rollback could not be saved and verified. Check ACTION on the SD card before running Individual Script again."
        fi
    fi

# M.I.B. sources this file; never exit its caller on a failed action.
return "$CPRGI_RC"
