#!/usr/bin/env bash
#
# Out-of-source build for the minimal CUDA OFX template.
#
#   ./build.sh                       # Release
#   ./build.sh Debug                 # Debug
#   ./build.sh Release install       # build, then copy the bundle into the
#                                    # host's OFX plugin directory
#   OFX_SOURCE_DIR=/path/to/openfx ./build.sh    # skip the network fetch
#   CUDA_ARCHS="86;89" ./build.sh                # override the arch list
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${here}/build"
config="${1:-Release}"

cmake_args=(-S "${here}" -B "${build_dir}" "-DCMAKE_BUILD_TYPE=${config}")

if [[ -n "${OFX_SOURCE_DIR:-}" ]]; then
  cmake_args+=("-DOFX_SOURCE_DIR=${OFX_SOURCE_DIR}")
fi
if [[ -n "${CUDA_ARCHS:-}" ]]; then
  cmake_args+=("-DCMAKE_CUDA_ARCHITECTURES=${CUDA_ARCHS}")
fi
if [[ "${CPU_ONLY:-0}" == "1" ]]; then
  cmake_args+=("-DOFXPLUGIN_ENABLE_CUDA=OFF")
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}" --config "${config}" --parallel

if [[ "${2:-}" == "install" ]]; then
  cmake --build "${build_dir}" --config "${config}" --target install-bundle
fi

echo
echo "Bundle: ${build_dir}/bundle/MinimalCudaGain.ofx.bundle"
find "${build_dir}/bundle" -name "*.ofx" -type f -printf '  %p\n' 2>/dev/null || true
