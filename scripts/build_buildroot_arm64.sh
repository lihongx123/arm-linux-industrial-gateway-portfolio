#!/usr/bin/env bash
set -euo pipefail

# Buildroot deliberately rejects PATH entries containing whitespace. WSL adds
# Windows paths by default, so keep this build hermetic and Linux-only.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
buildroot_dir="${BUILDROOT_DIR:-/tmp/buildroot-2025.02.18}"
output_dir="${BUILDROOT_OUTPUT_DIR:-/tmp/mqmgateway-br-output}"
cross_build_dir="${ARM64_BUILD_DIR:-/tmp/mqmgateway-arm64-build}"
result_dir="$repo_dir/results/arm64"

mkdir -p "$result_dir/buildroot" "$result_dir/cross-build" "$output_dir"
cp "$repo_dir/buildroot/configs/mqmgateway_aarch64_defconfig" "$output_dir/.config"

make -C "$buildroot_dir" O="$output_dir" olddefconfig 2>&1 | tee "$result_dir/buildroot/configure.log"
make -C "$buildroot_dir" O="$output_dir" \
    BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="$repo_dir/buildroot/linux-can.fragment" \
    BR2_PRIMARY_SITE="https://sources.buildroot.net" \
    -j"$(nproc)" 2>&1 | tee "$result_dir/buildroot/build.log"

export BUILDROOT_OUTPUT_DIR="$output_dir"
cmake -S "$repo_dir" -B "$cross_build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$repo_dir/cmake/toolchains/buildroot-aarch64.cmake" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_CXX_FLAGS=-DSPDLOG_FMT_EXTERNAL \
    -DBUILD_STRESS_TOOLS=ON \
    -DWITHOUT_TESTS=ON 2>&1 | tee "$result_dir/cross-build/configure.log"
cmake --build "$cross_build_dir" -j"$(nproc)" 2>&1 | tee "$result_dir/cross-build/build.log"
DESTDIR="$output_dir/target" cmake --install "$cross_build_dir" 2>&1 | tee "$result_dir/cross-build/install.log"

cp -a "$repo_dir/buildroot/overlay/." "$output_dir/target/"
chmod 0755 "$output_dir/target/root/run-arm-tests.sh" "$output_dir/target/root/run-arm-soak.sh" \
    "$output_dir/target/root/run-arm-stress.sh"
make -C "$buildroot_dir" O="$output_dir" \
    BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES="$repo_dir/buildroot/linux-can.fragment" \
    BR2_PRIMARY_SITE="https://sources.buildroot.net" \
    2>&1 | tee "$result_dir/buildroot/finalize.log"

file "$cross_build_dir/modmqttd/modmqttd" \
     "$cross_build_dir/src/iot_gateway/mqmgateway_iot" \
     "$cross_build_dir/src/serial/mqmgateway_rtu_transport_tests" \
    | tee "$result_dir/cross-build/file-identification.log"
"$output_dir/host/bin/aarch64-buildroot-linux-gnu-g++" --version \
    | tee "$result_dir/cross-build/compiler-version.log"
