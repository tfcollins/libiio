#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Run a command against an iiod-emu instance serving <xml> on 127.0.0.1:30431.
# Usage: with_emu.sh <xml> <cmd...>
# Env:   IIOD_EMU  path to the iiod-emu binary (default: iiod-emu from PATH)
#
# Exits with the command's status. The emulator's output is shown only when
# something fails.
set -u

PORT=30431

if [ $# -lt 2 ]; then
	echo "usage: $0 <xml> <cmd...>" >&2
	exit 2
fi
xml=$1
shift
emu=${IIOD_EMU:-iiod-emu}

if nc -z 127.0.0.1 "$PORT" 2>/dev/null; then
	echo "$0: 127.0.0.1:$PORT is already in use; refusing to test against it" >&2
	exit 1
fi

log=$(mktemp "${TMPDIR:-/tmp}/with_emu.XXXXXX") || exit 1
"$emu" -p "$PORT" "emu:$xml" >"$log" 2>&1 &
pid=$!

cleanup() {
	kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
	rm -f "$log"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

fail() {
	echo "$0: $*" >&2
	cat "$log" >&2
	exit 1
}

# Wait up to 10 s for the emulator to accept connections.
tries=0
while ! nc -z 127.0.0.1 "$PORT" 2>/dev/null; do
	kill -0 "$pid" 2>/dev/null || fail "iiod-emu exited before listening"
	tries=$((tries + 1))
	[ "$tries" -ge 100 ] && fail "iiod-emu not listening on port $PORT after 10 s"
	sleep 0.1
done

"$@"
rc=$?

if [ "$rc" -ne 0 ]; then
	echo "$0: command exited with $rc; iiod-emu log follows" >&2
	cat "$log" >&2
fi

exit "$rc"
