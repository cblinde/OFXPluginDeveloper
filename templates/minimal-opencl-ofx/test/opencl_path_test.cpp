// SPDX-License-Identifier: BSD-3-Clause
//
// The half of this template that runs on any machine: the gates that decide
// between the host's OpenCL queue and the CPU loop, and the environment override
// that forces one of them.
//
// Why it exists. Every other template can prove its path decision without the
// hardware - minimal-cuda-ofx has ExecutionMode.h, minimal-vulkan-ofx has
// VulkanMode.h, the ONNX template asserts its provider label through the
// contract harness. This template left the choice to the host entirely, so on a
// machine with a device the CPU path was unreachable by a test, on one without a
// device only that path ran, and no run could assert that the decision is
// *reported* to the user. PathDecision.h is the one place both callers read, and
// this is the test that executes it with no device, no loader and no SDK.
//
// What it does not prove: that the dispatch agrees with the CPU loop (that is
// opencl_suite's job, and it needs a device), or that the plug-in passes the
// decision into its label and its describe() correctly (that is the contract
// suite's forced-CPU run).
//
// Exit codes: 0 every case matched, 1 a mismatch.
#include "PathDecision.h"

#include <cstdio>

namespace {

int checks = 0;
int failures = 0;

void expect(bool ok, const char* what, const char* got, const char* want)
{
    ++checks;
    if (ok) {
        std::printf("  %-52s ok   %s\n", what, got);
    } else {
        std::printf("  %-52s FAIL %s (wanted %s)\n", what, got, want);
        ++failures;
    }
}

void expectPath(bool ok, const char* what, oclgain::Path got, oclgain::Path want)
{
    expect(ok, what, oclgain::PathName(got), oclgain::PathName(want));
}

void expectFault(bool ok, const char* what, oclgain::HandoverFault got,
                 oclgain::HandoverFault want)
{
    expect(ok, what, oclgain::HandoverFaultText(got), oclgain::HandoverFaultText(want));
}

oclgain::Facts hostWithQueue()
{
    oclgain::Facts facts;
    facts.hostEnabledOpenCL = true;
    facts.queuePresent = true;
    return facts;
}

/// A Buffers handover the plug-in can use: a queue, a pitch that is a positive
/// multiple of 4, the same origin, and a cl_mem on both sides.
oclgain::HandoverFacts buffers()
{
    oclgain::HandoverFacts facts;
    facts.queuePresent = true;
    facts.sourceBuffer = true;
    facts.destBuffer = true;
    facts.sourceRowPitch = 7680 * 4;
    facts.destRowPitch = 7680 * 4;
    return facts;
}

} // namespace

int main()
{
    std::printf("PathDecision: the gates and the handover, no device needed\n");

    // ---- parsing: three words, a typo, and nothing at all --------------------
    {
        bool recognised = false;
        const oclgain::Request unset = oclgain::ParseModeOverride(nullptr, &recognised);
        expect(unset == oclgain::Request::Auto && recognised,
               "OFXPLUGIN_OPENCL_MODE unset reads as auto",
               oclgain::RequestName(unset), oclgain::RequestName(oclgain::Request::Auto));

        recognised = false;
        const oclgain::Request empty = oclgain::ParseModeOverride("", &recognised);
        expect(empty == oclgain::Request::Auto && recognised, "an empty string reads as auto",
               oclgain::RequestName(empty), oclgain::RequestName(oclgain::Request::Auto));

        recognised = false;
        const oclgain::Request autoWord = oclgain::ParseModeOverride("auto", &recognised);
        expect(autoWord == oclgain::Request::Auto && recognised, "\"auto\" reads as auto",
               oclgain::RequestName(autoWord), oclgain::RequestName(oclgain::Request::Auto));

        recognised = false;
        const oclgain::Request cpu = oclgain::ParseModeOverride("cpu", &recognised);
        expect(cpu == oclgain::Request::CpuOnly && recognised, "\"cpu\" forces the CPU path",
               oclgain::RequestName(cpu), oclgain::RequestName(oclgain::Request::CpuOnly));

        recognised = false;
        const oclgain::Request opencl = oclgain::ParseModeOverride("opencl", &recognised);
        expect(opencl == oclgain::Request::OpenCLOnly && recognised,
               "\"opencl\" asks for the device", oclgain::RequestName(opencl),
               oclgain::RequestName(oclgain::Request::OpenCLOnly));

        recognised = false;
        const oclgain::Request gpu = oclgain::ParseModeOverride("gpu", &recognised);
        expect(gpu == oclgain::Request::OpenCLOnly && recognised,
               "\"gpu\" is the same request as \"opencl\"", oclgain::RequestName(gpu),
               oclgain::RequestName(oclgain::Request::OpenCLOnly));

        recognised = true;
        const oclgain::Request typo = oclgain::ParseModeOverride("CPU", &recognised);
        expect(typo == oclgain::Request::Auto && !recognised,
               "an uppercase \"CPU\" is unrecognised, not cpu", oclgain::RequestName(typo),
               oclgain::RequestName(oclgain::Request::Auto));

        recognised = true;
        const oclgain::Request junk = oclgain::ParseModeOverride("cl", &recognised);
        expect(junk == oclgain::Request::Auto && !recognised, "\"cl\" is unrecognised",
               oclgain::RequestName(junk), oclgain::RequestName(oclgain::Request::Auto));
    }

    // ---- the gates ----------------------------------------------------------
    {
        // Gate 0: the override wins over anything the host says, including a host
        // that offers a working queue.
        const oclgain::Decision forced =
            oclgain::DecidePath(oclgain::Request::CpuOnly, hostWithQueue());
        expectPath(forced.path == oclgain::Path::Cpu,
                   "OFXPLUGIN_OPENCL_MODE=cpu demotes a host that offers OpenCL", forced.path,
                   oclgain::Path::Cpu);
        expect(std::strstr(forced.reason, "OFXPLUGIN_OPENCL_MODE=cpu") != nullptr,
               "the forced-CPU reason names the override", forced.reason,
               "OFXPLUGIN_OPENCL_MODE=cpu was set");

        // Gate 1: no OpenCL from the host, which is every host that has no device.
        oclgain::Facts noHost;
        const oclgain::Decision plain = oclgain::DecidePath(oclgain::Request::Auto, noHost);
        expectPath(plain.path == oclgain::Path::Cpu, "a host without OpenCL renders on the CPU",
                   plain.path, oclgain::Path::Cpu);
        expect(std::strstr(plain.reason, "the host did not enable OpenCL") != nullptr,
               "the reason says the host did not enable it", plain.reason,
               "the host did not enable OpenCL on this render");

        // The device request cannot promote past that gate; it is demoted with a
        // reason that names the override, so a reader can tell the two apart.
        const oclgain::Decision demoted =
            oclgain::DecidePath(oclgain::Request::OpenCLOnly, noHost);
        expectPath(demoted.path == oclgain::Path::Cpu,
                   "OFXPLUGIN_OPENCL_MODE=opencl is demoted without a host queue",
                   demoted.path, oclgain::Path::Cpu);
        expect(std::strstr(demoted.reason, "did not enable OpenCL") != nullptr &&
                   std::strstr(demoted.reason, "opencl was set") != nullptr,
               "the demoted reason names both the override and the host", demoted.reason,
               "OFXPLUGIN_OPENCL_MODE=opencl was set, but the host did not enable OpenCL");

        // Gate 2: the host's queue is what the device path runs on.
        const oclgain::Decision device =
            oclgain::DecidePath(oclgain::Request::Auto, hostWithQueue());
        expectPath(device.path == oclgain::Path::OpenCL, "a host that offers OpenCL takes it",
                   device.path, oclgain::Path::OpenCL);
        expect(std::strstr(device.reason, "host's OpenCL queue") != nullptr,
               "the reason names the host's queue", device.reason, "the host's OpenCL queue");

        const oclgain::Decision asked =
            oclgain::DecidePath(oclgain::Request::OpenCLOnly, hostWithQueue());
        expectPath(asked.path == oclgain::Path::OpenCL,
                   "the device request and the host agree", asked.path, oclgain::Path::OpenCL);
        expect(std::strstr(asked.reason, "opencl was set") != nullptr,
               "the reason names the override", asked.reason,
               "OFXPLUGIN_OPENCL_MODE=opencl was set");

        // The host enables OpenCL and hands no queue: still the device path, and
        // the handover check below is what refuses it. A CPU path for a cl_mem
        // would read the pointer's contents as pixels.
        oclgain::Facts noQueue;
        noQueue.hostEnabledOpenCL = true;
        const oclgain::Decision brokenQueue = oclgain::DecidePath(oclgain::Request::Auto, noQueue);
        expectPath(brokenQueue.path == oclgain::Path::OpenCL,
                   "a missing queue does not demote to a path that cannot read cl_mem",
                   brokenQueue.path, oclgain::Path::OpenCL);
        expect(std::strstr(brokenQueue.reason, "no command queue") != nullptr,
               "the reason names the missing queue", brokenQueue.reason,
               "the host enabled OpenCL but handed no command queue");
    }

    // ---- the handover -------------------------------------------------------
    {
        expectFault(oclgain::CheckHandover(buffers()) == oclgain::HandoverFault::None,
                    "a queue, a pitch and two cl_mem are usable", oclgain::CheckHandover(buffers()),
                    oclgain::HandoverFault::None);

        oclgain::HandoverFacts noQueue = buffers();
        noQueue.queuePresent = false;
        expectFault(oclgain::CheckHandover(noQueue) == oclgain::HandoverFault::NoQueue,
                    "no queue is a fault, not a demotion", oclgain::CheckHandover(noQueue),
                    oclgain::HandoverFault::NoQueue);

        oclgain::HandoverFacts negativePitch = buffers();
        negativePitch.sourceRowPitch = -7680 * 4;
        expectFault(oclgain::CheckHandover(negativePitch) == oclgain::HandoverFault::BadPitch,
                    "a negative pitch is refused", oclgain::CheckHandover(negativePitch),
                    oclgain::HandoverFault::BadPitch);

        oclgain::HandoverFacts mismatchedPitch = buffers();
        mismatchedPitch.destRowPitch = 7680 * 4 * 2;
        expectFault(oclgain::CheckHandover(mismatchedPitch) == oclgain::HandoverFault::BadPitch,
                    "a pitch that differs between the images is refused",
                    oclgain::CheckHandover(mismatchedPitch), oclgain::HandoverFault::BadPitch);

        oclgain::HandoverFacts oddPitch = buffers();
        oddPitch.sourceRowPitch = 7680 * 4 + 1;
        oddPitch.destRowPitch = 7680 * 4 + 1;
        expectFault(oclgain::CheckHandover(oddPitch) == oclgain::HandoverFault::BadPitch,
                    "a pitch that is not a multiple of 4 bytes is refused",
                    oclgain::CheckHandover(oddPitch), oclgain::HandoverFault::BadPitch);

        oclgain::HandoverFacts origin = buffers();
        origin.sameBoundsOrigin = false;
        expectFault(oclgain::CheckHandover(origin) == oclgain::HandoverFault::BadOrigin,
                    "different bounds origins are refused", oclgain::CheckHandover(origin),
                    oclgain::HandoverFault::BadOrigin);

        oclgain::HandoverFacts noBuffer = buffers();
        noBuffer.sourceBuffer = false;
        expectFault(oclgain::CheckHandover(noBuffer) == oclgain::HandoverFault::NoBuffers,
                    "an empty kOfxImagePropData is refused", oclgain::CheckHandover(noBuffer),
                    oclgain::HandoverFault::NoBuffers);

        // Images: both sides carrying kOfxImageEffectPropOpenCLImage is the
        // handover, and an image has no pitch - a zero rowBytes, which a Buffers
        // render would refuse, is legal here.
        oclgain::HandoverFacts images = buffers();
        images.imagesHandover = true;
        images.sourceImage = true;
        images.destImage = true;
        images.sourceBuffer = false;
        images.destBuffer = false;
        images.sourceRowPitch = 0;
        images.destRowPitch = 0;
        expectFault(oclgain::CheckHandover(images) == oclgain::HandoverFault::None,
                    "an image handover needs no row pitch",
                    oclgain::CheckHandover(images), oclgain::HandoverFault::None);

        images.destImage = false;
        expectFault(oclgain::CheckHandover(images) == oclgain::HandoverFault::PartialImages,
                    "one image with the property and one without is refused",
                    oclgain::CheckHandover(images), oclgain::HandoverFault::PartialImages);
    }

    // The name is part of the line: the documentation gate adds the counts up by
    // suite name, the same way it reads "contract_test:" and "vulkan_mode_test:".
    std::printf("opencl_path_test: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
