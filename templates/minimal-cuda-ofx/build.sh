#!/usr/bin/env bash
#
# Out-of-source build for the minimal CUDA OFX template.
#
#   ./build.sh                        Release, CUDA if a toolkit is found
#   ./build.sh Debug                  Debug
#   ./build.sh Release install        build, then copy the bundle into the
#                                     host's OFX plugin directory
#   ./build.sh --bootstrap-cuda       fetch a real nvcc WITHOUT root, then build
#   CPU_ONLY=1 ./build.sh             build only the CPU path (no CUDA)
#
# Common overrides (environment):
#   OFX_SOURCE_DIR         existing openfx checkout (skips the network fetch);
#                          can point at Support/Developer/OpenFX inside Resolve
#   CUDA_HOME              toolkit to use; else discovered
#   CUDA_ARCHS             e.g. "86;89" (default 75;80;86;89;90[;100;120])
#   OFX_CUDA_TOOLKIT_DIR   where --bootstrap-cuda installs (default
#                          ${XDG_CACHE_HOME:-$HOME/.cache}/ofx-cuda-toolkit)
#   CUDA_VER               toolkit series to bootstrap (default 12.9)
#   CUDA_REPO              apt repo URL for the bootstrap (default ubuntu2404)
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${here}/build"

config="Release"
do_install=0
do_bootstrap=0

usage() {
  sed -n '2,24p' "${BASH_SOURCE[0]}" | sed 's/^#\{1,2\} \{0,1\}//'
}

for arg in "$@"; do
  case "$arg" in
    Debug|Release|RelWithDebInfo|MinSizeRel) config="$arg" ;;
    install)        do_install=1 ;;
    --bootstrap-cuda) do_bootstrap=1 ;;
    -h|--help)      usage; exit 0 ;;
    *) echo "error: unknown argument '$arg'" >&2; usage >&2; exit 2 ;;
  esac
done

die() { echo "error: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# CUDA toolkit discovery
#
# Order matters: an explicit CUDA_HOME wins, then a toolkit already on PATH,
# then the conventional install locations, then one bootstrapped by this script.
# ---------------------------------------------------------------------------
cuda_toolkit_dir="${OFX_CUDA_TOOLKIT_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/ofx-cuda-toolkit}"
cuda_ver="${CUDA_VER:-12.9}"
cuda_suffix="${cuda_ver//./-}"          # 12.9 -> 12-9
cuda_repo="${CUDA_REPO:-https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64}"

find_cuda_toolkit() {
  if [[ -n "${CUDA_HOME:-}" && -x "${CUDA_HOME}/bin/nvcc" ]]; then
    printf '%s\n' "$CUDA_HOME"; return 0
  fi
  if command -v nvcc >/dev/null 2>&1; then
    local p; p="$(command -v nvcc)"
    printf '%s\n' "$(dirname "$(dirname "$p")")"; return 0
  fi
  local candidate
  for candidate in /usr/local/cuda /usr/local/cuda-* /opt/cuda "${cuda_toolkit_dir}/home"; do
    if [[ -x "${candidate}/bin/nvcc" ]]; then printf '%s\n' "$candidate"; return 0; fi
  done
  return 1
}

# ---------------------------------------------------------------------------
# Bootstrap: a real nvcc + CUDA runtime headers, no root, no system packages.
#
# The official NVIDIA apt packages are plain .deb archives; they are fetched and
# unpacked with ar(1) + tar(1) into a private prefix and assembled into a
# synthetic CUDA_HOME. Verified working: CUDA 12.9.41 with GCC 13.3 on
# Ubuntu 24.04, producing a .ofx that links libcudart.so.12.
#
# Note: the pip wheels (nvidia-cuda-nvcc-cu12) are NOT sufficient - that wheel
# ships ptxas and the NVVM libraries but no nvcc/cudafe++/nvlink, so it cannot
# compile a translation unit on its own.
# ---------------------------------------------------------------------------
bootstrap_cuda() {
  local home="${cuda_toolkit_dir}/home"
  if [[ -x "${home}/bin/nvcc" ]]; then
    echo "[bootstrap] reusing ${home}"
    return 0
  fi

  command -v curl >/dev/null 2>&1 || die "bootstrap needs curl"
  command -v ar   >/dev/null 2>&1 || die "bootstrap needs binutils (ar)"
  command -v tar  >/dev/null 2>&1 || die "bootstrap needs tar"
  command -v gzip >/dev/null 2>&1 || die "bootstrap needs gzip"

  local debs="${cuda_toolkit_dir}/debs"
  local stage="${cuda_toolkit_dir}/stage"
  mkdir -p "$debs" "$stage"

  echo "[bootstrap] NVIDIA CUDA ${cuda_ver} compiler + runtime into ${cuda_toolkit_dir}"
  curl -fsSL -o "${debs}/Packages.gz" "${cuda_repo}/Packages.gz" \
    || die "cannot fetch ${cuda_repo}/Packages.gz - set CUDA_REPO for your distro, or install the CUDA toolkit and set CUDA_HOME"
  gzip -dc "${debs}/Packages.gz" > "${debs}/Packages"

  local pkg filename base
  for pkg in "cuda-nvcc-${cuda_suffix}" \
             "cuda-cudart-${cuda_suffix}" \
             "cuda-cudart-dev-${cuda_suffix}" \
             "cuda-nvvm-${cuda_suffix}" \
             "cuda-crt-${cuda_suffix}"; do
    filename="$(awk -v want="$pkg" '
      BEGIN { RS=""; FS="\n" }
      { name=""; file="" }
      { for (i=1;i<=NF;i++) {
          if ($i ~ /^Package: /)  name=substr($i,10);
          if ($i ~ /^Filename: /) file=substr($i,11);
        }
        if (name==want) { print file; exit } }' "${debs}/Packages")"

    [[ -n "$filename" ]] || die "package '$pkg' not found in the CUDA ${cuda_ver} repo (try CUDA_VER=12.8 or CUDA_REPO=...)"

    base="$(basename "$filename")"
    echo "[bootstrap]   ${base}"
    curl -fsSL -o "${debs}/${base}" "${cuda_repo}/${filename#./}" || die "download failed: ${base}"

    ( cd "$debs" && ar x "$base" ) || die "cannot unpack ${base} (is binutils installed?)"
    local data=""
    local candidate
    for candidate in "${debs}"/data.tar.*; do
      [[ -e "${candidate}" ]] && { data="${candidate}"; break; }
    done
    [[ -n "$data" ]] || die "${base} has no data.tar payload"
    tar -xf "$data" -C "$stage" || die "cannot extract ${data}"
    rm -f "${debs}"/data.tar.*
  done

  local installed
  installed="$(find "${stage}/usr/local" -maxdepth 1 -type d -name 'cuda-*' | head -1)"
  [[ -n "$installed" ]] || die "bootstrap produced no cuda-* directory"

  ln -sfn "$installed" "$home"
  [[ -x "${home}/bin/nvcc" ]] || die "bootstrap finished but ${home}/bin/nvcc is missing"

  echo "[bootstrap] done: $("${home}/bin/nvcc" --version | tail -1)"
  echo "[bootstrap] note: libcudart lives in ${home}/lib64; put it on LD_LIBRARY_PATH when"
  echo "[bootstrap]       loading the .ofx outside a host that ships its own CUDA runtime."
}

# ---------------------------------------------------------------------------
# Resolve the toolkit and decide what we are building
# ---------------------------------------------------------------------------
cuda_home=""
if [[ "${CPU_ONLY:-0}" == "1" ]]; then
  echo "[build] CPU_ONLY=1: building without the CUDA path"
else
  if ! cuda_home="$(find_cuda_toolkit)"; then
    cuda_home=""
    if [[ "$do_bootstrap" == "1" ]]; then
      bootstrap_cuda
      cuda_home="${cuda_toolkit_dir}/home"
    else
      cat >&2 <<EOF
error: no CUDA toolkit found, so the CUDA render path cannot be compiled.

  A working nvcc is required to build the plugin's GPU path. Either:

    1. bootstrap a real nvcc without root (verified, CUDA ${cuda_ver}, no GPU needed):

         ./build.sh --bootstrap-cuda

    2. point at an existing toolkit:

         CUDA_HOME=/usr/local/cuda ./build.sh

    3. build only the CPU path, on purpose:

         CPU_ONLY=1 ./build.sh

  Searched: \$CUDA_HOME, PATH, /usr/local/cuda*, /opt/cuda,
            ${cuda_toolkit_dir}/home
EOF
      exit 2
    fi
  fi
  echo "[build] CUDA toolkit: ${cuda_home}"
fi

# ---------------------------------------------------------------------------
# Configure + build
# ---------------------------------------------------------------------------
cmake_args=(-S "${here}" -B "${build_dir}" "-DCMAKE_BUILD_TYPE=${config}")

if [[ -n "$cuda_home" ]]; then
  cmake_args+=("-DCMAKE_CUDA_COMPILER=${cuda_home}/bin/nvcc")
  cmake_args+=("-DCUDAToolkit_ROOT=${cuda_home}")
  cmake_args+=("-DOFXPLUGIN_ENABLE_CUDA=ON")
else
  cmake_args+=("-DOFXPLUGIN_ENABLE_CUDA=OFF")
fi

# Plain ifs, not `cond && cmd`: under `set -e` a false test in an AND-list
# aborts the whole script.
if [[ -n "${OFX_SOURCE_DIR:-}" ]]; then
  cmake_args+=("-DOFX_SOURCE_DIR=${OFX_SOURCE_DIR}")
fi
if [[ -n "${CUDA_ARCHS:-}" ]]; then
  cmake_args+=("-DCMAKE_CUDA_ARCHITECTURES=${CUDA_ARCHS}")
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}" --config "${config}" --parallel

if [[ "$do_install" == "1" ]]; then
  cmake --build "${build_dir}" --config "${config}" --target install-bundle
fi

bundle="${build_dir}/bundle/MinimalCudaGain.ofx.bundle"
echo
echo "Bundle: ${bundle}"
find "${bundle}" -name '*.ofx' -type f -printf '  %p\n' 2>/dev/null || true

# Immediate, cheap proof that the intended path was actually compiled in.
ofx_file="$(find "${bundle}" -name '*.ofx' -type f | head -1)"
if [[ -n "$ofx_file" ]]; then
  exported="$(nm -D --defined-only "$ofx_file" 2>/dev/null | grep -c ' T ' || true)"
  echo "  exported dynamic symbols: ${exported} (expected 2)"
  # Note: no `grep -q` here. With `set -o pipefail`, grep -q exits early, nm dies
  # of SIGPIPE, and the pipeline reports failure even on a match - which made
  # this check report "ABSENT" for a binary that clearly had the launcher.
  launcher_syms="$(nm "$ofx_file" 2>/dev/null | grep -c 'LaunchGainRGBA32F' || true)"
  if [[ "${launcher_syms:-0}" -gt 0 ]]; then
    echo "  CUDA launcher: present (${launcher_syms} symbol(s))"
  else
    echo "  CUDA launcher: ABSENT (CPU-only build)"
  fi
fi
