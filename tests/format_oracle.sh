#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
testbin=${1:?native format test required}
module=${2:?production SO required}
checker=${3:-}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
"$testbin" "$module" "$work/hfsplus.img" 4 512
"$testbin" "$module" "$work/hfsx.img" 5 4096
if [ -n "$checker" ]; then
    "$checker" -f -n "$work/hfsplus.img"
    "$checker" -f -n "$work/hfsx.img"
fi
