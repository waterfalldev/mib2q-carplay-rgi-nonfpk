#!/bin/sh

# Shared CarPlay-RGI runtime log collector. No package is needed for standalone use:
# copy this file to the M.I.B. SD card as mod/command.sh and run Individual Script.
# The package build also includes this exact file in its installer directory.
# Standalone use arms a detached worker and returns to M.I.B. Leave the menu
# and reproduce the ticking after the worker's thirty-second preparation delay.
# Explicit "live", "manual" or "before-rollback" arguments run in the foreground on MMX;
# the package dispatcher already remounts the SD and invokes it there.
# The no-argument launcher returns safely when sourced by M.I.B. The MMX worker
# stays in the invoked shell so the package watchdog can stop that same process.
case "${1:-}" in
    '')
        (
            if ! mount -uw /net/mmx/fs/sda0; then
                echo "CarPlay-RGI log collection refused: could not remount the SD card read-write."
                exit 1
            fi
            on -f mmx /bin/sh /net/mmx/fs/sda0/mod/command.sh arm
            result=$?
            echo "CarPlay-RGI log arming result: $result (not the capture result)"
            echo "SD output: /net/mmx/fs/sda0/mod/carplay-rgi-runtime-logs"
            exit "$result"
        )
        CPRGI_COLLECT_RC=$?
        # return is for sourcing; exit is the fallback for direct execution.
        return "$CPRGI_COLLECT_RC" 2>/dev/null || exit "$CPRGI_COLLECT_RC"
        ;;
    arm|background|live|manual|before-rollback) ;;
    *)
        echo "CarPlay-RGI log collection refused: unknown argument: $1"
        return 1 2>/dev/null || exit 1
        ;;
esac

PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
export PATH

LOG_BASE=/net/mmx/fs/sda0/mod/carplay-rgi-runtime-logs
LIVE_LOCK=$LOG_BASE/.live-active
LOCK_OWNED=0
RUN=
COLLECT_ERROR=0
TRIGGER=${1:-manual}

# Release only this capture's reservation. A refused or unacknowledged launch
# must not remove a worker's lock or a later capture's reservation.
release_live_lock()
{
    if [ "$LOCK_OWNED" = 1 ]; then
        if [ -n "$RUN" ] && [ -r "$LIVE_LOCK/run" ] &&
           [ "`cat "$LIVE_LOCK/run"`" = "$RUN" ]; then
            rm -f "$LIVE_LOCK/run" || return 1
            rmdir "$LIVE_LOCK" 2>/dev/null || return 1
        elif [ -z "$RUN" ]; then
            rmdir "$LIVE_LOCK" 2>/dev/null || return 1
        else
            return 1
        fi
        LOCK_OWNED=0
    fi
}
trap release_live_lock 0

for utility in cat cksum ls mkdir mv rm rmdir sleep sync; do
    command -v "$utility" >/dev/null 2>&1 || {
        echo "CarPlay-RGI log collection refused: required MMX utility is missing: $utility"
        exit 1
    }
done

if [ -L "$LOG_BASE" ]; then
    echo "CarPlay-RGI log collection refused: output path is a symbolic link: $LOG_BASE"
    exit 1
fi
mkdir -p "$LOG_BASE" || {
    echo "CarPlay-RGI log collection refused: could not create $LOG_BASE"
    exit 1
}
OUT=
if [ "$TRIGGER" = background ]; then
    case "${2:-}" in 01|02|03|04|05|06|07|08|09) RUN=$2 ;; *) exit 1 ;; esac
    OUT=$LOG_BASE/$RUN
    [ -d "$OUT" ] && [ ! -L "$OUT" ] && [ ! -L "$LIVE_LOCK" ] &&
        [ ! -L "$LIVE_LOCK/run" ] && [ -r "$LIVE_LOCK/run" ] &&
        [ "`cat "$LIVE_LOCK/run"`" = "$RUN" ] || exit 1
    # Keep this marker: even a completed reserved slot must never be rerun.
    mkdir "$OUT/.worker" || exit 1
    # on -d detaches parentage; ignoring HUP also survives the M.I.B. session.
    trap '' 1
else
    if [ "$TRIGGER" = arm ]; then
        if ! mkdir "$LIVE_LOCK" 2>/dev/null; then
            echo "CarPlay-RGI live capture already active or awaiting inspection. Check .live-active and the numbered capture; no new capture started."
            exit 1
        fi
        LOCK_OWNED=1
    fi
    for SLOT in 01 02 03 04 05 06 07 08 09; do
        if [ ! -e "$LOG_BASE/$SLOT" ] && [ ! -L "$LOG_BASE/$SLOT" ]; then
            OUT=$LOG_BASE/$SLOT
            mkdir "$OUT" || {
                echo "CarPlay-RGI log collection refused: could not create $OUT"
                exit 1
            }
            RUN=$SLOT
            break
        fi
    done
fi
[ -n "$OUT" ] || {
    echo "CarPlay-RGI log collection refused: all nine capture slots are occupied. Preserve them on the PC."
    exit 1
}
SUMMARY=$OUT/summary.txt

if [ "$TRIGGER" = arm ]; then
    echo "$RUN" > "$LIVE_LOCK/run" || {
        rm -f "$LIVE_LOCK/run"
        rmdir "$LIVE_LOCK" 2>/dev/null
        LOCK_OWNED=0
        exit 1
    }
fi
# The coordinator leaves the reserved summary for the worker to append to.
if [ "$TRIGGER" = background ]; then
    [ -f "$SUMMARY" ] && [ ! -L "$SUMMARY" ] || exit 1
else
    cat /dev/null > "$SUMMARY" || exit 1
fi

record()
{
    echo "$*"
    echo "$*" >> "$SUMMARY" || exit 1
}

collect_file()
{
    SOURCE_PATH=$1
    OUTPUT_NAME=$2
    # QNX /tmp may map to /dev/shmem, whose files can be advertised as
    # name-special (S_IFNAM) rather than regular files. Test readability,
    # not -f, or valid runtime logs can be falsely reported as missing.
    if [ ! -r "$SOURCE_PATH" ] || [ -L "$SOURCE_PATH" ]; then
        record "MISSING $SOURCE_PATH"
        return 0
    fi

    TEMP_PATH=$OUT/$OUTPUT_NAME.new.$$
    rm -f "$TEMP_PATH" || return 1
    if ! cat "$SOURCE_PATH" 2>> "$SUMMARY" > "$TEMP_PATH"; then
        rm -f "$TEMP_PATH"
        record "ERROR reading $SOURCE_PATH"
        return 1
    fi
    if ! mv -f "$TEMP_PATH" "$OUT/$OUTPUT_NAME"; then
        rm -f "$TEMP_PATH"
        record "ERROR publishing $OUTPUT_NAME"
        return 1
    fi
    FILE_ID=`cksum "$OUT/$OUTPUT_NAME" 2>> "$SUMMARY"` || {
        record "ERROR checksumming $OUTPUT_NAME"
        return 1
    }
    set -- $FILE_ID
    record "COPIED $SOURCE_PATH -> $OUTPUT_NAME ($1:$2)"
    return 0
}

# Run one read-only diagnostic with a five-second limit. Direct command launch
# makes PROBE_PID the process being stopped, not an intermediate shell. Output
# stays on the SD even after a timeout; do not follow or clear the system log.
collect_probe()
{
    PROBE_NAME=$1
    shift
    record "PROBE $PROBE_NAME: $* (limit: 5 s)"
    if [ "$TRIGGER" = live ] || [ "$TRIGGER" = background ]; then
        if command -v date >/dev/null 2>&1; then
            record "PROBE $PROBE_NAME unit clock: `date`"
        fi
    fi
    "$@" > "$OUT/$PROBE_NAME" 2>&1 &
    PROBE_PID=$!
    PROBE_TIMED_OUT=1
    for PROBE_SECOND in 1 2 3 4 5; do
        if ! kill -0 "$PROBE_PID" 2>/dev/null; then
            PROBE_TIMED_OUT=0
            break
        fi
        sleep 1
    done
    # Check again at the deadline so a command finishing in the last second
    # is not incorrectly reported as timed out.
    if [ "$PROBE_TIMED_OUT" = 1 ] && kill -0 "$PROBE_PID" 2>/dev/null; then
        kill -9 "$PROBE_PID" 2>/dev/null
        wait "$PROBE_PID" 2>/dev/null
        record "WARNING: $PROBE_NAME timed out after 5 s; partial output retained."
        COLLECT_ERROR=1
    else
        wait "$PROBE_PID"
        PROBE_RC=$?
        record "PROBE $PROBE_NAME exit status: $PROBE_RC"
        if [ "$PROBE_RC" != 0 ]; then
            record "WARNING: $PROBE_NAME failed; diagnostic output retained."
            COLLECT_ERROR=1
        fi
    fi
    PROBE_PID=
    if [ -f "$OUT/$PROBE_NAME" ]; then
        PROBE_ID=`cksum "$OUT/$PROBE_NAME" 2>> "$SUMMARY"` || {
            COLLECT_ERROR=1
            return 0
        }
        set -- $PROBE_ID
        record "CAPTURED $PROBE_NAME ($1:$2)"
    fi
}

# Each live query is a bounded snapshot. No indefinite system-log reader or
# shell resource limit is needed; the latter failed on the vehicle's QNX shell.
# Stop an active query if the collector is interrupted.
cleanup_probe()
{
    if [ -n "${PROBE_PID:-}" ]; then
        kill -KILL "$PROBE_PID" 2>/dev/null
        wait "$PROBE_PID" 2>/dev/null
        PROBE_PID=
    fi
}
finish_collector()
{
    COLLECT_RC=$?
    cleanup_probe
    if [ "$LOCK_OWNED" = 1 ]; then
        if ! release_live_lock || ! sync; then
            COLLECT_RC=1
            echo "ERROR: live reservation cleanup or its flush failed." >> "$SUMMARY"
        fi
    fi
    if [ "$TRIGGER" = background ]; then
        # RESULT is separate from the launch acknowledgement. All normal capture
        # writes and settling complete before exit 0; early exits stay failures.
        if [ "$COLLECT_RC" = 0 ] && [ "${COLLECTION_COMPLETE:-0}" != 1 ]; then
            COLLECT_RC=1
        fi
        if ! echo "$COLLECT_RC" > "$OUT/RESULT" || ! sync; then
            COLLECT_RC=1
            echo 1 > "$OUT/RESULT"
            sync
        fi
    fi
    trap - 0
    exit "$COLLECT_RC"
}
trap finish_collector 0
trap 'exit 1' 2 15
if [ "$TRIGGER" = background ]; then
    trap '' 1
    LOCK_OWNED=1
else
    trap 'exit 1' 1
fi

collect_live_activity()
{
    record "LIVE: keep CarPlay connected; reproduce the ticking now."
    record "LIVE: six observation rounds, with five ten-second gaps plus probe time."
    record "LIVE: system messages and sampled thread activity, not recorded sound or an audio-buffer trace."

    for LIVE_ROUND in 01 02 03 04 05 06; do
        record "LIVE round $LIVE_ROUND started."
        if command -v date >/dev/null 2>&1; then
            record "LIVE round $LIVE_ROUND unit clock: `date`"
        fi
        if command -v sloginfo >/dev/null 2>&1; then
            collect_probe "sloginfo-live-$LIVE_ROUND.txt" sloginfo -t
        else
            record "MISSING optional utility: sloginfo (live system messages unavailable)"
        fi
        if command -v pidin >/dev/null 2>&1; then
            for LIVE_PROCESS in io-audio audio_service maneuver_render; do
                collect_probe "live-$LIVE_ROUND-$LIVE_PROCESS-sched.txt" pidin -p "$LIVE_PROCESS" sched
                collect_probe "live-$LIVE_ROUND-$LIVE_PROCESS-ttimes.txt" pidin -p "$LIVE_PROCESS" ttimes
            done
        else
            record "MISSING optional utility: pidin (live thread activity unavailable)"
        fi
        if [ "$LIVE_ROUND" != 06 ]; then
            sleep 10 || { COLLECT_ERROR=1; break; }
        fi
    done

    record "LIVE: observation complete; collecting the final buffers and runtime files."
}

if [ "$TRIGGER" = arm ]; then
    record "ARMING: reserved capture $RUN for background observation."
    # The detached worker owns the reservation from this point. Preserve it on
    # an unacknowledged launch; don't guess whether a delayed child is still alive.
    LOCK_OWNED=0
    if ! (
        trap '' 1
        on -d -s -f mmx /bin/sh /net/mmx/fs/sda0/mod/command.sh background "$RUN" \
            < /dev/null > "$OUT/worker-output.txt" 2>&1
    ); then
        if [ ! -d "$OUT/.worker" ]; then
            LOCK_OWNED=1
            echo 1 > "$OUT/RESULT"
            sync
        fi
        record "ERROR: detached launch failed; inspect worker-output.txt."
        exit 1
    fi
    for ARM_SECOND in 0 1 2 3 4 5; do
        if [ -f "$OUT/STARTED" ] && [ ! -L "$OUT/STARTED" ]; then
            record "ARMED: capture $RUN. Return from Individual Script to CarPlay now."
            record "Recording begins after 30 seconds. Reproduce the ticking; leave the SD inserted and the unit on for at least three minutes."
            record "This is a launch acknowledgement, not collection success. Check RESULT and summary.txt afterwards."
            exit 0
        fi
        [ "$ARM_SECOND" != 5 ] || break
        sleep 1 || exit 1
    done
    record "ERROR: background startup was not acknowledged within five seconds. Reservation retained; inspect the capture before retrying."
    exit 1
fi

record "CarPlay-RGI runtime log collection started."
record "This collector writes only to the SD card."
record "Capture: $RUN (trigger: $TRIGGER)"
if command -v date >/dev/null 2>&1; then
    record "Unit clock: `date`"
fi

if [ "$TRIGGER" = background ]; then
    record "BACKGROUND: armed; allowing 30 seconds to leave M.I.B. before observation."
    echo "$$" > "$OUT/STARTED" || exit 1
    sleep 30 || exit 1
fi

if [ "$TRIGGER" = live ] || [ "$TRIGGER" = background ]; then
    collect_live_activity
fi

# QNX 6.5 uses slogger, confirmed in the v12 process capture. Keep the entire
# buffer: audio messages may use vendor-specific codes or omit audio keywords.
# -t supplies millisecond timestamps; no -c (clear) or -w (wait indefinitely).
# Capture early, before other probes can displace messages in the ring buffer.
if command -v sloginfo >/dev/null 2>&1; then
    collect_probe sloginfo-mmx.txt sloginfo -t
else
    record "MISSING optional utility: sloginfo (MMX system/audio messages unavailable)"
fi
if [ "$TRIGGER" = live ] || [ "$TRIGGER" = background ]; then
    record "SKIPPED live RCC system-log query: remote launch failed with ENOMEM in the vehicle capture; local MMX snapshots retained."
elif command -v on >/dev/null 2>&1; then
    collect_probe sloginfo-rcc.txt on -f rcc sloginfo -t
else
    record "MISSING optional utility: on (RCC system/audio messages unavailable)"
fi

collect_file /tmp/carplay_wrapper.log carplay_wrapper.log || COLLECT_ERROR=1
collect_file /tmp/maneuver_render.log maneuver_render.log || COLLECT_ERROR=1
collect_file /tmp/carplay_hook.log carplay_hook.log || COLLECT_ERROR=1
collect_file /tmp/carplay_hook.log.1 carplay_hook.log.1 || COLLECT_ERROR=1
collect_file /tmp/carplay_hook.log.2 carplay_hook.log.2 || COLLECT_ERROR=1
collect_file /tmp/carplay_hook.log.3 carplay_hook.log.3 || COLLECT_ERROR=1
collect_file /tmp/carplay_java.log carplay_java.log || COLLECT_ERROR=1
collect_file /tmp/carplay_java.log.1 carplay_java.log.1 || COLLECT_ERROR=1
collect_file /tmp/carplay_most_output carplay_most_output.txt || COLLECT_ERROR=1
collect_file /tmp/carplay_most_output_ready carplay_most_output_ready.txt || COLLECT_ERROR=1
# The renderer's latest settled maneuver frame, exactly as the MOST encoder
# gets it (binary PPM). Missing unless a CarPlay maneuver was shown since the renderer started.
collect_file /tmp/carplay_most_frame.ppm carplay_most_frame.ppm || COLLECT_ERROR=1
collect_file /ramdisk/pps/device/usb-1.0.1 usb-1.0.1.txt || COLLECT_ERROR=1

# dmdt is not on the MMX search path. gc listed the display contexts on the tested firmware; gs and gd may print nothing on some units. All three are
# display-manager queries (get), never a context switch.
DMDT=/eso/bin/apps/dmdt
if [ "$TRIGGER" = live ] || [ "$TRIGGER" = background ]; then
    record "SKIPPED live dmdt queries: these probes crashed during the vehicle capture; avoiding diagnostic disruption during guidance."
elif [ -x "$DMDT" ]; then
    "$DMDT" gc > "$OUT/dmdt-gc.txt" 2>&1
    record "OPTIONAL dmdt gc exit status: $?"
    "$DMDT" gs > "$OUT/dmdt-gs.txt" 2>&1
    record "OPTIONAL dmdt gs exit status: $? (output may be empty on this firmware)"
    "$DMDT" gd > "$OUT/dmdt-gd.txt" 2>&1
    record "OPTIONAL dmdt gd exit status: $? (output may be empty on this firmware)"
else
    record "MISSING optional utility: $DMDT"
fi

ls -l /tmp/carplay* > "$OUT/tmp-carplay-files.txt" 2>&1 ||
    record "NOTE: no /tmp/carplay* paths were listed."

ls -l \
    /mnt/app/root/hooks/carplay_startup.sh \
    /mnt/app/root/hooks/carplay_cleanup.sh \
    /mnt/app/root/hooks/carplay_processes.sh \
    /mnt/app/root/hooks/carplay_monitor.sh \
    /mnt/app/root/hooks/libcarplay_hook.so \
    /mnt/app/root/hooks/maneuver_render \
    /mnt/app/root/hooks/flag_atlas.rgba \
    /mnt/app/root/hooks/carplay_rgi_install.state \
    /mnt/app/carplay_verbose \
    /mnt/app/eso/hmi/lsd/jars/carplay_hook.jar \
    /mnt/system/etc/eso/production/smartphone_integrator.json \
    /mnt/system/etc/eso/production/dio_manager.json \
    > "$OUT/installed-files.txt" 2>&1 ||
    record "NOTE: one or more installed paths were not listed."

cat /dev/null > "$OUT/installed-cksum.txt" || COLLECT_ERROR=1
for INSTALLED_PATH in \
    /mnt/app/root/hooks/carplay_startup.sh \
    /mnt/app/root/hooks/carplay_cleanup.sh \
    /mnt/app/root/hooks/carplay_processes.sh \
    /mnt/app/root/hooks/carplay_monitor.sh \
    /mnt/app/root/hooks/libcarplay_hook.so \
    /mnt/app/root/hooks/maneuver_render \
    /mnt/app/root/hooks/flag_atlas.rgba \
    /mnt/app/root/hooks/carplay_rgi_install.state \
    /mnt/app/carplay_verbose \
    /mnt/app/eso/hmi/lsd/jars/carplay_hook.jar \
    /mnt/system/etc/eso/production/smartphone_integrator.json \
    /mnt/system/etc/eso/production/dio_manager.json
do
    if [ -f "$INSTALLED_PATH" ] && [ ! -L "$INSTALLED_PATH" ]; then
        cksum "$INSTALLED_PATH" >> "$OUT/installed-cksum.txt" 2>> "$SUMMARY" || COLLECT_ERROR=1
    else
        echo "MISSING $INSTALLED_PATH" >> "$OUT/installed-cksum.txt" || COLLECT_ERROR=1
    fi
done

if command -v pidin >/dev/null 2>&1; then
    pidin ar > "$OUT/processes.txt" 2>&1 || {
        record "NOTE: pidin ar failed."
        COLLECT_ERROR=1
    }
    # Free and total memory, for the renderer's render-target size.
    pidin info > "$OUT/memory.txt" 2>&1 || record "NOTE: pidin info failed."
    # One scheduling snapshot per relevant process, not a live CPU trace.
    # Missing/exited processes and unsupported queries remain visible in output.
    for AUDIO_PROCESS in io-audio audio_service maneuver_render; do
        collect_probe "$AUDIO_PROCESS-sched.txt" pidin -p "$AUDIO_PROCESS" sched
    done
else
    record "MISSING optional utility: pidin"
fi

if command -v netstat >/dev/null 2>&1; then
    netstat -an > "$OUT/netstat-an.txt" 2>&1 || record "NOTE: netstat -an failed."
    netstat -in > "$OUT/netstat-in.txt" 2>&1 || record "NOTE: netstat -in failed."
else
    record "MISSING optional utility: netstat"
fi

record "Collection content complete; allowing five seconds for SD writes to settle."
sync || {
    record "ERROR: filesystem flush request failed."
    exit 1
}
sleep 5 || {
    record "ERROR: storage settling delay failed."
    exit 1
}
sync || {
    record "ERROR: final filesystem flush request failed."
    exit 1
}

if [ "$COLLECT_ERROR" = 0 ]; then
    record "SUCCESS: runtime evidence copied to $OUT"
    sync || exit 1
    COLLECTION_COMPLETE=1
    exit 0
fi

record "PARTIAL: collection encountered one or more read or SD-write errors."
sync
exit 1
