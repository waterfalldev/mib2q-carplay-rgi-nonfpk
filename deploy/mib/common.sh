check_file() {
    file_path=$1
    [ -f "$file_path" ] && [ ! -L "$file_path" ] || return 1
    expected_crc=$2
    expected_size=$3
    # Optional. When set, a mismatch is logged with what was actually found,
    # which on a unit with no shell access is the only diagnostic there is.
    # The rollback ownership helpers deliberately pass nothing: they try several
    # identities in turn, so a failed attempt there is not a finding.
    check_label=$4
    identity=`cksum "$file_path" 2>> "$LOG"` || {
        [ -z "$check_label" ] || log "MISMATCH $check_label: cannot read $file_path"
        return 1
    }
    set -- $identity
    if [ "$1" != "$expected_crc" ] || [ "$2" != "$expected_size" ]; then
        [ -z "$check_label" ] || log "MISMATCH $check_label: $file_path is $1:$2, expected $expected_crc:$expected_size"
        return 1
    fi
    return 0
}

# First field of ls -l, i.e. the permission string. Only ever compared with
# another string from the same ls, so no assumption is made about its format.
mode_of() {
    mode_listing=`ls -l "$1" 2>> "$LOG"` || return 1
    set -- $mode_listing
    echo "$1"
}

# Proves the partition can actually be written before anything on it is changed.
# Without this, a read-only mount is only discovered once files have already
# been committed, which needs a rollback instead of a clean refusal.
probe_writable() {
    probe_path=$1
    rm -f "$probe_path" 2>> "$LOG" || return 1
    # Not ": >". A redirection failure on a POSIX special built-in terminates a
    # non-interactive shell outright, so a read-only partition would end the
    # script before the die below could explain itself. An external command
    # fails the ordinary way. stderr is redirected before stdout so the failure
    # text reaches the log rather than the M.I.B. console.
    cat /dev/null 2>> "$LOG" > "$probe_path" || return 1
    rm -f "$probe_path" 2>> "$LOG" || return 1
    [ ! -e "$probe_path" ] || return 1
    return 0
}

# Everything that keeps a pre-existing file's permissions rests on this unit's
# cp being able to preserve them and on ls reporting them, so both are proved
# against the real file here, before anything is changed. Discovering it at
# staging time would also be safe, but this says so plainly and early.
probe_preserve() {
    model_path=$1
    probe_path=$2
    rm -f "$probe_path" 2>> "$LOG" || return 1
    cp -p "$model_path" "$probe_path" 2>> "$LOG" || return 1
    model_mode=`mode_of "$model_path"` || return 1
    probe_mode=`mode_of "$probe_path"` || return 1
    rm -f "$probe_path" 2>> "$LOG" || return 1
    [ ! -e "$probe_path" ] || return 1
    if [ "$model_mode" != "$probe_mode" ]; then
        log "MISMATCH mode probe: copy of $model_path came out $probe_mode, not $model_mode"
        return 1
    fi
    return 0
}

# The two configuration files already exist on the unit and their stock
# permissions were never recorded off the car, so nothing here invents a mode:
# the staged copy is created from the live file so it inherits that file's bits,
# the contents are then rewritten in place, and the two modes are compared
# before the rename. Install and rollback both leave the permission bits exactly
# as the car had them.
stage_config() {
    source_path=$1
    temp_path=$2
    model_path=$3
    expected_crc=$4
    expected_size=$5
    description=$6

    rm -f "$temp_path" || return 1
    cp -p "$model_path" "$temp_path" 2>> "$LOG" || return 1
    cat "$source_path" 2>> "$LOG" > "$temp_path" || return 1
    model_mode=`mode_of "$model_path"` || return 1
    temp_mode=`mode_of "$temp_path"` || return 1
    if [ "$model_mode" != "$temp_mode" ]; then
        log "MISMATCH $description: staged mode $temp_mode does not match live mode $model_mode"
        return 1
    fi
    check_file "$temp_path" "$expected_crc" "$expected_size" "$description" || return 1
    return 0
}
missing_state_record() {
    # grep returns 1 for no matches; a read error must not look like absence.
    [ "`grep -c "^${1}=" "$STATE" 2>> "$LOG"`" = "0" ]
}
