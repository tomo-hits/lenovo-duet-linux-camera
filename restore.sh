#!/bin/sh
# SPDX-License-Identifier: MIT
# Keep this entry point usable with: sudo sh restore.sh
if ! command -v python3 >/dev/null 2>&1; then
    echo 'Python 3 is required. Install python3, then run this script again.' >&2
    exit 1
fi
exec python3 "$(dirname "$0")/lib/entrypoint.py" restore "$@"
