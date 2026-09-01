#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stephen Olesen

set -eu

binary=${1:?usage: test_lifecycle.sh /path/to/i2ckiss}
test_dir=$(mktemp -d /tmp/i2ckiss-lifecycle-XXXXXX)
endpoint=$test_dir/tnc
logfile=$test_dir/i2ckiss.log
pid=

cleanup()
{
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -f "$endpoint" "$test_dir/i2ckiss-999-10.lock" "$logfile"
    rmdir "$test_dir" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

"$binary" --no-reset --retry-min-ms 10 --retry-max-ms 20 \
    --lock-dir "$test_dir" 999 0x10 symlink "$endpoint" \
    >"$logfile" 2>&1 &
pid=$!

attempt=0
while [ ! -L "$endpoint" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ] || ! kill -0 "$pid" 2>/dev/null; then
        echo "bridge did not publish its PTY" >&2
        exit 1
    fi
    sleep 0.05
done

# A transient consumer opens and closes the slave. The bridge and the same PTY
# must remain available for the next consumer.
(
    exec 3<>"$endpoint"
    exec 3>&-
)
sleep 0.1
kill -0 "$pid"
test -e "$endpoint"

kill -TERM "$pid"
wait "$pid"
pid=
test ! -L "$endpoint"

echo "lifecycle test passed"
