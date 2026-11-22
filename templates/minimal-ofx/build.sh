#!/usr/bin/env bash
#
# The reference build: one command, from a clean checkout, ending in proof.
#
#   templates/minimal-ofx/build.sh              Release
#   templates/minimal-ofx/build.sh Debug        another configuration
#   OFX_SOURCE_DIR=/path/to/openfx ./build.sh   offline / an existing checkout
#
# Step 1 fetches the OpenFX tag the repository pins (OFX_Release_1.5.1) into
# .ofx-ref/ when it is not there. Step 4 does not read the source: it inspects
# the produced .ofx - exported dynamic symbols with nm, ELF headers with
# readelf, then dlopen()s it and reads the OfxPlugin struct the registration
# hook produced.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "${here}/../.." && pwd)"
config="${1:-Release}"

case "$config" in
  Debug|Release|RelWithDebInfo|MinSizeRel) ;;
  *) echo "error: unknown configuration '$config'" >&2; exit 2 ;;
esac

die() { echo "FAIL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Headers
# ---------------------------------------------------------------------------
ofx_dir="${OFX_SOURCE_DIR:-${repo}/.ofx-ref}"
if [[ -f "${ofx_dir}/include/ofxImageEffect.h" && -f "${ofx_dir}/Support/Library/ofxsImageEffect.cpp" ]]; then
  echo "[1/4] OpenFX headers: ${ofx_dir}"
else
  echo "[1/4] OpenFX headers missing at ${ofx_dir}; fetching OFX_Release_1.5.1"
  command -v git >/dev/null 2>&1 || die "git is required to fetch the OpenFX headers, or set OFX_SOURCE_DIR"
  git -c advice.detachedHead=false clone -q --depth 1 --branch OFX_Release_1.5.1 \
    https://github.com/AcademySoftwareFoundation/openfx.git "${ofx_dir}" \
    || die "cannot fetch the OpenFX headers; set OFX_SOURCE_DIR to an existing checkout"
  [[ -f "${ofx_dir}/include/ofxImageEffect.h" ]] || die "${ofx_dir} is not an openfx checkout"
  echo "      fetched $(git -C "${ofx_dir}" describe --tags --always 2>/dev/null || echo 'OFX_Release_1.5.1')"
fi

# ---------------------------------------------------------------------------
# 2. Configure + build
# ---------------------------------------------------------------------------
echo "[2/4] configuring (${config})"
cmake -S "${here}" -B "${here}/build" \
  -DCMAKE_BUILD_TYPE="${config}" \
  -DOFX_SOURCE_DIR="${ofx_dir}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null

echo "[3/4] compiling"
cmake --build "${here}/build" --parallel

# ---------------------------------------------------------------------------
# 3. Find the artifact
# ---------------------------------------------------------------------------
ofx_file="$(find "${here}/build/bundle" -name '*.ofx' -type f | head -1)"
[[ -n "${ofx_file}" ]] || die "the build produced no .ofx inside build/bundle"

# ---------------------------------------------------------------------------
# 4. Prove the artifact, not the source
# ---------------------------------------------------------------------------
echo "[4/4] verifying ${ofx_file}"
file "${ofx_file}"

exports="$(nm -D --defined-only "${ofx_file}" | awk '$2 == "T" {print $3}' | sort)"
expected=$'OfxGetNumberOfPlugins\nOfxGetPlugin'
if [[ "${exports}" != "${expected}" ]]; then
  echo "  exported text symbols:" >&2
  printf '    %s\n' "${exports:-<none>}" >&2
  echo "  expected exactly: ${expected//$'\n'/ and }" >&2
  die "wrong set of exported entry points"
fi
echo "  exports        : $(echo "${exports}" | tr '\n' ' ')"

# Strong T symbols are what the loader resolves; weak W symbols are also visible
# in the dynamic table, so count every defined dynamic symbol. The version script
# (exports.map) is what keeps this at 2: -fvisibility=hidden alone leaves five
# weak std::vector<OFX::...> helpers exported.
defined_total="$(nm -D --defined-only "${ofx_file}" | wc -l)"
echo "  dynamic symbols: ${defined_total} defined in total"
[[ "${defined_total}" == "2" ]] || die "the dynamic symbol table has ${defined_total} defined symbols, expected exactly 2"

# OfxSetHost is declared in ofxCore.h:577 but the Support library does not
# define it; the host hands itself over through OfxPlugin::setHost instead.
set_host_syms="$(nm -D --defined-only "${ofx_file}" | grep -c 'OfxSetHost' || true)"
[[ "${set_host_syms}" == "0" ]] || die "OfxSetHost is exported; its absence is what the playbook documents"

undef_ofx="$(nm -D -u "${ofx_file}" | awk '{print $NF}' | grep -E '^(Ofx|OFX)' || true)"
[[ -z "${undef_ofx}" ]] || die "undefined OFX symbols in the module: ${undef_ofx}"
echo "  undefined OFX  : none (all host access goes through fetchSuite at runtime)"

rpath="$(readelf -d "${ofx_file}" | grep -E 'RPATH|RUNPATH' || true)"
if [[ -n "${rpath}" ]]; then
  echo "  ${rpath}"
  # shellcheck disable=SC2016  # matching the literal text $ORIGIN, not expanding it
  [[ "${rpath}" == *'$ORIGIN'* ]] || die "the module records a build-machine library path"
fi
echo "  RPATH/RUNPATH  : ${rpath:+$ORIGIN-based}${rpath:-none}"

# The registration hook is only real if a loader can call it. dlopen() the
# artifact, call the entry point, and read the struct it returns.
command -v python3 >/dev/null 2>&1 || die "python3 is required for the registration check"
python3 - "${ofx_file}" <<'PY'
import ctypes
import sys

class OfxPlugin(ctypes.Structure):
    _fields_ = [
        ("pluginApi", ctypes.c_char_p),
        ("apiVersion", ctypes.c_int),
        ("pluginIdentifier", ctypes.c_char_p),
        ("pluginVersionMajor", ctypes.c_uint),
        ("pluginVersionMinor", ctypes.c_uint),
        ("setHost", ctypes.c_void_p),
        ("mainEntry", ctypes.c_void_p),
    ]

lib = ctypes.CDLL(sys.argv[1])
lib.OfxGetNumberOfPlugins.restype = ctypes.c_int
lib.OfxGetPlugin.restype = ctypes.POINTER(OfxPlugin)
lib.OfxGetPlugin.argtypes = [ctypes.c_int]

count = lib.OfxGetNumberOfPlugins()
if count != 1:
    sys.exit(f"FAIL: OfxGetNumberOfPlugins() = {count}, expected 1")

plugin = lib.OfxGetPlugin(0).contents
print(f"  dlopen         : OfxGetNumberOfPlugins() = {count}")
print(f"  pluginApi      : {plugin.pluginApi.decode()} (v{plugin.apiVersion})")
print(f"  identifier     : {plugin.pluginIdentifier.decode()}")
print(f"  version        : {plugin.pluginVersionMajor}.{plugin.pluginVersionMinor}")
print(f"  setHost        : {'present' if plugin.setHost else 'NULL'}")
print(f"  mainEntry      : {'present' if plugin.mainEntry else 'NULL'}")

# ofxImageEffect.h:26 defines kOfxImageEffectPluginApi as
# "OfxImageEffectPluginAPI" (no trailing 'Plugin'), and ofxsImageEffect.cpp:3021
# copies it into OfxPlugin::pluginApi with apiVersion 1 (:3022).
problems = []
if plugin.pluginApi != b"OfxImageEffectPluginAPI":
    problems.append(f"pluginApi is {plugin.pluginApi!r}, expected b'OfxImageEffectPluginAPI'")
if plugin.apiVersion != 1:
    problems.append(f"apiVersion is {plugin.apiVersion}, expected 1")
if plugin.pluginIdentifier != b"com.ofxplugindeveloper.referencegain":
    problems.append(f"identifier is {plugin.pluginIdentifier!r}")
if plugin.pluginVersionMajor != 1:
    problems.append(f"major version is {plugin.pluginVersionMajor}")
if not plugin.setHost:
    problems.append("OfxPlugin::setHost is NULL")
if not plugin.mainEntry:
    problems.append("OfxPlugin::mainEntry is NULL")
if problems:
    sys.exit("FAIL: " + "; ".join(problems))
PY

echo
echo "OK: ${ofx_file}"
echo "    bundle: $(dirname "$(dirname "$(dirname "${ofx_file}")")")"
