#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
#
# POSIX counterpart of testkit/check-ofx-artifact.bat. Checks the artifact a
# build produced, not the source it came from: an OFX module is loadable only if
# it exports exactly the two entry points, links only what the host provides or
# the bundle ships, and (for a CUDA build) carries device code in the image.
#
#   check-artifact.sh <plugin.ofx> [--cuda] [--strict]
#
#   nm -D --defined-only   ~ dumpbin /exports    : exactly the two entry points
#   readelf -d             ~ dumpbin /dependents : what must be found at load time
#   readelf -S / cuobjdump ~ cuobjdump -lelf     : device code is present
#
# The tools are looked up, not assumed to be on PATH: build.sh may run with a
# stripped environment, and a cross build puts nm/readelf under a target prefix.
# A missing tool is a skip by default and a failure under --strict, which is what
# build.sh passes: a build that could not inspect its own artifact must not
# report success.
#
# Exits 1 on a wrong export set, a missing libraries line or missing device code,
# 3 on a missing tool under --strict, 0 otherwise.
#
# Not yet exercised on a Linux host in this repository - the Windows twin has the
# measured evidence. Treat a first run as a review of the checks themselves.

set -u

OFX=""
WANT_CUDA=0
STRICT=0

usage() {
    echo "usage: check-artifact.sh <plugin.ofx> [--cuda] [--strict]" >&2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --cuda)   WANT_CUDA=1 ;;
        --strict) STRICT=1 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "error: unexpected argument \"$1\"" >&2; usage; exit 2 ;;
        *)  if [ -z "$OFX" ]; then OFX="$1"; else
                echo "error: unexpected argument \"$1\"" >&2; usage; exit 2
            fi ;;
    esac
    shift
done

if [ -z "$OFX" ]; then
    usage
    exit 2
fi
if [ ! -f "$OFX" ]; then
    echo "error: no such file: $OFX" >&2
    exit 1
fi

# ---- resolve a binutils tool -------------------------------------------------
# Order: $CROSS_COMPILE prefix (a cross build's nm is not the host nm), PATH,
# then the usual toolkit locations.
find_tool() {
    name="$1"
    if [ -n "${CROSS_COMPILE:-}" ] && command -v "${CROSS_COMPILE}${name}" >/dev/null 2>&1; then
        command -v "${CROSS_COMPILE}${name}"
        return 0
    fi
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
        return 0
    fi
    return 1
}

NM=$(find_tool nm) || NM=""
READELF=$(find_tool readelf) || READELF=""

if [ -z "$NM" ]; then
    if [ "$STRICT" = "1" ]; then
        echo "check-artifact: FAILED - nm not found"
        echo "  looked on PATH and under \$CROSS_COMPILE. Install binutils."
        exit 3
    fi
    echo "  artifact: skipped (nm not found on PATH)"
fi

# ---- exports ----------------------------------------------------------------
# nm -D lists the dynamic symbol table; --defined-only drops the undefined
# references that would otherwise inflate the count. A host resolves the module
# through exactly two symbols, and both must be strong text definitions: a third
# `T` means a leaked Support symbol and fails the build.
#
# Weak `W` template instantiations are counted separately and do not fail it.
# They are what `-fvisibility=hidden` alone leaves behind, the templates without
# a version script ship five or six of them, and counting them as exports made
# this check reject two correct bundles (2026-11-26) while the build's own count,
# which greps ' T ', said `2 (expected 2)`. The version script in
# templates/minimal-ofx/exports.map is what removes them.
if [ -n "$NM" ]; then
    exports=$("$NM" -D --defined-only "$OFX" 2>/dev/null | awk '$2 ~ /^[TtWw]$/ {print $2, $3}')
    strong=$(printf '%s\n' "$exports" | awk '$1 == "T" {print $2}')
    weak=$(printf '%s\n' "$exports" | awk '$1 == "W" {print $2}')
    count=$(printf '%s\n' "$strong" | grep -c . || true)

    missing=""
    printf '%s\n' "$strong" | grep -qx "OfxGetNumberOfPlugins" || missing="$missing OfxGetNumberOfPlugins"
    printf '%s\n' "$strong" | grep -qx "OfxGetPlugin" || missing="$missing OfxGetPlugin"
    if [ -n "$missing" ]; then
        echo "FAIL: not exported as a strong definition:$missing"
        exit 1
    fi
    if [ "$count" != "2" ]; then
        echo "FAIL: nm reports $count strong text exports, expected 2"
        printf '%s\n' "$strong"
        exit 1
    fi
    weak_count=$(printf '%s\n' "$weak" | grep -c . || true)
    if [ "$weak_count" = "0" ]; then
        echo "  exports: 2 (OfxGetNumberOfPlugins, OfxGetPlugin)"
    else
        echo "  exports: 2 (OfxGetNumberOfPlugins, OfxGetPlugin), plus $weak_count weak C++ helper(s)"
        echo "           a version script (templates/minimal-ofx/exports.map) drops the weak ones"
    fi
fi

# ---- dependents --------------------------------------------------------------
# What the loader must find. On Linux the bundle ships its libraries beside the
# .ofx and the host prepends that directory, so $ORIGIN entries are expected;
# a bare libcudart.so with no $ORIGIN is the failure this print is for.
if [ -n "$READELF" ]; then
    echo "  dependents:"
    "$READELF" -d "$OFX" 2>/dev/null | awk '/NEEDED/ {gsub(/[][]/, "", $5); print "    " $5}'
elif [ "$STRICT" = "1" ]; then
    echo "check-artifact: FAILED - readelf not found"
    exit 3
else
    echo "  dependents: skipped (readelf not found on PATH)"
fi

[ "$WANT_CUDA" = "1" ] || exit 0

# ---- device code -------------------------------------------------------------
# readelf -S finds the fatbin section in a host-linked image; cuobjdump -lelf
# names the cubins and is the closer analogue of the Windows check. Either is
# enough evidence that device code is in the file, so try readelf first and fall
# back to cuobjdump when it is installed.
found=0
if [ -n "$READELF" ]; then
    if "$READELF" -S "$OFX" 2>/dev/null | grep -q "nv_fatbin"; then
        echo "  device code: nv_fatbin section present"
        found=1
    fi
fi
if [ "$found" = "0" ]; then
    CUOBJDUMP=$(find_tool cuobjdump) || CUOBJDUMP=""
    if [ -n "$CUOBJDUMP" ]; then
        cubins=$("$CUOBJDUMP" -lelf "$OFX" 2>/dev/null | grep -c "\.cubin" || true)
        if [ "$cubins" != "0" ]; then
            echo "  device code: $cubins cubin(s)"
            found=1
        fi
    fi
fi

if [ "$found" = "0" ]; then
    if [ -z "$READELF" ] && [ "$STRICT" = "1" ]; then
        echo "check-artifact: FAILED - neither readelf nor cuobjdump found"
        exit 3
    fi
    echo "FAIL: --cuda was requested but the image contains no device code"
    exit 1
fi

exit 0
