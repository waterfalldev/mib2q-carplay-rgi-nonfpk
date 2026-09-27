#!/bin/sh

# Shared CarPlay-RGI runtime log collector. The package build includes this exact
# file in its installer directory. Before a rollback the dispatcher runs it on MMX
# with the SD writable, in the foreground and bounded by its own watchdog, so the
# volatile /tmp logs reach the SD card before the uninstall.
case "${1:-}" in
    before-rollback) ;;
    *)
        echo "[RGI] ERROR! Unknown argument: ${1:-(none)}"
        return 1 2>/dev/null || exit 1
        ;;
esac

PATH=${PATH:+$PATH:}/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/mnt/app/armle/bin:/mnt/app/armle/sbin:/mnt/app/armle/usr/bin:/mnt/app/armle/usr/sbin
export PATH

LOG_BASE=/net/mmx/fs/sda0/mod/carplay-rgi-runtime-logs
RUN=
COLLECT_ERROR=0
TRIGGER=$1

for utility in cat cksum ls mkdir mv rm sleep sync; do
    command -v "$utility" >/dev/null 2>&1 || {
        echo "[RGI] ERROR! Missing MMX tool: $utility"
        exit 1
    }
done

if [ -L "$LOG_BASE" ]; then
    echo "[RGI] ERROR! Log folder is a symbolic link: $LOG_BASE"
    exit 1
fi
mkdir -p "$LOG_BASE" || {
    echo "[RGI] ERROR! Could not create $LOG_BASE"
    exit 1
}
OUT=
for SLOT in 01 02 03 04 05 06 07 08 09; do
    if [ ! -e "$LOG_BASE/$SLOT" ] && [ ! -L "$LOG_BASE/$SLOT" ]; then
        OUT=$LOG_BASE/$SLOT
        mkdir "$OUT" || {
            echo "[RGI] ERROR! Could not create $OUT"
            exit 1
        }
        RUN=$SLOT
        break
    fi
done
[ -n "$OUT" ] || {
    echo "[RGI] ERROR! All 9 capture slots are full. Copy them to the PC, then clear them."
    exit 1
}
SUMMARY=$OUT/summary.txt
cat /dev/null > "$SUMMARY" || exit 1

# Detail goes to summary.txt only; the M.I.B. screen gets [RGI] lines.
record()
{
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

# The same five-second limit for a diagnostic whose failure or overrun is only
# noted in the summary, not reported as a partial capture.
collect_optional_probe()
{
    OPTIONAL_COLLECT_ERROR=$COLLECT_ERROR
    collect_probe "$@"
    COLLECT_ERROR=$OPTIONAL_COLLECT_ERROR
}

# Stop an active query if the collector is interrupted (the dispatcher's watchdog).
finish_collector()
{
    COLLECT_RC=$?
    if [ -n "${PROBE_PID:-}" ]; then
        kill -KILL "$PROBE_PID" 2>/dev/null
        wait "$PROBE_PID" 2>/dev/null
        PROBE_PID=
    fi
    trap - 0
    exit "$COLLECT_RC"
}
trap finish_collector 0
trap 'exit 1' 1 2 15

record "CarPlay-RGI runtime log collection started."
record "This collector writes only to the SD card."
record "Capture: $RUN (trigger: $TRIGGER)"
if command -v date >/dev/null 2>&1; then
    record "Unit clock: `date`"
fi

# QNX 6.5 uses slogger, confirmed in the v12 process capture. Keep the entire
# buffer: audio messages may use vendor-specific codes or omit audio keywords.
# -t supplies millisecond timestamps; no -c (clear) or -w (wait indefinitely).
# Capture early, before other probes can displace messages in the ring buffer.
# MMX and RCC share this log: v18-v22's `on -f rcc sloginfo -t` returned the same
# lines, read a moment later.
if command -v sloginfo >/dev/null 2>&1; then
    collect_probe sloginfo-mmx.txt sloginfo -t
else
    record "MISSING optional utility: sloginfo (MMX system/audio messages unavailable)"
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
if [ -x "$DMDT" ]; then
    # Output may be empty for gs and gd on this firmware.
    collect_optional_probe dmdt-gc.txt "$DMDT" gc
    collect_optional_probe dmdt-gs.txt "$DMDT" gs
    collect_optional_probe dmdt-gd.txt "$DMDT" gd
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
    # No process list: `pidin ar` records every command line, and those carry the
    # vehicle's identifiers into logs that get shared.
    # Free and total memory, for the renderer's render-target size.
    collect_optional_probe memory.txt pidin info
    # One scheduling snapshot per relevant process, not a live CPU trace.
    # Missing/exited processes and unsupported queries remain visible in output.
    for AUDIO_PROCESS in io-audio audio_service maneuver_render; do
        collect_probe "$AUDIO_PROCESS-sched.txt" pidin -p "$AUDIO_PROCESS" sched
    done
else
    record "MISSING optional utility: pidin"
fi

if command -v netstat >/dev/null 2>&1; then
    collect_optional_probe netstat-an.txt netstat -an
    collect_optional_probe netstat-in.txt netstat -in
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
    echo "[RGI] Logs saved to $OUT"
    sync || exit 1
    exit 0
fi

record "PARTIAL: collection encountered one or more read or SD-write errors."
echo "[RGI] ERROR! Logs only partly saved (see $SUMMARY)."
sync
exit 1
