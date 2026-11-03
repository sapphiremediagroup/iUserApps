// gpu-test: runs the kernel's experiments on the Intel Gen9 GPU (the GpuTest
// syscall) and prints their reports. Run it from the terminal as root:
//
//   gpu-test info          map the GGTT and report what the firmware left in it
//   gpu-test gtt           buffer objects: bind, CPU access both ways, unbind
//   gpu-test flip [secs]   show a test pattern from a buffer object for a few
//                          seconds (default 5), then the desktop again
//   gpu-test engine        the blitter engine runs commands that store to memory
//   gpu-test hang          a deliberate blitter hang: detected, reset, recovered
//   gpu-test render        the render engine runs commands that store to memory
//   gpu-test irq [times]   both engines signal completion with an interrupt;
//                          with a count, repeats quietly and prints the full
//                          report of the first run that fails
//   gpu-test ppgtt         a per-process 48-bit GPU address space, CPU-coherent
//   gpu-test gem [times]   GPU buffer objects through /dev/dri/renderD128 (the
//                          i915 ioctls): create, mmap, softpinned execbuffers
//                          on both engines (repeated and timed with a count);
//                          `gem cpu-only` checks the rest where there is no GPU
//   gpu-test ctx           render contexts as a 3D driver needs them: the
//                          default state, isolation, registers kept between
//                          batches, the whitelist, PIPE_CONTROL
//   gpu-test fence         execbuffer without waiting: busy/wait, implicit
//                          and explicit (sync object) fences, a hang caught
//   gpu-test uapi          the rest of what Mesa asks: GETPARAM, QUERY,
//                          context parameters, address spaces, engine maps,
//                          timestamps, tiling, caching (`uapi cpu-only` in QEMU)
//   gpu-test sync          timeline sync objects, sync object and sync_file
//                          fds, dma-buf sharing (PRIME), FENCE_IN/OUT
//                          (`sync cpu-only` in QEMU)
//   gpu-test dev           the render node as libdrm finds it: /dev/dri and
//                          /sys/dev/char/226:128 (`dev cpu-only` in QEMU)
//   gpu-test window        a window whose pixels are GPU buffers (dma-buf
//                          surfaces), drawn by the blitter; `window [frames]
//                          cpu-only` draws with the CPU (QEMU)
//   gpu-test state         the driver's request queue: what ran, who retired
//                          it, how long it took, the engines' registers
//   gpu-test all           info, gtt, engine, render, irq, ppgtt, gem, ctx,
//                          fence, uapi, sync, dev, then flip (not hang)
//
// The reports also go to the kernel log (/kernel.log on the PC). Exit status
// 0 when every experiment run reported ok.

#include <cstdint.hpp>
#include <cstring.hpp>
#include <syscall.hpp>

bool gem_test(std::uint64_t times, bool cpuOnly);   // gem.cpp
bool ctx_test();                                    // gem.cpp
bool fence_test();                                  // gem.cpp
bool uapi_test(bool cpuOnly);                       // gem.cpp
bool sync_test(bool cpuOnly);                       // gem.cpp
bool dev_test(bool cpuOnly);                        // gem.cpp
bool window_test(std::uint64_t frames, bool cpuOnly);    // gem.cpp
void gem_flush();                                   // gem.cpp: their output, collected

namespace {

constexpr std::uint64_t kReportSize = 64 * 1024;
char gReport[kReportSize];

void out(const char* text) {
    std::write(1, text, std::strlen(text));
}

bool failed(std::uint64_t result) {
    return static_cast<std::int64_t>(result) < 0;
}

// Runs one command and prints its report (when `print`, or when it failed);
// true if it ended in ": ok".
bool run(std::GpuTestCommand command, bool print = true) {
    const std::uint64_t got = std::gpu_test(command, gReport, kReportSize);
    if (failed(got)) {
        const std::int64_t error = -static_cast<std::int64_t>(got);
        if (error == 1) {
            out("gpu-test: permission denied (run it as root)\n");
        } else if (error == 19) {
            out("gpu-test: no Intel Gen9 GPU driven by this kernel\n");
        } else {
            out("gpu-test: the GpuTest call failed\n");
        }
        return false;
    }
    const char ok[] = ": ok\n";
    const std::uint64_t okLength = sizeof(ok) - 1;
    const bool passed = got >= okLength && std::memcmp(gReport + got - okLength, ok, okLength) == 0;
    if (print || !passed) {
        std::write(1, gReport, got);
    }
    return passed;
}

void out_number(std::uint64_t value) {
    char digits[24];
    int length = 0;
    do {
        digits[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char text[24];
    for (int i = 0; i < length; ++i) {
        text[i] = digits[length - 1 - i];
    }
    std::write(1, text, static_cast<std::uint64_t>(length));
}

// `gpu-test irq N`: N quiet runs, stopping at the first failure (whose report
// run() prints in full), with a progress line every 50 runs.
bool repeat_irq(std::uint64_t times) {
    for (std::uint64_t i = 1; i <= times; ++i) {
        if (!run(std::GpuTestCommand::Irq, false)) {
            out("gpu-test: irq run ");
            out_number(i);
            out(" of ");
            out_number(times);
            out(" failed\n");
            return false;
        }
        if (i % 50 == 0 || i == times) {
            out("gpu-test: irq ");
            out_number(i);
            out(" of ");
            out_number(times);
            out(" ok\n");
        }
    }
    return true;
}

std::uint64_t parse_count(const char* text, std::uint64_t fallback, std::uint64_t limit) {
    if (!text || !*text) {
        return fallback;
    }
    std::uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') {
            return fallback;
        }
        value = value * 10 + static_cast<std::uint64_t>(*text - '0');
    }
    return value == 0 || value > limit ? fallback : value;
}

bool flip(std::uint64_t seconds) {
    if (!run(std::GpuTestCommand::FlipOn)) {
        return false;
    }
    std::sleep(seconds * 1000);
    return run(std::GpuTestCommand::FlipOff);
}

void usage() {
    out("usage: gpu-test info | gtt | flip [seconds] | engine | hang | render | irq [times] | ppgtt | gem [times|cpu-only] | ctx | fence | uapi [cpu-only] | sync [cpu-only] | dev [cpu-only] | window [frames] [cpu-only] | state | all\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const char* what = argv[1];
    bool ok = false;
    if (std::strcmp(what, "info") == 0) {
        ok = run(std::GpuTestCommand::Info);
    } else if (std::strcmp(what, "gtt") == 0) {
        ok = run(std::GpuTestCommand::Gtt);
    } else if (std::strcmp(what, "flip") == 0) {
        ok = flip(parse_count(argc > 2 ? argv[2] : nullptr, 5, 60));
    } else if (std::strcmp(what, "engine") == 0) {
        ok = run(std::GpuTestCommand::Engine);
    } else if (std::strcmp(what, "hang") == 0) {
        ok = run(std::GpuTestCommand::Hang);
    } else if (std::strcmp(what, "render") == 0) {
        ok = run(std::GpuTestCommand::Render);
    } else if (std::strcmp(what, "irq") == 0) {
        ok = argc > 2 ? repeat_irq(parse_count(argv[2], 1, 10000)) : run(std::GpuTestCommand::Irq);
    } else if (std::strcmp(what, "ppgtt") == 0) {
        ok = run(std::GpuTestCommand::Ppgtt);
    } else if (std::strcmp(what, "gem") == 0) {
        const bool cpuOnly = argc > 2 && std::strcmp(argv[2], "cpu-only") == 0;
        ok = gem_test(cpuOnly ? 1 : parse_count(argc > 2 ? argv[2] : nullptr, 1, 100000), cpuOnly);
        gem_flush();
    } else if (std::strcmp(what, "ctx") == 0) {
        ok = ctx_test();
        gem_flush();
    } else if (std::strcmp(what, "fence") == 0) {
        ok = fence_test();
        gem_flush();
    } else if (std::strcmp(what, "uapi") == 0) {
        ok = uapi_test(argc > 2 && std::strcmp(argv[2], "cpu-only") == 0);
        gem_flush();
    } else if (std::strcmp(what, "sync") == 0) {
        ok = sync_test(argc > 2 && std::strcmp(argv[2], "cpu-only") == 0);
        gem_flush();
    } else if (std::strcmp(what, "dev") == 0) {
        ok = dev_test(argc > 2 && std::strcmp(argv[2], "cpu-only") == 0);
        gem_flush();
    } else if (std::strcmp(what, "window") == 0) {
        std::uint64_t frames = 0;
        bool cpuOnly = false;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "cpu-only") == 0) {
                cpuOnly = true;
            } else {
                for (const char* c = argv[i]; *c >= '0' && *c <= '9'; ++c) {
                    frames = frames * 10 + static_cast<std::uint64_t>(*c - '0');
                }
            }
        }
        ok = window_test(frames, cpuOnly);
        gem_flush();
    } else if (std::strcmp(what, "state") == 0) {
        ok = run(std::GpuTestCommand::DrmState);
    } else if (std::strcmp(what, "all") == 0) {
        const bool info = run(std::GpuTestCommand::Info);
        const bool gtt = info && run(std::GpuTestCommand::Gtt);
        const bool engine = gtt && run(std::GpuTestCommand::Engine);
        const bool render = engine && run(std::GpuTestCommand::Render);
        const bool irq = render && run(std::GpuTestCommand::Irq);
        const bool ppgtt = irq && run(std::GpuTestCommand::Ppgtt);
        const bool gem = ppgtt && gem_test(1, false);
        gem_flush();
        const bool ctx = gem && ctx_test();
        gem_flush();
        const bool fence = ctx && fence_test();
        gem_flush();
        const bool uapi = fence && uapi_test(false);
        gem_flush();
        const bool sync = uapi && sync_test(false);
        gem_flush();
        const bool dev = sync && dev_test(false);
        gem_flush();
        ok = dev && flip(5);
    } else {
        usage();
        return 2;
    }
    out(ok ? "gpu-test: all ok\n" : "gpu-test: FAILED\n");
    return ok ? 0 : 1;
}
