#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Run the api ctest label through daqiri:, once per XML: each run gets its own
# iiod-emu (with_emu.sh) and TESTS_API_URI pointing at it.
# Usage: run_loopback.sh <build-dir> [<xml-dir>]
# Env:   IIOD_EMU             see with_emu.sh
#        DAQIRI_LOOPBACK_URI  URI under test (default daqiri:127.0.0.1,framing=iiod);
#                             set it to ip:127.0.0.1 for the control run
#
# Stops at the first XML whose api tests fail and exits non-zero.
set -u

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
	echo "usage: $0 <build-dir> [<xml-dir>]" >&2
	exit 2
fi

here=$(cd "$(dirname "$0")" && pwd)
build=$1
xmls=${2:-$here/../resources/xmls}
uri=${DAQIRI_LOOPBACK_URI:-daqiri:127.0.0.1,framing=iiod}

found=0
for xml in "$xmls"/*.xml; do
	[ -e "$xml" ] || continue
	found=1

	echo "== $(basename "$xml") via $uri"
	# -L is a regex: match the api label only, not daqiri-loopback.
	if ! (cd "$build" && "$here/with_emu.sh" "$xml" \
		env TESTS_API_URI="$uri" ctest -L '^api$' -j1 --output-on-failure); then
		echo "$0: api tests failed for $xml via $uri" >&2
		exit 1
	fi
done

if [ "$found" -eq 0 ]; then
	echo "$0: no XML files in $xmls" >&2
	exit 1
fi
