#!/bin/sh
# M.I.B. release compatibility entry point. It may be sourced by the launcher.
# Keep the dispatcher in custom.sh, shared by both M.I.B. menu names.
if [ -r /net/mmx/fs/sda0/mod/custom.sh ]; then
    . /net/mmx/fs/sda0/mod/custom.sh
else
    echo "CarPlay-RGI refused: copy the complete prepared sdcard overlay."
    return 1
fi
