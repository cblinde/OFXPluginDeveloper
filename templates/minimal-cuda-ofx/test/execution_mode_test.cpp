// SPDX-License-Identifier: BSD-3-Clause
//
// Host test for src/ExecutionMode.h: the four gates, the override parser and the
// boundaries where a decision flips.
//
// No CUDA, no OFX, no GPU: the policy is a pure function over facts, so a machine
// that cannot run a kernel can still show what the plug-in would decide. What
// this cannot test is whether the facts are gathered correctly - that is the
// plug-in's job and the contract suite's job (see run-tests.bat/sh).
//
// Exit status is 0 only if every case matches.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ExecutionMode.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void fail(const char* what, const char* expected, const char* actual)
{
    ++g_failures;
    if (g_failures <= 20) {
        std::printf("FAIL %s: expected %s, got %s\n", what, expected, actual);
    }
}

void expect(const char* what, mcofx::ExecutionMode expected, mcofx::ExecutionMode actual)
{
    ++g_checks;
    if (expected != actual) {
        fail(what, mcofx::ModeName(expected), mcofx::ModeName(actual));
    }
}

void expectTrue(const char* what, bool condition)
{
    ++g_checks;
    if (!condition) {
        fail(what, "true", "false");
    }
}

mcofx::FrameFacts Viable4k()
{
    mcofx::FrameFacts facts;
    facts.hostHandedDevicePointers = true;
    facts.deviceQueryable = true;
    facts.frameBytes = 1920LL * 1080LL * 4LL * 2LL; // RGBA16F at 1080p
    facts.deviceFreeBytes = 2LL * facts.frameBytes + 256LL * 1024LL * 1024LL + 1; // just enough
    facts.inputOnDevice = true;
    return facts;
}

} // namespace

void setModeEnv(const char* value)
{
#if defined(_WIN32)
    _putenv_s("OFXPLUGIN_GPU_MODE", value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        unsetenv("OFXPLUGIN_GPU_MODE");
    } else {
        setenv("OFXPLUGIN_GPU_MODE", value, 1);
    }
#endif
}

void expectLabel(const char* what, const std::string& expected, const std::string& actual)
{
    ++g_checks;
    if (expected != actual) {
        ++g_failures;
        if (g_failures <= 20) {
            std::printf("FAIL %s: expected \"%s\", got \"%s\"\n", what, expected.c_str(),
                        actual.c_str());
        }
        return;
    }
    // Printed even when it matches: the point of this pass is to record what the
    // label reads on the branch no host here can render, and a check that only
    // speaks when it fails leaves that sentence in nobody's log.
    std::printf("  label: %s\n", actual.c_str());
}

int main()
{
    using mcofx::DecideMode;
    using mcofx::ExecutionMode;
    // C++14: the language has no using-enum declaration, so the three names this
    // test writes most often get aliases.
    // writes most often get aliases.
    constexpr ExecutionMode CpuOnly = ExecutionMode::CpuOnly;
    constexpr ExecutionMode CudaOnly = ExecutionMode::CudaOnly;
    constexpr ExecutionMode CpuCudaHybrid = ExecutionMode::CpuCudaHybrid;

    // ---- the override parser: a typo is not auto -------------------------
    {
        bool ok = false;
        expect("parse: empty is auto", ExecutionMode::Auto, mcofx::ParseModeOverride("", &ok));
        expectTrue("parse: empty recognised", ok);
        expect("parse: auto", ExecutionMode::Auto, mcofx::ParseModeOverride("auto", &ok));
        expectTrue("parse: auto recognised", ok);
        expect("parse: cpu", CpuOnly, mcofx::ParseModeOverride("cpu", &ok));
        expect("parse: cuda", CudaOnly, mcofx::ParseModeOverride("cuda", &ok));
        expect("parse: hybrid", CpuCudaHybrid, mcofx::ParseModeOverride("hybrid", &ok));
        expectTrue("parse: hybrid recognised", ok);
        expect("parse: typo falls back to auto", ExecutionMode::Auto,
               mcofx::ParseModeOverride("hybryd", &ok));
        expectTrue("parse: typo is reported", !ok);
    }

    // ---- gate 0: an explicit CPU request wins ----------------------------
    expect("gate 0: cpu override on a capable device", CpuOnly,
           DecideMode(CpuOnly, Viable4k()).mode);
    {
        mcofx::FrameFacts broken;
        broken.deviceQueryable = false;
        expect("gate 0: cpu override without a device", CpuOnly, DecideMode(CpuOnly, broken).mode);
    }

    // ---- gate 1: host and device ----------------------------------------
    {
        mcofx::FrameFacts facts = Viable4k();
        facts.hostHandedDevicePointers = false;
        expect("gate 1: host did not enable CUDA", CpuOnly, DecideMode(ExecutionMode::Auto, facts).mode);
        expect("gate 1: cuda override cannot invent device pointers", CpuOnly,
               DecideMode(CudaOnly, facts).mode);
        expect("gate 1: hybrid override cannot either", CpuOnly,
               DecideMode(CpuCudaHybrid, facts).mode);
    }
    {
        mcofx::FrameFacts facts = Viable4k();
        facts.deviceQueryable = false;
        expect("gate 1: no device answered", CpuOnly, DecideMode(ExecutionMode::Auto, facts).mode);
    }

    // ---- gate 2: VRAM, at the boundary ----------------------------------
    {
        mcofx::FrameFacts facts = Viable4k();
        facts.deviceFreeBytes = 2LL * facts.frameBytes + 256LL * 1024LL * 1024LL - 1;
        expect("gate 2: one byte short of two frames plus 256 MiB", CpuOnly,
               DecideMode(ExecutionMode::Auto, facts).mode);
        facts.deviceFreeBytes += 1; // exactly the requirement is enough
        expect("gate 2: exactly two frames plus 256 MiB", CudaOnly,
               DecideMode(ExecutionMode::Auto, facts).mode);
    }
    {
        mcofx::FrameFacts facts = Viable4k();
        facts.deviceFreeBytes = 2LL * facts.frameBytes + 256LL * 1024LL * 1024LL + 4096;
        expect("gate 2: one byte above the requirement", CudaOnly,
               DecideMode(ExecutionMode::Auto, facts).mode);
    }
    {
        // The same rule at 4K RGBA32F: 64 MiB per frame, so 128 MiB plus headroom.
        mcofx::FrameFacts facts = Viable4k();
        facts.frameBytes = 3840LL * 2160LL * 4LL * 4LL;
        facts.deviceFreeBytes = 2LL * facts.frameBytes; // no room for the headroom
        expect("gate 2: 4K 32F without headroom", CpuOnly, DecideMode(ExecutionMode::Auto, facts).mode);
    }

    // ---- gate 3: transfer balance ---------------------------------------
    {
        mcofx::FrameFacts facts = Viable4k();
        facts.inputOnDevice = false; // a copy would be needed
        facts.estimatedKernelMs = 4.0;
        facts.estimatedCopyMs = 6.0; // 1.5x the kernel
        expect("gate 3: copy costs 1.5x the kernel", CpuOnly, DecideMode(ExecutionMode::Auto, facts).mode);
        facts.estimatedCopyMs = 4.0; // 1.0x: not dominated
        expect("gate 3: copy costs the same as the kernel", CudaOnly,
               DecideMode(ExecutionMode::Auto, facts).mode);
        facts.estimatedCopyMs = 5.1; // 1.275x
        expect("gate 3: copy costs 1.275x the kernel", CpuOnly, DecideMode(ExecutionMode::Auto, facts).mode);
        facts.estimatedCopyMs = 5.0; // 1.25x: the rule is "more than", so not dominated
        expect("gate 3: copy at exactly 1.25x", CudaOnly, DecideMode(ExecutionMode::Auto, facts).mode);
    }
    {
        // On the OFX GPU path the pixels are already device pointers: no copy, and
        // a copy estimate must not demote the render.
        mcofx::FrameFacts facts = Viable4k();
        facts.inputOnDevice = true;
        facts.estimatedKernelMs = 4.0;
        facts.estimatedCopyMs = 60.0;
        expect("gate 3: device-resident pixels ignore the copy estimate", CudaOnly,
               DecideMode(ExecutionMode::Auto, facts).mode);
    }

    // ---- gate 4: hybrid needs work and a kernel worth overlapping -------
    {
        mcofx::FrameFacts facts = Viable4k();
        facts.hasIndependentCpuTask = true;
        facts.estimatedKernelMs = 1.0;
        expect("gate 4: 1 ms kernel with independent work", CpuCudaHybrid,
               DecideMode(ExecutionMode::Auto, facts).mode);
        facts.estimatedKernelMs = 0.9;
        expect("gate 4: 0.9 ms kernel is not worth overlapping", CudaOnly,
               DecideMode(ExecutionMode::Auto, facts).mode);
    }
    {
        mcofx::FrameFacts facts = Viable4k();
        facts.hasIndependentCpuTask = false;
        facts.estimatedKernelMs = 40.0;
        expect("gate 4: 40 ms kernel without independent work", CudaOnly,
               DecideMode(ExecutionMode::Auto, facts).mode);
        expect("gate 4: hybrid override still needs the device gates", CpuCudaHybrid,
               DecideMode(CpuCudaHybrid, facts).mode);
    }

    // ---- overrides after the gates ---------------------------------------
    {
        mcofx::FrameFacts facts = Viable4k();
        expect("override: cuda on a capable device", CudaOnly, DecideMode(CudaOnly, facts).mode);
        expect("override: hybrid on a capable device", CpuCudaHybrid,
               DecideMode(CpuCudaHybrid, facts).mode);
        facts.deviceFreeBytes = 0;
        expect("override: cuda cannot override the VRAM gate", CpuOnly,
               DecideMode(CudaOnly, facts).mode);
        expect("override: hybrid cannot either", CpuOnly, DecideMode(CpuCudaHybrid, facts).mode);
    }

    // ---- the reason strings are never empty ------------------------------
    {
        const mcofx::ModeDecision d = DecideMode(ExecutionMode::Auto, Viable4k());
        ++g_checks;
        if (d.reason == nullptr || *d.reason == '\0') {
            fail("decision always carries a reason", "non-empty", "empty");
        }
    }

    // The Device label, branch by branch. The contract suite can only render the
    // CPU branch (the fake host never sets kOfxImageEffectPropCudaEnabled), so
    // the CUDA_ONLY wording is asserted here, through the same PathLabel() the
    // plug-in calls in processImagesCuda().
    {
        using namespace mcofx;
        FrameFacts device;
        device.hostHandedDevicePointers = true;
        device.inputOnDevice = true;
        device.deviceQueryable = true;
        device.frameBytes = 1920LL * 1080LL * 16LL;
        device.deviceFreeBytes = 8LL * 1024 * 1024 * 1024;

        expectLabel("label: CUDA_ONLY requested", "CUDA_ONLY - CUDA_ONLY requested",
                    PathLabel(DecideMode(ExecutionMode::CudaOnly, device)));
        expectLabel("label: CUDA_ONLY by gate 4", "CUDA_ONLY - no independent CPU work to overlap",
                    PathLabel(DecideMode(ExecutionMode::Auto, device)));

        FrameFacts hostPointers;
        hostPointers.deviceQueryable = true;
        hostPointers.deviceFreeBytes = device.deviceFreeBytes;
        hostPointers.frameBytes = device.frameBytes;
        expectLabel("label: CPU_ONLY because the host did not enable CUDA",
                    "CPU_ONLY - the host did not set kOfxImageEffectPropCudaEnabled, so the "
                    "pixels are host pointers",
                    PathLabel(DecideMode(ExecutionMode::Auto, hostPointers)));

        // With the override unreadable, both branches carry the same sentence -
        // the property the contract run proves for the CPU side only.
        setModeEnv("gpux");
        expectLabel("label: the typo reaches the CUDA_ONLY label",
                    "CUDA_ONLY - CUDA_ONLY requested; "
                    "OFXPLUGIN_GPU_MODE='gpux' is not auto|cpu|cuda|hybrid",
                    PathLabel(DecideMode(ExecutionMode::CudaOnly, device)));
        setModeEnv(nullptr);
    }

    std::printf("execution_mode_test: %d cases, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
