// gpu-test gem: GPU buffer objects through /dev/dri/renderD128, the way
// libdrm and Mesa use it (Linux's i915 ioctls, i915_drm.hpp). It checks:
// VERSION and GETPARAM; GEM_CREATE (rounding, zeroed pages, handle reuse);
// GEM_MMAP_OFFSET and mmap, write-back and write-combining; EXECBUFFER2 with
// softpinned objects, one at a canonical high address, on the render engine
// and the blitter: a batch the CPU wrote through its write-back mapping
// stores into a two-page buffer, which the CPU reads without any flush; the
// errors for what is not supported; and that a closed handle's object lives
// on while it is still mapped. With `times`, the execbuffers repeat (timed).
// `cpu-only` (QEMU, no Intel GPU) does everything but run batches.

#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <syscall.hpp>

#include "i915_drm.hpp"

namespace {

using std::int32_t;
using std::int64_t;
using std::uint8_t;
using std::uint32_t;
using std::uint64_t;

// Output is collected and written at the end (gem_flush): a terminal or a
// file on a hard disk can take tens of ms a write, which would stretch the
// timed parts (a spinner the CPU ends would run into the hang check).
char gOut[64 * 1024];
uint64_t gOutUsed = 0;

void flush_out() {
    if (gOutUsed != 0) {
        std::write(1, gOut, gOutUsed);
        gOutUsed = 0;
    }
}

void emit(const char* text, uint64_t length) {
    if (gOutUsed + length > sizeof(gOut)) {
        flush_out();
    }
    if (length > sizeof(gOut)) {
        std::write(1, text, length);
        return;
    }
    std::memcpy(gOut + gOutUsed, text, length);
    gOutUsed += length;
}

int gFailures = 0;
const char* gName = "gem";   // what check() lines start with

void out(const char* text) {
    emit(text, std::strlen(text));
}

void out_dec(uint64_t value) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char text[24];
    for (int i = 0; i < n; ++i) {
        text[i] = digits[n - 1 - i];
    }
    emit(text, static_cast<uint64_t>(n));
}

void out_hex(uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    char text[18] = {'0', 'x'};
    int n = 2;
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        const int digit = static_cast<int>((value >> shift) & 0xF);
        if (digit != 0 || started || shift == 0) {
            text[n++] = kDigits[digit];
            started = true;
        }
    }
    emit(text, static_cast<uint64_t>(n));
}

uint64_t clock_ns() {
    return _syscall_impl(static_cast<uint64_t>(std::Syscall::GetTimeNs));
}

uint64_t gStartNs = 0;   // when set, check() lines carry the ms since then

void check(bool ok, const char* what) {
    out(gName);
    out(ok ? ": ok   " : ": FAIL ");
    if (gStartNs != 0) {
        out("[");
        out_dec((clock_ns() - gStartNs) / 1000000);
        out(" ms] ");
    }
    out(what);
    out("\n");
    if (!ok) {
        ++gFailures;
    }
}

// The driver's request statistics and engine state (gpu-test state).
char gState[32 * 1024];

void print_state() {
    const uint64_t got = std::gpu_test(std::GpuTestCommand::DrmState, gState, sizeof(gState));
    if (static_cast<int64_t>(got) > 0) {
        emit(gState, got);
    }
}

void out_errno(const char* what, int64_t result) {
    out(what);
    if (result < 0) {
        out(" errno ");
        out_dec(static_cast<uint64_t>(-result));
    } else {
        out(" ");
        out_dec(static_cast<uint64_t>(result));
    }
}

// The raw result: 0/positive, or -errno.
int64_t sys_ioctl(uint64_t fd, uint64_t request, void* arg) {
    return static_cast<int64_t>(_syscall_impl(static_cast<uint64_t>(std::Syscall::Ioctl), fd, request,
                                              reinterpret_cast<uint64_t>(arg)));
}

struct MmapFileArgs {
    uint64_t addr, length, prot, flags, fd, offset;
};

constexpr uint64_t kProtRead = 1;
constexpr uint64_t kProtWrite = 2;
constexpr uint64_t kMapShared = 1;

void* map(uint64_t fd, uint64_t offset, uint64_t length) {
    MmapFileArgs args = {0, length, kProtRead | kProtWrite, kMapShared, fd, offset};
    const uint64_t result = _syscall_impl(static_cast<uint64_t>(std::Syscall::MmapFile),
                                          reinterpret_cast<uint64_t>(&args));
    return static_cast<int64_t>(result) < 0 ? nullptr : reinterpret_cast<void*>(result);
}

int64_t map_error(uint64_t fd, uint64_t offset, uint64_t length) {
    MmapFileArgs args = {0, length, kProtRead | kProtWrite, kMapShared, fd, offset};
    const uint64_t result = _syscall_impl(static_cast<uint64_t>(std::Syscall::MmapFile),
                                          reinterpret_cast<uint64_t>(&args));
    return static_cast<int64_t>(result) < 0 ? static_cast<int64_t>(result) : 0;
}

void unmap(void* address, uint64_t length) {
    _syscall_impl(static_cast<uint64_t>(std::Syscall::Munmap), reinterpret_cast<uint64_t>(address), length);
}

uint32_t create(uint64_t fd, uint64_t size, uint64_t* rounded = nullptr) {
    drm::drm_i915_gem_create request = {};
    request.size = size;
    if (sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CREATE, &request) != 0) {
        return 0;
    }
    if (rounded) {
        *rounded = request.size;
    }
    return request.handle;
}

bool close_handle(uint64_t fd, uint32_t handle) {
    drm::drm_gem_close request = {handle, 0};
    return sys_ioctl(fd, drm::DRM_IOCTL_GEM_CLOSE, &request) == 0;
}

volatile uint32_t* map_object(uint64_t fd, uint32_t handle, uint64_t mode, uint64_t size) {
    drm::drm_i915_gem_mmap_offset request = {};
    request.handle = handle;
    request.flags = mode;
    if (sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_MMAP_OFFSET, &request) != 0) {
        return nullptr;
    }
    return static_cast<volatile uint32_t*>(map(fd, request.offset, size));
}

// Waits until the GPU is done with the object (GEM_WAIT; below 0: forever).
int64_t wait_object(uint64_t fd, uint32_t handle, int64_t timeoutNs = -1) {
    drm::drm_i915_gem_wait request = {};
    request.bo_handle = handle;
    request.timeout_ns = timeoutNs;
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_WAIT, &request);
}

// Commands (Gen9 MI): a 64-bit-address dword store, and the batch's end.
constexpr uint32_t kStoreDword = (0x20u << 23) | 2;   // MI_STORE_DWORD_IMM_GEN4
constexpr uint32_t kBatchEnd = 0x0Au << 23;           // MI_BATCH_BUFFER_END

constexpr uint64_t kBatchVa = 0x10000;
// The target in the upper half of the 48-bit space, given in canonical form
// as Mesa does; the commands use the 48-bit address.
constexpr uint64_t kTargetVa48 = 0x800000100000ull;
constexpr uint64_t kTargetVaCanonical = 0xFFFF800000100000ull;
constexpr uint64_t kTargetBytes = 8192;

// Writes a batch that stores `first` at the start of the target and
// `second` in its second page.
void write_batch(volatile uint32_t* batch, uint32_t first, uint32_t second) {
    uint32_t n = 0;
    batch[n++] = kStoreDword;
    batch[n++] = static_cast<uint32_t>(kTargetVa48);
    batch[n++] = static_cast<uint32_t>(kTargetVa48 >> 32);
    batch[n++] = first;
    batch[n++] = kStoreDword;
    batch[n++] = static_cast<uint32_t>(kTargetVa48 + 4096 + 8);
    batch[n++] = static_cast<uint32_t>((kTargetVa48 + 4096 + 8) >> 32);
    batch[n++] = second;
    batch[n++] = kBatchEnd;
    batch[n++] = 0;
}

int64_t execbuffer(uint64_t fd, uint32_t batch, uint32_t target, uint64_t ring,
                   uint64_t targetFlags = drm::EXEC_OBJECT_PINNED, uint32_t relocations = 0, uint32_t context = 0) {
    drm::drm_i915_gem_exec_object2 objects[2] = {};
    objects[0].handle = target;
    objects[0].offset = kTargetVaCanonical;
    objects[0].flags = targetFlags | drm::EXEC_OBJECT_WRITE | drm::EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
    objects[0].relocation_count = relocations;
    objects[1].handle = batch;
    objects[1].offset = kBatchVa;
    objects[1].flags = drm::EXEC_OBJECT_PINNED | drm::EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
    drm::drm_i915_gem_execbuffer2 exec = {};
    exec.buffers_ptr = reinterpret_cast<uint64_t>(objects);
    exec.buffer_count = 2;
    exec.batch_len = 40;
    exec.flags = ring | drm::I915_EXEC_NO_RELOC | drm::I915_EXEC_HANDLE_LUT;
    exec.rsvd1 = context;
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec);
}

uint64_t now_ns() {
    return _syscall_impl(static_cast<uint64_t>(std::Syscall::GetTimeNs));
}

// One batch on `ring`: the stores land and are read back unflushed.
bool run_on(uint64_t fd, uint32_t batch, uint32_t target, volatile uint32_t* batchMap,
            volatile uint32_t* targetMap, uint64_t ring, uint32_t first, uint32_t second, bool quiet) {
    targetMap[0] = 0;
    targetMap[1024 + 2] = 0;
    write_batch(batchMap, first, second);
    int64_t result = execbuffer(fd, batch, target, ring);
    if (result == 0) {
        result = wait_object(fd, target);   // it returns before the batch has run
    }
    const bool ok = result == 0 && targetMap[0] == first && targetMap[1024 + 2] == second;
    if (!quiet || !ok) {
        out(ring == drm::I915_EXEC_BLT ? "gem: blitter" : "gem: render");
        out(" execbuffer returned ");
        out_dec(result < 0 ? static_cast<uint64_t>(-result) : 0);
        out(result < 0 ? " (errno)" : "");
        out("; read back without a flush: ");
        out_hex(targetMap[0]);
        out(", ");
        out_hex(targetMap[1024 + 2]);
        out("\n");
    }
    return ok;
}

} // namespace

void gem_flush() {
    flush_out();
}

bool gem_test(uint64_t times, bool cpuOnly) {
    gFailures = 0;
    gName = "gem";
    if (cpuOnly) {
        char unused[16];
        std::gpu_test(std::GpuTestCommand::DrmWithoutGpu, unused, sizeof(unused));
    }
    const uint64_t opened = _syscall_impl(static_cast<uint64_t>(std::Syscall::Open),
                                          reinterpret_cast<uint64_t>("/dev/dri/renderD128"), O_RDWR | O_CLOEXEC, 0);
    if (static_cast<int64_t>(opened) < 0) {
        out("gem: cannot open /dev/dri/renderD128, errno ");
        out_dec(static_cast<uint64_t>(-static_cast<int64_t>(opened)));
        out("\n");
        return false;
    }
    const uint64_t fd = opened;

    char name[16] = {};
    drm::drm_version version = {};
    version.name = reinterpret_cast<uint64_t>(name);
    version.name_len = sizeof(name) - 1;
    check(sys_ioctl(fd, drm::DRM_IOCTL_VERSION, &version) == 0 && std::strcmp(name, "i915") == 0 &&
              version.version_major == 1 && version.name_len == 4,
          "VERSION says i915 1.x");

    int chipset = 0;
    drm::drm_i915_getparam param = {};
    param.param = drm::I915_PARAM_CHIPSET_ID;
    param.value = reinterpret_cast<uint64_t>(&chipset);
    const int64_t gotChipset = sys_ioctl(fd, drm::DRM_IOCTL_I915_GETPARAM, &param);
    if (cpuOnly) {
        check(gotChipset == -19, "GETPARAM CHIPSET_ID is ENODEV without the GPU");
    } else {
        check(gotChipset == 0 && chipset != 0, "GETPARAM CHIPSET_ID");
        out("gem: chipset ");
        out_hex(static_cast<uint64_t>(chipset));
        out("\n");
    }
    int softpin = 0;
    param.param = drm::I915_PARAM_HAS_EXEC_SOFTPIN;
    param.value = reinterpret_cast<uint64_t>(&softpin);
    check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GETPARAM, &param) == 0 && softpin == 1, "GETPARAM HAS_EXEC_SOFTPIN");

    uint64_t rounded = 0;
    const uint32_t target = create(fd, 5000, &rounded);
    const uint32_t batch = create(fd, 4096);
    check(target != 0 && batch != 0 && batch != target && rounded == kTargetBytes,
          "GEM_CREATE: two handles, 5000 bytes rounded to 8192");

    volatile uint32_t* targetMap = map_object(fd, target, drm::I915_MMAP_OFFSET_WB, kTargetBytes);
    volatile uint32_t* batchMap = map_object(fd, batch, drm::I915_MMAP_OFFSET_WB, 4096);
    check(targetMap && batchMap, "MMAP_OFFSET (write-back) and mmap");
    if (!targetMap || !batchMap) {
        std::close(fd);
        return false;
    }
    check(targetMap[0] == 0 && targetMap[2047] == 0 && batchMap[1023] == 0, "new objects are zeroed");

    drm::drm_i915_gem_mmap_offset gtt = {};
    gtt.handle = target;
    gtt.flags = drm::I915_MMAP_OFFSET_GTT;
    check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_MMAP_OFFSET, &gtt) == -19, "MMAP_OFFSET GTT is ENODEV (no aperture mmaps)");
    drm::drm_i915_gem_mmap_offset wb = {};
    wb.handle = target;
    wb.flags = drm::I915_MMAP_OFFSET_WB;
    sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_MMAP_OFFSET, &wb);
    check(map_error(fd, wb.offset, 3 * 4096) == -22, "mmap past the object's end is EINVAL");

    // A write-combining mapping of an object of its own (mixing caching
    // modes on one page is undefined on x86).
    const uint32_t wcObject = create(fd, 4096);
    volatile uint32_t* wcMap = map_object(fd, wcObject, drm::I915_MMAP_OFFSET_WC, 4096);
    if (wcMap) {
        for (uint32_t i = 0; i < 1024; ++i) {
            wcMap[i] = 0xA5A50000u | i;
        }
        asm volatile("sfence" ::: "memory");
    }
    bool wcOk = wcMap != nullptr;
    for (uint32_t i = 0; wcOk && i < 1024; ++i) {
        wcOk = wcMap[i] == (0xA5A50000u | i);
    }
    check(wcOk, "MMAP_OFFSET (write-combining): written and read back");

    if (cpuOnly) {
        check(execbuffer(fd, batch, target, drm::I915_EXEC_RENDER) == -19, "EXECBUFFER2 is ENODEV without the GPU");
    } else {
        check(run_on(fd, batch, target, batchMap, targetMap, drm::I915_EXEC_RENDER, 0x600DF00D, 0x600DF00E, false),
              "render: a softpinned batch stores into a two-page object at a canonical address");
        check(run_on(fd, batch, target, batchMap, targetMap, drm::I915_EXEC_BLT, 0xB1170001, 0xB1170002, false),
              "blitter: the same objects, still bound");
        if (times > 1) {
            bool all = true;
            const uint64_t start = now_ns();
            uint64_t i = 0;
            for (; i < times && all; ++i) {
                const uint64_t ring = (i % 2) ? drm::I915_EXEC_BLT : drm::I915_EXEC_RENDER;
                all = run_on(fd, batch, target, batchMap, targetMap, ring, 0x10000000u + static_cast<uint32_t>(i),
                             0x20000000u + static_cast<uint32_t>(i), true);
            }
            const uint64_t elapsed = now_ns() - start;
            out("gem: ");
            out_dec(i);
            out(" execbuffers, ");
            out_dec(elapsed / (i ? i : 1) / 1000);
            out(" us each\n");
            check(all, "repeated execbuffers on both engines");
            print_state();
        }
        check(execbuffer(fd, batch, target, drm::I915_EXEC_RENDER, 0) == -22, "an object without EXEC_OBJECT_PINNED is EINVAL");
        check(execbuffer(fd, batch, target, drm::I915_EXEC_RENDER, drm::EXEC_OBJECT_PINNED, 1) == -22,
              "relocations are EINVAL (softpin only)");
        check(execbuffer(fd, batch, 999, drm::I915_EXEC_RENDER) == -2, "an unknown handle is ENOENT");
    }

    // Closing the handle leaves the mapping working; munmap then frees it.
    targetMap[5] = 0x5EED5EED;
    check(close_handle(fd, target), "GEM_CLOSE while mapped");
    check(targetMap[5] == 0x5EED5EED, "the closed handle's object lives on in its mapping");
    check(!close_handle(fd, target), "a second GEM_CLOSE of the handle fails");
    unmap(const_cast<uint32_t*>(targetMap), kTargetBytes);
    const uint32_t again = create(fd, 4096);
    volatile uint32_t* againMap = map_object(fd, again, drm::I915_MMAP_OFFSET_WB, 4096);
    check(again == target && againMap && againMap[5] == 0, "the handle is reused for a fresh, zeroed object");

    std::close(fd);
    out(gFailures == 0 ? "gem: all checks passed\n" : "gem: some checks FAILED\n");
    return gFailures == 0;
}

// gpu-test ctx: what a 3D driver relies on in the render engine's contexts
// (step 6b): every context starts from the recorded default state (the
// context workarounds in it, no register of another context), keeps its
// registers from one batch to the next (also when its ring wraps), may write
// the whitelisted registers from a user batch, and the 3D pipe's
// PIPE_CONTROL post-sync write works from one. Contexts come from
// CONTEXT_CREATE and go with CONTEXT_DESTROY.
namespace {

constexpr uint32_t kLri2 = (0x22u << 23) | 3;          // MI_LOAD_REGISTER_IMM of 2 registers
constexpr uint32_t kLri1 = (0x22u << 23) | 1;
constexpr uint32_t kSrm = (0x24u << 23) | 2;           // MI_STORE_REGISTER_MEM (Gen8)
constexpr uint32_t kPipeControl = 0x7a000004;          // PIPE_CONTROL, 6 dwords
constexpr uint32_t kPcCsStall = 1u << 20;
constexpr uint32_t kPcQwWrite = 1u << 14;
constexpr uint32_t kGpr0 = 0x2600;                     // the render engine's CS_GPR0 (lower half)
constexpr uint32_t kCsChicken1 = 0x2580;               // whitelisted, saved with the context

struct Batch {
    volatile uint32_t* words;
    uint32_t n = 0;
    void put(uint32_t w) { words[n++] = w; }
    void store_register(uint32_t reg, uint32_t targetOffset) {
        put(kSrm);
        put(reg);
        put(static_cast<uint32_t>(kTargetVa48 + targetOffset));
        put(static_cast<uint32_t>((kTargetVa48 + targetOffset) >> 32));
    }
    void end() {
        put(kBatchEnd);
        put(0);
    }
};

uint32_t context_create(uint64_t fd) {
    drm::drm_i915_gem_context_create_ext request = {};
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &request) == 0 ? request.ctx_id : 0;
}

int64_t context_destroy(uint64_t fd, uint32_t id) {
    drm::drm_i915_gem_context_destroy request = {id, 0};
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &request);
}

int64_t set_param(uint64_t fd, uint32_t context, uint64_t param, uint64_t value, uint32_t size = 0) {
    drm::drm_i915_gem_context_param request = {context, size, param, value};
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM, &request);
}

// The value, or -errno.
int64_t get_param(uint64_t fd, uint32_t context, uint64_t param) {
    drm::drm_i915_gem_context_param request = {context, 0, param, 0};
    const int64_t result = sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, &request);
    return result != 0 ? result : static_cast<int64_t>(request.value);
}

} // namespace

bool ctx_test() {
    gFailures = 0;
    gName = "ctx";
    const uint64_t opened = _syscall_impl(static_cast<uint64_t>(std::Syscall::Open),
                                          reinterpret_cast<uint64_t>("/dev/dri/renderD128"), O_RDWR | O_CLOEXEC, 0);
    if (static_cast<int64_t>(opened) < 0) {
        out("ctx: cannot open /dev/dri/renderD128, errno ");
        out_dec(static_cast<uint64_t>(-static_cast<int64_t>(opened)));
        out("\n");
        return false;
    }
    const uint64_t fd = opened;
    const uint32_t target = create(fd, kTargetBytes);
    const uint32_t batch = create(fd, 4096);
    volatile uint32_t* t = map_object(fd, target, drm::I915_MMAP_OFFSET_WB, kTargetBytes);
    volatile uint32_t* b = map_object(fd, batch, drm::I915_MMAP_OFFSET_WB, 4096);
    const uint32_t a = context_create(fd);
    const uint32_t c = context_create(fd);
    check(t && b && a != 0 && c != 0 && a != c, "CONTEXT_CREATE: two contexts");
    if (!t || !b || !a || !c) {
        std::close(fd);
        return false;
    }
    auto run = [&](uint32_t context) {
        const int64_t queued = execbuffer(fd, batch, target, drm::I915_EXEC_RENDER, drm::EXEC_OBJECT_PINNED, 0, context);
        return queued != 0 ? queued : wait_object(fd, target);
    };

    // A fresh context: the context workarounds came with the default state
    // (CS_CHICKEN1's preemption bits are 0b010), and no GPR of anyone else.
    for (uint32_t i = 0; i < 16; ++i) {
        t[i] = 0xFFFFFFFFu;
    }
    Batch first{b};
    first.store_register(kCsChicken1, 0);
    first.store_register(kGpr0, 4);
    first.end();
    check(run(a) == 0, "a batch in a new context");
    out("ctx: a new context reads CS_CHICKEN1 ");
    out_hex(t[0]);
    out(", CS_GPR0 ");
    out_hex(t[1]);
    out("\n");
    check((t[0] & 0x7) == 0x2, "it starts with the context workarounds (CS_CHICKEN1 preemption bits 0b010)");

    // Registers last: GPR0 set in one batch, read in the next.
    Batch set{b};
    set.put(kLri2);
    set.put(kGpr0);
    set.put(0xCAFEF00D);
    set.put(kGpr0 + 4);
    set.put(0x12345678);
    set.end();
    check(run(a) == 0, "context A sets CS_GPR0");
    Batch get{b};
    get.store_register(kGpr0, 8);
    get.store_register(kGpr0 + 4, 12);
    get.end();
    t[2] = t[3] = 0;
    check(run(a) == 0 && t[2] == 0xCAFEF00D && t[3] == 0x12345678, "context A still has it in its next batch");
    t[2] = 0xFFFFFFFFu;
    check(run(c) == 0 && t[2] == 0, "context B, new, does not see it (starts from the default state)");
    Batch setB{b};
    setB.put(kLri1);
    setB.put(kGpr0);
    setB.put(0xBEEF0000);
    setB.end();
    check(run(c) == 0, "context B sets its own CS_GPR0");
    Batch getAgain{b};
    getAgain.store_register(kGpr0, 8);
    getAgain.end();
    t[2] = 0;
    check(run(a) == 0 && t[2] == 0xCAFEF00D, "context A's value is untouched by B");
    t[2] = 0;
    check(run(c) == 0 && t[2] == 0xBEEF0000, "and B keeps its own");

    // A whitelisted register, written from a user batch, per context.
    Batch chicken{b};
    chicken.put(kLri1);
    chicken.put(kCsChicken1);
    chicken.put((1u << 16) | 1u);      // object-level preemption bit on
    chicken.store_register(kCsChicken1, 16);
    chicken.end();
    t[4] = 0;
    check(run(a) == 0 && (t[4] & 1) == 1, "a user batch writes a whitelisted register (CS_CHICKEN1)");
    Batch peek{b};
    peek.store_register(kCsChicken1, 20);
    peek.end();
    t[5] = 0xFFFFFFFFu;
    check(run(c) == 0 && (t[5] & 1) == 0, "in its own context only");

    // The 3D pipe: PIPE_CONTROL's post-sync qword write from a user batch.
    Batch pc{b};
    pc.put(kPipeControl);
    pc.put(kPcCsStall | kPcQwWrite);
    pc.put(static_cast<uint32_t>(kTargetVa48 + 32));
    pc.put(static_cast<uint32_t>((kTargetVa48 + 32) >> 32));
    pc.put(0x3D3D3D3D);
    pc.put(0x11112222);
    pc.end();
    t[8] = t[9] = 0;
    check(run(a) == 0 && t[8] == 0x3D3D3D3D && t[9] == 0x11112222, "PIPE_CONTROL post-sync write from a user batch");

    // Many batches: each reads the value the last one left, through
    // several wraps of the context's ring.
    bool chain = true;
    uint32_t done = 0;
    for (uint32_t i = 0; i < 300 && chain; ++i) {
        Batch step{b};
        step.store_register(kGpr0, 40);
        step.put(kLri1);
        step.put(kGpr0);
        step.put(0x1000 + i);
        step.end();
        t[10] = 0;
        chain = run(a) == 0 && (i == 0 || t[10] == 0x1000 + i - 1);
        done = i + 1;
    }
    out("ctx: ");
    out_dec(done);
    out(" chained batches in one context\n");
    check(chain, "300 batches, each sees the register the last one set (the ring wraps)");

    check(context_destroy(fd, c) == 0, "CONTEXT_DESTROY");
    check(run(c) == -2, "a destroyed context is ENOENT");
    check(context_destroy(fd, 0) == -2, "the default context cannot be destroyed (ENOENT)");
    check(run(0) == 0, "the default context runs");

    std::close(fd);
    if (gFailures != 0) {
        print_state();
    }
    out(gFailures == 0 ? "ctx: all checks passed\n" : "ctx: some checks FAILED\n");
    return gFailures == 0;
}

// gpu-test fence: step 6c, execbuffer without waiting (i915's request queue).
// A "spinner" (a batch that jumps back to its own start, as IGT's spinners
// do, ended by the CPU writing MI_BATCH_BUFFER_END over its first command)
// keeps the render engine busy while the test checks that:
//   * execbuffer returned at once, and GEM_BUSY, GEM_WAIT and SYNCOBJ_WAIT
//     see the batch running (waits with a timeout run out);
//   * a blitter batch writing the same object waits for it (implicit
//     fencing), and one on another object marked EXEC_OBJECT_ASYNC waits for
//     the sync object the spinner signals (an explicit fence);
//   * both run once the spinner ends, and the waits then succeed;
//   * spinners ended after 0 to 100 ms finish (their breadcrumbs land);
//   * 1000 queued batches drain (throughput, one wait at the end);
//   * a spinner nobody ends is a hang: caught in about 2 s, the request
//     fails, GET_RESET_STATS counts it for its context, which is banned
//     (EIO), and the default context still works.
// Each part uses a context of its own, so a hang in one does not ban the
// default context the others use. Lines carry the ms since the start, and
// the driver's state report comes at the end.
namespace {

constexpr uint64_t kSpinVa = 0x20000;
constexpr uint64_t kOtherVa = 0x30000;
constexpr uint32_t kArbCheck = 0x05u << 23;            // MI_ARB_CHECK
constexpr uint32_t kBatchStartPpgtt = (0x31u << 23) | (1u << 8) | 1u;   // MI_BATCH_BUFFER_START, PPGTT

struct Obj {
    uint32_t handle;
    uint64_t va;
    uint64_t flags;
};

// Objects as given, the batch last; fences optional.
int64_t submit(uint64_t fd, const Obj* objs, uint32_t count, uint64_t ring, uint32_t context = 0,
               drm::drm_i915_gem_exec_fence* fences = nullptr, uint32_t fenceCount = 0) {
    drm::drm_i915_gem_exec_object2 list[4] = {};
    for (uint32_t i = 0; i < count; ++i) {
        list[i].handle = objs[i].handle;
        list[i].offset = objs[i].va;
        list[i].flags = drm::EXEC_OBJECT_PINNED | drm::EXEC_OBJECT_SUPPORTS_48B_ADDRESS | objs[i].flags;
    }
    drm::drm_i915_gem_execbuffer2 exec = {};
    exec.buffers_ptr = reinterpret_cast<uint64_t>(list);
    exec.buffer_count = count;
    exec.flags = ring | drm::I915_EXEC_NO_RELOC | drm::I915_EXEC_HANDLE_LUT;
    exec.rsvd1 = context;
    if (fences) {
        exec.flags |= drm::I915_EXEC_FENCE_ARRAY;
        exec.cliprects_ptr = reinterpret_cast<uint64_t>(fences);
        exec.num_cliprects = fenceCount;
    }
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec);
}

void write_spinner(volatile uint32_t* spin) {
    spin[0] = kArbCheck;
    spin[1] = kBatchStartPpgtt;
    spin[2] = static_cast<uint32_t>(kSpinVa);
    spin[3] = static_cast<uint32_t>(kSpinVa >> 32);
    asm volatile("mfence" ::: "memory");
}

void end_spinner(volatile uint32_t* spin) {
    spin[0] = kBatchEnd;
    asm volatile("mfence" ::: "memory");
}

// A batch that stores `value` at `va` and ends.
void write_store(volatile uint32_t* batch, uint64_t va, uint32_t value) {
    batch[0] = kStoreDword;
    batch[1] = static_cast<uint32_t>(va);
    batch[2] = static_cast<uint32_t>(va >> 32);
    batch[3] = value;
    batch[4] = kBatchEnd;
    batch[5] = 0;
}

uint32_t syncobj_create(uint64_t fd, bool signaled) {
    drm::drm_syncobj_create request = {};
    request.flags = signaled ? drm::DRM_SYNCOBJ_CREATE_SIGNALED : 0;
    return sys_ioctl(fd, drm::DRM_IOCTL_SYNCOBJ_CREATE, &request) == 0 ? request.handle : 0;
}

int64_t syncobj_wait(uint64_t fd, uint32_t handle, uint64_t forNs) {
    drm::drm_syncobj_wait request = {};
    request.handles = reinterpret_cast<uint64_t>(&handle);
    request.count_handles = 1;
    request.timeout_nsec = static_cast<int64_t>(now_ns() + forNs);
    return sys_ioctl(fd, drm::DRM_IOCTL_SYNCOBJ_WAIT, &request);
}

uint32_t busy(uint64_t fd, uint32_t handle) {
    drm::drm_i915_gem_busy request = {};
    request.handle = handle;
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_BUSY, &request) == 0 ? request.busy : 0xFFFFFFFFu;
}

} // namespace

bool fence_test() {
    gFailures = 0;
    gName = "fence";
    gStartNs = now_ns();
    const uint64_t opened = _syscall_impl(static_cast<uint64_t>(std::Syscall::Open),
                                          reinterpret_cast<uint64_t>("/dev/dri/renderD128"), O_RDWR | O_CLOEXEC, 0);
    if (static_cast<int64_t>(opened) < 0) {
        out("fence: cannot open /dev/dri/renderD128, errno ");
        out_dec(static_cast<uint64_t>(-static_cast<int64_t>(opened)));
        out("\n");
        gStartNs = 0;
        return false;
    }
    const uint64_t fd = opened;

    drm::drm_get_cap cap = {drm::DRM_CAP_SYNCOBJ, 0};
    int fenceArray = 0;
    drm::drm_i915_getparam param = {};
    param.param = drm::I915_PARAM_HAS_EXEC_FENCE_ARRAY;
    param.value = reinterpret_cast<uint64_t>(&fenceArray);
    check(sys_ioctl(fd, drm::DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1 &&
              sys_ioctl(fd, drm::DRM_IOCTL_I915_GETPARAM, &param) == 0 && fenceArray == 1,
          "DRM_CAP_SYNCOBJ and HAS_EXEC_FENCE_ARRAY");

    const uint32_t spin = create(fd, 4096);
    const uint32_t store = create(fd, 4096);
    const uint32_t target = create(fd, kTargetBytes);
    const uint32_t other = create(fd, 4096);
    volatile uint32_t* spinMap = map_object(fd, spin, drm::I915_MMAP_OFFSET_WB, 4096);
    volatile uint32_t* storeMap = map_object(fd, store, drm::I915_MMAP_OFFSET_WB, 4096);
    volatile uint32_t* t = map_object(fd, target, drm::I915_MMAP_OFFSET_WB, kTargetBytes);
    volatile uint32_t* o = map_object(fd, other, drm::I915_MMAP_OFFSET_WB, 4096);
    const uint32_t done = syncobj_create(fd, false);
    const uint32_t already = syncobj_create(fd, true);
    const uint32_t spinCtx = context_create(fd);
    check(spinMap && storeMap && t && o && done && already && spinCtx, "objects, sync objects, a context for the spinner");
    if (!spinMap || !storeMap || !t || !o || !done || !already || !spinCtx) {
        std::close(fd);
        gStartNs = 0;
        return false;
    }
    check(syncobj_wait(fd, already, 0) == 0, "a sync object created signaled is signaled");
    check(syncobj_wait(fd, done, 0) == -22, "waiting on one without a fence is EINVAL");

    // The spinner on the render engine, in a context of its own, writing the
    // target, signalling `done`.
    write_spinner(spinMap);
    t[0] = 0;
    o[0] = 0;
    Obj spinObjs[2] = {{target, kTargetVaCanonical, drm::EXEC_OBJECT_WRITE}, {spin, kSpinVa, 0}};
    drm::drm_i915_gem_exec_fence signal = {done, drm::I915_EXEC_FENCE_SIGNAL};
    const uint64_t before = now_ns();
    const int64_t queued = submit(fd, spinObjs, 2, drm::I915_EXEC_RENDER, spinCtx, &signal, 1);
    const uint64_t took = now_ns() - before;
    out("fence: execbuffer of the spinner returned after ");
    out_dec(took / 1000);
    out_errno(" us, result", queued);
    out("\n");
    check(queued == 0 && took < 50 * 1000 * 1000ull, "execbuffer returns while its batch runs");
    check(busy(fd, target) != 0, "GEM_BUSY: the target is busy");
    check(wait_object(fd, target, 20 * 1000 * 1000) == -62, "GEM_WAIT with a 20 ms timeout runs out (ETIME)");
    check(syncobj_wait(fd, done, 20 * 1000 * 1000) == -62, "SYNCOBJ_WAIT on its fence runs out too");

    // Implicit: a blitter write to the same object waits for the spinner.
    write_store(storeMap, kTargetVa48, 0x51DE0001);
    Obj storeObjs[2] = {{target, kTargetVaCanonical, drm::EXEC_OBJECT_WRITE}, {store, kBatchVa, 0}};
    const int64_t implicitQueued = submit(fd, storeObjs, 2, drm::I915_EXEC_BLT);
    check(implicitQueued == 0, "a blitter write to the same object is queued");
    // Explicit: a blitter store to another object (ASYNC: no implicit wait)
    // behind the sync object, from a second store batch.
    const uint32_t store2 = create(fd, 4096);
    volatile uint32_t* store2Map = map_object(fd, store2, drm::I915_MMAP_OFFSET_WB, 4096);
    if (store2Map) {
        write_store(store2Map, kOtherVa, 0xFE0CE001);
    }
    Obj otherObjs[2] = {{other, kOtherVa, drm::EXEC_OBJECT_WRITE | drm::EXEC_OBJECT_ASYNC}, {store2, kBatchVa + 0x4000, 0}};
    drm::drm_i915_gem_exec_fence waitDone = {done, drm::I915_EXEC_FENCE_WAIT};
    check(store2Map && submit(fd, otherObjs, 2, drm::I915_EXEC_BLT, 0, &waitDone, 1) == 0,
          "a batch waiting on the spinner's sync object is queued");
    std::sleep(50);
    check(t[0] == 0 && o[0] == 0, "50 ms later neither has run: both wait for the spinner");

    end_spinner(spinMap);
    const int64_t spinWait = syncobj_wait(fd, done, 1000 * 1000 * 1000ull);
    check(spinWait == 0, "the spinner ends: its sync object signals");
    drm::drm_i915_reset_stats spinStats = {};
    spinStats.ctx_id = spinCtx;
    check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GET_RESET_STATS, &spinStats) == 0 && spinStats.batch_active == 0 &&
              spinStats.batch_pending == 0,
          "it finished by itself (GET_RESET_STATS: no hang in its context)");
    const int64_t implicitWait = wait_object(fd, target, 1000 * 1000 * 1000ll);
    const bool implicitOk = implicitWait == 0 && t[0] == 0x51DE0001;
    check(implicitOk, "the blitter's write ran after it (implicit fencing)");
    if (!implicitOk) {
        out_errno("fence:   GEM_WAIT", implicitWait);
        out(", the target holds ");
        out_hex(t[0]);
        out("\n");
    }
    const int64_t explicitWait = wait_object(fd, other, 1000 * 1000 * 1000ll);
    const bool explicitOk = explicitWait == 0 && o[0] == 0xFE0CE001;
    check(explicitOk, "and the batch behind the sync object ran (explicit fence)");
    if (!explicitOk) {
        out_errno("fence:   GEM_WAIT", explicitWait);
        out(", the object holds ");
        out_hex(o[0]);
        out("\n");
    }
    check(busy(fd, target) == 0 && busy(fd, other) == 0, "GEM_BUSY: idle again");

    // Spinners ended after 0, 1, 5, 20 and 100 ms: the breadcrumb after a
    // batch that ran a while must land. Each in a fresh context.
    {
        const uint32_t delays[] = {0, 1, 5, 20, 100};
        bool all = true;
        for (uint32_t delay : delays) {
            const uint32_t ctx = context_create(fd);
            const uint32_t sync = syncobj_create(fd, false);
            write_spinner(spinMap);
            Obj objs[1] = {{spin, kSpinVa, 0}};
            drm::drm_i915_gem_exec_fence sig = {sync, drm::I915_EXEC_FENCE_SIGNAL};
            const int64_t q = submit(fd, objs, 1, drm::I915_EXEC_RENDER, ctx, &sig, 1);
            if (delay) {
                std::sleep(delay);
            }
            end_spinner(spinMap);
            const uint64_t ended = now_ns();
            const int64_t w = q == 0 ? syncobj_wait(fd, sync, 3000ull * 1000 * 1000) : q;
            const uint64_t after = (now_ns() - ended) / 1000;
            drm::drm_i915_reset_stats st = {};
            st.ctx_id = ctx;
            const bool hung = sys_ioctl(fd, drm::DRM_IOCTL_I915_GET_RESET_STATS, &st) != 0 || st.batch_active != 0;
            const bool ok = q == 0 && w == 0 && !hung;
            if (!ok || delay == 100) {
                out("fence: a spinner ended after ");
                out_dec(delay);
                out(" ms: ");
                out(ok ? "done " : "FAILED ");
                out_dec(after);
                out(" us after the end");
                out_errno(", execbuffer", q);
                out_errno(", wait", w);
                out(hung ? ", it hung\n" : "\n");
            }
            all = all && ok;
        }
        check(all, "spinners ended after 0 to 100 ms all finish (no hang)");
    }

    // Throughput: queue 1000 on the default context, wait once.
    write_store(storeMap, kTargetVa48, 0x7777);
    t[0] = 0;
    int64_t firstError = 0;
    uint32_t queuedCount = 0;
    const uint64_t start = now_ns();
    for (uint32_t i = 0; i < 1000 && firstError == 0; ++i) {
        firstError = submit(fd, storeObjs, 2, (i % 2) ? drm::I915_EXEC_BLT : drm::I915_EXEC_RENDER);
        if (firstError == 0) {
            ++queuedCount;
        }
    }
    const uint64_t submitted = now_ns() - start;
    const int64_t drainWait = wait_object(fd, target);
    const uint64_t total = now_ns() - start;
    out("fence: ");
    out_dec(queuedCount);
    out(" batches queued in ");
    out_dec(submitted / 1000);
    out(" us, all done after ");
    out_dec(total / 1000);
    out(" us (");
    out_dec(queuedCount ? total / 1000 / queuedCount : 0);
    out(" us each)");
    if (firstError != 0) {
        out_errno("; execbuffer", firstError);
    }
    out_errno("; GEM_WAIT", drainWait);
    out(", the target holds ");
    out_hex(t[0]);
    out("\n");
    check(queuedCount == 1000 && drainWait == 0 && t[0] == 0x7777, "1000 queued batches on both engines drain");

    // A hang: a spinner in a context of its own that nobody ends.
    // Not recoverable, as Mesa makes its contexts: a hang bans it.
    const uint32_t hangCtx = context_create(fd);
    check(hangCtx != 0 && set_param(fd, hangCtx, drm::I915_CONTEXT_PARAM_RECOVERABLE, 0) == 0,
          "a context for the hang (RECOVERABLE cleared)");
    const uint32_t hung = syncobj_create(fd, false);
    write_spinner(spinMap);
    drm::drm_i915_gem_exec_fence signalHung = {hung, drm::I915_EXEC_FENCE_SIGNAL};
    Obj hangObjs[1] = {{spin, kSpinVa, 0}};
    check(submit(fd, hangObjs, 1, drm::I915_EXEC_RENDER, hangCtx, &signalHung, 1) == 0, "a spinner nobody ends");
    const uint64_t hangStart = now_ns();
    const int64_t waited = syncobj_wait(fd, hung, 6000ull * 1000 * 1000);
    const uint64_t hangMs = (now_ns() - hangStart) / 1000000;
    out("fence: the hang was dealt with after ");
    out_dec(hangMs);
    out(" ms\n");
    check(waited == 0 && hangMs >= 1500 && hangMs < 5000, "it is caught as a hang in about 2 s and its fence signals");
    drm::drm_i915_reset_stats stats = {};
    stats.ctx_id = hangCtx;
    check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GET_RESET_STATS, &stats) == 0 && stats.batch_active == 1,
          "GET_RESET_STATS: one batch of the context hung");
    check(submit(fd, hangObjs, 1, drm::I915_EXEC_RENDER, hangCtx) == -5, "the context is banned (EIO)");
    end_spinner(spinMap);   // the hung batch is gone; tidy for whatever runs next
    write_store(storeMap, kTargetVa48, 0xA11FE11);
    t[0] = 0;
    const int64_t afterQueued = submit(fd, storeObjs, 2, drm::I915_EXEC_RENDER);
    const int64_t afterWait = afterQueued == 0 ? wait_object(fd, target, 2000ll * 1000 * 1000) : afterQueued;
    const bool afterOk = afterWait == 0 && t[0] == 0xA11FE11;
    check(afterOk, "the default context works after the reset");
    if (!afterOk) {
        out_errno("fence:   execbuffer", afterQueued);
        out_errno(", GEM_WAIT", afterWait);
        out(", the target holds ");
        out_hex(t[0]);
        out("\n");
    }

    // A recoverable context (i915's default) is not banned by its hang: its
    // next batch runs (from the default state).
    {
        const uint32_t ctx = context_create(fd);
        const uint32_t sync = syncobj_create(fd, false);
        write_spinner(spinMap);
        drm::drm_i915_gem_exec_fence sig = {sync, drm::I915_EXEC_FENCE_SIGNAL};
        Obj objs[1] = {{spin, kSpinVa, 0}};
        const int64_t q = submit(fd, objs, 1, drm::I915_EXEC_RENDER, ctx, &sig, 1);
        const int64_t w = q == 0 ? syncobj_wait(fd, sync, 6000ull * 1000 * 1000) : q;
        end_spinner(spinMap);
        write_store(storeMap, kTargetVa48, 0x8ECE0001);
        t[0] = 0;
        Obj again[2] = {{target, kTargetVaCanonical, drm::EXEC_OBJECT_WRITE}, {store, kBatchVa, 0}};
        const int64_t q2 = submit(fd, again, 2, drm::I915_EXEC_RENDER, ctx);
        const int64_t w2 = q2 == 0 ? wait_object(fd, target, 2000ll * 1000 * 1000) : q2;
        drm::drm_i915_reset_stats st = {};
        st.ctx_id = ctx;
        const bool counted = sys_ioctl(fd, drm::DRM_IOCTL_I915_GET_RESET_STATS, &st) == 0 && st.batch_active == 1;
        const bool ok = q == 0 && w == 0 && q2 == 0 && w2 == 0 && t[0] == 0x8ECE0001 && counted;
        check(ok, "a recoverable context's hang is counted, and its next batch runs");
        if (!ok) {
            out_errno("fence:   execbuffer", q);
            out_errno(", wait", w);
            out_errno(", next execbuffer", q2);
            out_errno(", GEM_WAIT", w2);
            out(", the target holds ");
            out_hex(t[0]);
            out("\n");
        }
    }

    std::close(fd);
    gStartNs = 0;
    print_state();
    out(gFailures == 0 ? "fence: all checks passed\n" : "fence: some checks FAILED\n");
    return gFailures == 0;
}

// gpu-test uapi: step 6d, the rest of what Mesa's i915 code (iris, anv,
// intel/dev and intel/common) asks the kernel before and while it renders:
// GETPARAM, QUERY (topology, engines, memory regions), context parameters
// and CONTEXT_CREATE_EXT's SETPARAM chain, address spaces (VM_CREATE and
// CONTEXT_PARAM_VM), engine maps, single-timeline contexts, the timestamp
// register, the aperture, tiling, caching, madvise and GEM_CREATE_EXT.
// `cpu-only` (QEMU, no Intel GPU) skips what needs the GPU.
namespace {

int64_t getparam(uint64_t fd, int32_t param) {
    int value = 0;
    drm::drm_i915_getparam request = {};
    request.param = param;
    request.value = reinterpret_cast<uint64_t>(&value);
    const int64_t result = sys_ioctl(fd, drm::DRM_IOCTL_I915_GETPARAM, &request);
    return result != 0 ? result : value;
}

// One QUERY item; its length afterwards (bytes, or -errno).
int32_t query(uint64_t fd, uint64_t id, void* data, int32_t length, uint32_t flags = 0) {
    drm::drm_i915_query_item item = {};
    item.query_id = id;
    item.length = length;
    item.flags = flags;
    item.data_ptr = reinterpret_cast<uint64_t>(data);
    drm::drm_i915_query request = {};
    request.num_items = 1;
    request.items_ptr = reinterpret_cast<uint64_t>(&item);
    if (sys_ioctl(fd, drm::DRM_IOCTL_I915_QUERY, &request) != 0) {
        return -1000;
    }
    return item.length;
}

uint32_t vm_create(uint64_t fd) {
    drm::drm_i915_gem_vm_control request = {};
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_VM_CREATE, &request) == 0 ? request.vm_id : 0;
}

int64_t vm_destroy(uint64_t fd, uint32_t id) {
    drm::drm_i915_gem_vm_control request = {};
    request.vm_id = id;
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_VM_DESTROY, &request);
}

// CONTEXT_CREATE_EXT with a chain of SETPARAM extensions; the id, or -errno.
int64_t context_create_with(uint64_t fd, drm::drm_i915_gem_context_create_ext_setparam* params, uint32_t count,
                            uint32_t flags = 0) {
    for (uint32_t i = 0; i < count; ++i) {
        params[i].base.next_extension = i + 1 < count ? reinterpret_cast<uint64_t>(&params[i + 1]) : 0;
    }
    drm::drm_i915_gem_context_create_ext request = {};
    request.flags = flags | (count ? drm::I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS : 0);
    request.extensions = count ? reinterpret_cast<uint64_t>(params) : 0;
    const int64_t result = sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &request);
    return result != 0 ? result : request.ctx_id;
}

drm::drm_i915_gem_context_create_ext_setparam setparam_ext(uint64_t param, uint64_t value, uint32_t size = 0) {
    drm::drm_i915_gem_context_create_ext_setparam ext = {};
    ext.base.name = drm::I915_CONTEXT_CREATE_EXT_SETPARAM;
    ext.param.param = param;
    ext.param.value = value;
    ext.param.size = size;
    return ext;
}

struct EngineMap {
    uint64_t extensions;
    drm::i915_engine_class_instance engines[3];
} __attribute__((packed));

} // namespace

bool uapi_test(bool cpuOnly) {
    gFailures = 0;
    gName = "uapi";
    const uint64_t opened = _syscall_impl(static_cast<uint64_t>(std::Syscall::Open),
                                          reinterpret_cast<uint64_t>("/dev/dri/renderD128"), O_RDWR | O_CLOEXEC, 0);
    if (static_cast<int64_t>(opened) < 0) {
        out("uapi: cannot open /dev/dri/renderD128, errno ");
        out_dec(static_cast<uint64_t>(-static_cast<int64_t>(opened)));
        out("\n");
        return false;
    }
    const uint64_t fd = opened;

    // GETPARAM: what anv refuses to start without, then the rest.
    check(getparam(fd, drm::I915_PARAM_HAS_WAIT_TIMEOUT) == 1 && getparam(fd, drm::I915_PARAM_HAS_EXECBUF2) == 1 &&
              getparam(fd, drm::I915_PARAM_HAS_EXEC_SOFTPIN) == 1 &&
              getparam(fd, drm::I915_PARAM_HAS_EXEC_FENCE_ARRAY) == 1,
          "GETPARAM: wait timeout, execbuf2, softpin, fence arrays (anv's requirements)");
    check(getparam(fd, drm::I915_PARAM_HAS_ALIASING_PPGTT) == 3 && getparam(fd, drm::I915_PARAM_MMAP_GTT_VERSION) >= 4 &&
              getparam(fd, drm::I915_PARAM_HAS_CONTEXT_ISOLATION) == 3 && getparam(fd, drm::I915_PARAM_HAS_LLC) == 1 &&
              getparam(fd, drm::I915_PARAM_HAS_EXEC_CAPTURE) == 1 && getparam(fd, drm::I915_PARAM_HAS_GPU_RESET) == 2,
          "GETPARAM: full PPGTT, MMAP_OFFSET, isolated render and copy contexts, LLC, capture, engine reset");
    check(getparam(fd, drm::I915_PARAM_HAS_BSD) == 0 && getparam(fd, drm::I915_PARAM_HAS_SCHEDULER) == 0 &&
              getparam(fd, drm::I915_PARAM_PXP_STATUS) == -19 && getparam(fd, drm::I915_PARAM_HUC_STATUS) == -19 &&
              getparam(fd, 9999) == -22,
          "GETPARAM: no video engine, no scheduler, PXP and HuC ENODEV, unknown EINVAL");
    if (!cpuOnly) {
        const int64_t freq = getparam(fd, drm::I915_PARAM_CS_TIMESTAMP_FREQUENCY);
        const int64_t eus = getparam(fd, drm::I915_PARAM_EU_TOTAL);
        out("uapi: timestamps at ");
        out_dec(freq > 0 ? static_cast<uint64_t>(freq) : 0);
        out(" Hz, revision ");
        out_dec(static_cast<uint64_t>(getparam(fd, drm::I915_PARAM_REVISION)));
        out(", ");
        out_dec(eus > 0 ? static_cast<uint64_t>(eus) : 0);
        out(" EUs, slices ");
        out_hex(static_cast<uint64_t>(getparam(fd, drm::I915_PARAM_SLICE_MASK)));
        out(", subslices ");
        out_hex(static_cast<uint64_t>(getparam(fd, drm::I915_PARAM_SUBSLICE_MASK)));
        out("\n");
        check(freq == 12000000, "CS_TIMESTAMP_FREQUENCY is Skylake's 12 MHz");
        check(eus > 0 && getparam(fd, drm::I915_PARAM_SUBSLICE_TOTAL) > 0 && getparam(fd, drm::I915_PARAM_SLICE_MASK) > 0,
              "EU_TOTAL, SUBSLICE_TOTAL, SLICE_MASK from the fuses");

        // QUERY: the topology, as Mesa's intel_device_info reads it.
        uint8_t topo[64] = {};
        const int32_t needed = query(fd, drm::DRM_I915_QUERY_TOPOLOGY_INFO, nullptr, 0);
        const int32_t got = query(fd, drm::DRM_I915_QUERY_TOPOLOGY_INFO, topo, sizeof(topo));
        uint32_t counted = 0;
        for (int32_t i = 16 + 4; i < 32 && got == 32; ++i) {
            for (uint8_t m = topo[i]; m; m &= m - 1) {
                ++counted;
            }
        }
        check(needed == 32 && got == 32 && topo[16] == static_cast<uint8_t>(getparam(fd, drm::I915_PARAM_SLICE_MASK)) &&
                  counted == static_cast<uint64_t>(eus),
              "QUERY topology: 32 bytes, its EU masks add up to EU_TOTAL");
        check(query(fd, drm::DRM_I915_QUERY_TOPOLOGY_INFO, topo, 8) == -22, "QUERY with too small a buffer is EINVAL");
    }
    struct {
        drm::drm_i915_query_engine_info header;
        drm::drm_i915_engine_info engines[4];
    } engines = {};
    const int32_t engineBytes = query(fd, drm::DRM_I915_QUERY_ENGINE_INFO, &engines, sizeof(engines));
    check(engineBytes == 16 + 2 * 56 && engines.header.num_engines == 2 &&
              engines.engines[0].engine.engine_class == drm::I915_ENGINE_CLASS_RENDER &&
              engines.engines[1].engine.engine_class == drm::I915_ENGINE_CLASS_COPY,
          "QUERY engine info: the render and copy engines");
    struct {
        drm::drm_i915_query_memory_regions header;
        drm::drm_i915_memory_region_info regions[2];
    } regions = {};
    const int32_t regionBytes = query(fd, drm::DRM_I915_QUERY_MEMORY_REGIONS, &regions, sizeof(regions));
    out("uapi: system memory ");
    out_dec(regions.regions[0].probed_size >> 20);
    out(" MiB, ");
    out_dec(regions.regions[0].unallocated_size >> 20);
    out(" MiB free\n");
    check(regionBytes == 16 + 88 && regions.header.num_regions == 1 &&
              regions.regions[0].region.memory_class == drm::I915_MEMORY_CLASS_SYSTEM && regions.regions[0].probed_size > 0,
          "QUERY memory regions: system memory only");
    regions.header.rsvd[0] = 1;
    check(query(fd, drm::DRM_I915_QUERY_MEMORY_REGIONS, &regions, sizeof(regions)) == -22,
          "QUERY with a header that is not zero is EINVAL");
    check(query(fd, drm::DRM_I915_QUERY_HWCONFIG_BLOB, nullptr, 0) == -19 && query(fd, 99, nullptr, 0) == -22,
          "QUERY hwconfig ENODEV (no GuC), unknown EINVAL");

    // Context parameters on the default context.
    check(get_param(fd, 0, drm::I915_CONTEXT_PARAM_GTT_SIZE) == (1ll << 48), "CONTEXT_GETPARAM GTT_SIZE: 48 bits");
    check(get_param(fd, 0, drm::I915_CONTEXT_PARAM_RECOVERABLE) == 1 && get_param(fd, 0, drm::I915_CONTEXT_PARAM_BANNABLE) == 1 &&
              get_param(fd, 0, drm::I915_CONTEXT_PARAM_PERSISTENCE) == 1,
          "the defaults: recoverable, bannable, persistent");
    const uint32_t c = context_create(fd);
    check(c != 0 && set_param(fd, c, drm::I915_CONTEXT_PARAM_RECOVERABLE, 0) == 0 &&
              get_param(fd, c, drm::I915_CONTEXT_PARAM_RECOVERABLE) == 0,
          "SETPARAM RECOVERABLE 0 (what iris does)");
    check(set_param(fd, c, drm::I915_CONTEXT_PARAM_PRIORITY, 0) == 0 &&
              set_param(fd, c, drm::I915_CONTEXT_PARAM_PRIORITY, 1) == -19 &&
              set_param(fd, c, drm::I915_CONTEXT_PARAM_PRIORITY, 5000) == -22,
          "PRIORITY: the default only (ENODEV: no scheduler), out of range EINVAL");
    check(set_param(fd, c, drm::I915_CONTEXT_PARAM_RECOVERABLE, 0, 8) == -22 &&
              set_param(fd, c, drm::I915_CONTEXT_PARAM_GTT_SIZE, 1) == -22 && set_param(fd, 77, drm::I915_CONTEXT_PARAM_RECOVERABLE, 0) == -2,
          "SETPARAM: a size for a scalar EINVAL, GTT_SIZE read only, unknown context ENOENT");

    // Address spaces.
    const int64_t defaultVm = get_param(fd, 0, drm::I915_CONTEXT_PARAM_VM);
    check(defaultVm > 0 && get_param(fd, 0, drm::I915_CONTEXT_PARAM_VM) == defaultVm &&
              get_param(fd, c, drm::I915_CONTEXT_PARAM_VM) == defaultVm,
          "CONTEXT_GETPARAM VM: the file's default address space has an id, shared by its contexts");
    const uint32_t vm = vm_create(fd);
    drm::drm_i915_gem_context_create_ext_setparam withVm[2] = {
        setparam_ext(drm::I915_CONTEXT_PARAM_VM, vm), setparam_ext(drm::I915_CONTEXT_PARAM_RECOVERABLE, 0)};
    const int64_t inVm = context_create_with(fd, withVm, 2);
    check(vm != 0 && vm != defaultVm && inVm > 0 && get_param(fd, static_cast<uint32_t>(inVm), drm::I915_CONTEXT_PARAM_VM) == vm &&
              get_param(fd, static_cast<uint32_t>(inVm), drm::I915_CONTEXT_PARAM_RECOVERABLE) == 0,
          "VM_CREATE, and CONTEXT_CREATE_EXT with SETPARAM VM and RECOVERABLE (anv's way)");
    drm::drm_i915_gem_context_create_ext_setparam badVm[1] = {setparam_ext(drm::I915_CONTEXT_PARAM_VM, 63)};
    drm::drm_i915_gem_context_create_ext_setparam clone[1] = {setparam_ext(0, 0)};
    clone[0].base.name = drm::I915_CONTEXT_CREATE_EXT_CLONE;
    drm::drm_i915_gem_context_create_ext_setparam withId[1] = {setparam_ext(drm::I915_CONTEXT_PARAM_RECOVERABLE, 0)};
    withId[0].param.ctx_id = 1;
    check(context_create_with(fd, badVm, 1) == -2 && context_create_with(fd, clone, 1) == -22 &&
              context_create_with(fd, withId, 1) == -22,
          "CREATE_EXT: an unknown VM ENOENT, CLONE EINVAL, a ctx_id in SETPARAM EINVAL");
    check(vm_destroy(fd, vm) == 0 && vm_destroy(fd, vm) == -2 &&
              get_param(fd, static_cast<uint32_t>(inVm), drm::I915_CONTEXT_PARAM_GTT_SIZE) == (1ll << 48),
          "VM_DESTROY drops the id; the context keeps its address space");

    // An engine map: [copy, render, empty].
    EngineMap map = {};
    map.engines[0] = {drm::I915_ENGINE_CLASS_COPY, 0};
    map.engines[1] = {drm::I915_ENGINE_CLASS_RENDER, 0};
    map.engines[2] = {drm::I915_ENGINE_CLASS_INVALID_NONE, drm::I915_ENGINE_CLASS_INVALID_NONE};
    drm::drm_i915_gem_context_create_ext_setparam withMap[1] = {
        setparam_ext(drm::I915_CONTEXT_PARAM_ENGINES, reinterpret_cast<uint64_t>(&map), sizeof(map))};
    const int64_t mapped = context_create_with(fd, withMap, 1);
    EngineMap back = {};
    drm::drm_i915_gem_context_param getMap = {static_cast<uint32_t>(mapped), sizeof(back), drm::I915_CONTEXT_PARAM_ENGINES,
                                              reinterpret_cast<uint64_t>(&back)};
    check(mapped > 0 && sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, &getMap) == 0 && getMap.size == sizeof(map) &&
              back.engines[0].engine_class == drm::I915_ENGINE_CLASS_COPY &&
              back.engines[1].engine_class == drm::I915_ENGINE_CLASS_RENDER,
          "CONTEXT_PARAM_ENGINES: a map of copy, render and an empty slot, read back");
    EngineMap video = map;
    video.engines[0] = {drm::I915_ENGINE_CLASS_VIDEO, 0};
    drm::drm_i915_gem_context_create_ext_setparam withVideo[1] = {
        setparam_ext(drm::I915_CONTEXT_PARAM_ENGINES, reinterpret_cast<uint64_t>(&video), sizeof(video))};
    check(context_create_with(fd, withVideo, 1) == -2, "a map with a video engine (there is none) is ENOENT");

    // Tiling, caching, madvise, CREATE_EXT.
    const uint32_t bo = create(fd, 64 * 1024);
    drm::drm_i915_gem_set_tiling tiling = {bo, drm::I915_TILING_X, 512, 99};
    drm::drm_i915_gem_get_tiling got = {bo, 0, 0, 0};
    check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_SET_TILING, &tiling) == 0 && tiling.swizzle_mode == drm::I915_BIT_6_SWIZZLE_NONE &&
              sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_GET_TILING, &got) == 0 && got.tiling_mode == drm::I915_TILING_X,
          "SET_TILING X / GET_TILING (no swizzling on Gen9)");
    drm::drm_i915_gem_set_tiling badStride = {bo, drm::I915_TILING_X, 100, 0};
    drm::drm_i915_gem_set_tiling yTiled = {bo, drm::I915_TILING_Y, 128, 0};
    drm::drm_i915_gem_set_tiling badMode = {bo, 7, 512, 0};
    check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_SET_TILING, &badStride) == -22 &&
              sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_SET_TILING, &yTiled) == 0 &&
              sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_SET_TILING, &badMode) == -22,
          "a stride that is not a tile multiple EINVAL, Y with 128 ok, an unknown mode EINVAL");
    drm::drm_i915_gem_caching caching = {bo, 99};
    const bool cachedFirst = sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_GET_CACHING, &caching) == 0 &&
                             caching.caching == drm::I915_CACHING_CACHED;
    drm::drm_i915_gem_caching none = {bo, drm::I915_CACHING_NONE};
    drm::drm_i915_gem_caching bad = {bo, 7};
    check(cachedFirst && sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_SET_CACHING, &none) == 0 &&
              sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_GET_CACHING, &caching) == 0 && caching.caching == drm::I915_CACHING_NONE &&
              sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_SET_CACHING, &bad) == -22,
          "GET_CACHING: CACHED by default; SET_CACHING NONE; an unknown level EINVAL");
    drm::drm_i915_gem_madvise madvise = {bo, drm::I915_MADV_DONTNEED, 0};
    drm::drm_i915_gem_madvise badMadv = {bo, 9, 0};
    check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_MADVISE, &madvise) == 0 && madvise.retained == 1 &&
              sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_MADVISE, &badMadv) == -22,
          "MADVISE DONTNEED: retained; an unknown advice EINVAL");
    drm::drm_i915_gem_memory_class_instance system = {drm::I915_MEMORY_CLASS_SYSTEM, 0};
    drm::drm_i915_gem_create_ext_memory_regions regionsExt = {};
    regionsExt.base.name = drm::I915_GEM_CREATE_EXT_MEMORY_REGIONS;
    regionsExt.num_regions = 1;
    regionsExt.regions = reinterpret_cast<uint64_t>(&system);
    drm::drm_i915_gem_create_ext createExt = {5000, 0, 0, reinterpret_cast<uint64_t>(&regionsExt)};
    check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CREATE_EXT, &createExt) == 0 && createExt.handle != 0 && createExt.size == 8192,
          "GEM_CREATE_EXT in system memory (iris's way once regions are queried)");
    drm::drm_i915_gem_memory_class_instance device = {drm::I915_MEMORY_CLASS_DEVICE, 0};
    regionsExt.regions = reinterpret_cast<uint64_t>(&device);
    drm::drm_i915_gem_create_ext inDevice = {4096, 0, 0, reinterpret_cast<uint64_t>(&regionsExt)};
    drm::i915_user_extension pat = {};
    pat.name = drm::I915_GEM_CREATE_EXT_SET_PAT;
    drm::drm_i915_gem_create_ext withPat = {4096, 0, 0, reinterpret_cast<uint64_t>(&pat)};
    drm::drm_i915_gem_create_ext cpuAccess = {4096, 0, drm::I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS, 0};
    check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CREATE_EXT, &inDevice) == -22 &&
              sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CREATE_EXT, &withPat) == -19 &&
              sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_CREATE_EXT, &cpuAccess) == -22,
          "CREATE_EXT: device memory EINVAL (none), SET_PAT ENODEV (before Meteor Lake), NEEDS_CPU_ACCESS EINVAL");

    if (!cpuOnly) {
        // The timestamp register, as Mesa reads it for timer queries.
        drm::drm_i915_reg_read first = {0x2358 | drm::I915_REG_READ_8B_WA, 0};
        drm::drm_i915_reg_read second = {0x2358 | drm::I915_REG_READ_8B_WA, 0};
        drm::drm_i915_reg_read otherReg = {0x2030, 0};
        const bool read1 = sys_ioctl(fd, drm::DRM_IOCTL_I915_REG_READ, &first) == 0;
        std::sleep(2);
        const bool read2 = sys_ioctl(fd, drm::DRM_IOCTL_I915_REG_READ, &second) == 0;
        out("uapi: RING_TIMESTAMP ");
        out_hex(first.val);
        out(" then ");
        out_hex(second.val);
        out("\n");
        check(read1 && read2 && second.val > first.val && sys_ioctl(fd, drm::DRM_IOCTL_I915_REG_READ, &otherReg) == -22,
              "REG_READ: the render timestamp moves on; other registers EINVAL");
        drm::drm_i915_gem_get_aperture aperture = {};
        check(sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_GET_APERTURE, &aperture) == 0 && aperture.aper_size == (4ull << 30) &&
                  aperture.aper_available_size > 0,
              "GET_APERTURE: the 4 GiB GGTT");

        // Batches through the engine map: index 0 is the copy engine, 1 render, 2 empty.
        const uint32_t target = create(fd, kTargetBytes);
        const uint32_t batch = create(fd, 4096);
        volatile uint32_t* t = map_object(fd, target, drm::I915_MMAP_OFFSET_WB, kTargetBytes);
        volatile uint32_t* b = map_object(fd, batch, drm::I915_MMAP_OFFSET_WB, 4096);
        bool mapOk = t && b && mapped > 0;
        for (uint32_t index = 0; mapOk && index < 2; ++index) {
            write_store(b, kTargetVa48, 0xE0000000u | index);
            t[0] = 0;
            mapOk = execbuffer(fd, batch, target, index, drm::EXEC_OBJECT_PINNED, 0, static_cast<uint32_t>(mapped)) == 0 &&
                    wait_object(fd, target) == 0 && t[0] == (0xE0000000u | index);
        }
        check(mapOk, "execbuffer through the map: slots 0 (copy) and 1 (render) run");
        check(execbuffer(fd, batch, target, 2, drm::EXEC_OBJECT_PINNED, 0, static_cast<uint32_t>(mapped)) == -22 &&
                  execbuffer(fd, batch, target, 3, drm::EXEC_OBJECT_PINNED, 0, static_cast<uint32_t>(mapped)) == -22,
              "the empty slot and one past the map are EINVAL");
        check(set_param(fd, static_cast<uint32_t>(mapped), drm::I915_CONTEXT_PARAM_ENGINES, 0, 0) == -22,
              "the map cannot change once the context has run");

        // A context in another address space: the same addresses, other pages.
        write_store(b, kTargetVa48, 0x5EC0DD01);
        t[0] = 0;
        check(t && b && execbuffer(fd, batch, target, drm::I915_EXEC_RENDER, drm::EXEC_OBJECT_PINNED, 0,
                                   static_cast<uint32_t>(inVm)) == 0 &&
                  wait_object(fd, target) == 0 && t[0] == 0x5EC0DD01,
              "a batch in the context of the second address space");

        // An uncached object: the GPU's store is seen after SET_DOMAIN.
        const uint32_t plain = create(fd, kTargetBytes);
        volatile uint32_t* p = map_object(fd, plain, drm::I915_MMAP_OFFSET_WB, kTargetBytes);
        drm::drm_i915_gem_caching uncached = {plain, drm::I915_CACHING_NONE};
        write_store(b, kTargetVa48, 0x0CAC0001);
        if (p) {
            p[0] = 0;
        }
        drm::drm_i915_gem_set_domain domain = {plain, 1, 0};   // I915_GEM_DOMAIN_CPU read
        check(p && sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_SET_CACHING, &uncached) == 0 &&
                  execbuffer(fd, batch, plain, drm::I915_EXEC_RENDER) == 0 &&
                  sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_SET_DOMAIN, &domain) == 0 && p[0] == 0x0CAC0001,
              "an uncached object: the GPU's store read back after SET_DOMAIN");

        // A single-timeline context: a blitter batch on another object waits
        // for the render engine's spinner before it.
        const int64_t single = context_create_with(fd, nullptr, 0, drm::I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE);
        const uint32_t spin = create(fd, 4096);
        const uint32_t other = create(fd, 4096);
        const uint32_t store = create(fd, 4096);
        volatile uint32_t* spinMap = map_object(fd, spin, drm::I915_MMAP_OFFSET_WB, 4096);
        volatile uint32_t* o = map_object(fd, other, drm::I915_MMAP_OFFSET_WB, 4096);
        volatile uint32_t* storeMap = map_object(fd, store, drm::I915_MMAP_OFFSET_WB, 4096);
        bool timeline = single > 0 && spinMap && o && storeMap;
        if (timeline) {
            write_spinner(spinMap);
            write_store(storeMap, kOtherVa, 0x51C1E001);
            o[0] = 0;
            Obj spinObjs[1] = {{spin, kSpinVa, 0}};
            Obj storeObjs[2] = {{other, kOtherVa, drm::EXEC_OBJECT_WRITE}, {store, kBatchVa + 0x4000, 0}};
            timeline = submit(fd, spinObjs, 1, drm::I915_EXEC_RENDER, static_cast<uint32_t>(single)) == 0 &&
                       submit(fd, storeObjs, 2, drm::I915_EXEC_BLT, static_cast<uint32_t>(single)) == 0;
            std::sleep(30);
            const bool waited = o[0] == 0;
            end_spinner(spinMap);
            timeline = timeline && waited && wait_object(fd, other, 1000ll * 1000 * 1000) == 0 && o[0] == 0x51C1E001;
        }
        check(timeline, "SINGLE_TIMELINE: the blitter batch ran only after the render spinner before it");
    }

    std::close(fd);
    if (gFailures != 0 && !cpuOnly) {
        print_state();
    }
    out(gFailures == 0 ? "uapi: all checks passed\n" : "uapi: some checks FAILED\n");
    return gFailures == 0;
}

// gpu-test sync: step 6d-2, what Mesa's Vulkan runtime (vk_drm_syncobj),
// iris's fences and the window-system code use to pass work between
// queues, processes and the display: timeline sync objects (signal, query,
// wait for a point, its submission or its availability, transfer), sync
// objects as fds, sync_files (export, import, merge, poll, info), dma-buf
// fds (PRIME export and import across two opens of the render node, size,
// mmap, CPU-access sync, implicit fences as sync_files), and execbuffer with
// timeline fences and FENCE_IN / FENCE_OUT. A hang nobody waits for must be
// caught by the timer. `cpu-only` (QEMU) skips the batches.
namespace {

constexpr uint64_t kFileHandle = 1ull << 48;   // an fd slot from a DRM struct, as a handle

uint64_t handle_of(int32_t slot) {
    return kFileHandle | static_cast<uint64_t>(slot);
}

int64_t timeline_signal(uint64_t fd, uint32_t handle, uint64_t point) {
    drm::drm_syncobj_timeline_array request = {reinterpret_cast<uint64_t>(&handle), reinterpret_cast<uint64_t>(&point), 1, 0};
    return sys_ioctl(fd, drm::DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &request);
}

// The point reached (or added, with LAST_SUBMITTED), or -errno.
int64_t timeline_query(uint64_t fd, uint32_t handle, uint32_t flags = 0) {
    uint64_t point = ~0ull;
    drm::drm_syncobj_timeline_array request = {reinterpret_cast<uint64_t>(&handle), reinterpret_cast<uint64_t>(&point), 1, flags};
    const int64_t result = sys_ioctl(fd, drm::DRM_IOCTL_SYNCOBJ_QUERY, &request);
    return result != 0 ? result : static_cast<int64_t>(point);
}

int64_t timeline_wait(uint64_t fd, uint32_t handle, uint64_t point, uint64_t forNs, uint32_t flags = 0) {
    drm::drm_syncobj_timeline_wait request = {};
    request.handles = reinterpret_cast<uint64_t>(&handle);
    request.points = reinterpret_cast<uint64_t>(&point);
    request.count_handles = 1;
    request.flags = flags;
    request.timeout_nsec = forNs == 0 ? 0 : static_cast<int64_t>(now_ns() + forNs);
    return sys_ioctl(fd, drm::DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &request);
}

int64_t transfer(uint64_t fd, uint32_t from, uint64_t fromPoint, uint32_t to, uint64_t toPoint) {
    drm::drm_syncobj_transfer request = {from, to, fromPoint, toPoint, 0, 0};
    return sys_ioctl(fd, drm::DRM_IOCTL_SYNCOBJ_TRANSFER, &request);
}

int64_t syncobj_reset(uint64_t fd, uint32_t handle) {
    drm::drm_syncobj_array request = {reinterpret_cast<uint64_t>(&handle), 1, 0};
    return sys_ioctl(fd, drm::DRM_IOCTL_SYNCOBJ_RESET, &request);
}

// The fd slot, or -errno.
int64_t syncobj_to_fd(uint64_t fd, uint32_t handle, uint32_t flags = 0, uint64_t point = 0) {
    drm::drm_syncobj_handle request = {handle, flags, -1, 0, point};
    const int64_t result = sys_ioctl(fd, drm::DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &request);
    return result != 0 ? result : request.fd;
}

int64_t syncobj_from_fd(uint64_t fd, int32_t slot, uint32_t handle = 0, uint32_t flags = 0, uint64_t point = 0) {
    drm::drm_syncobj_handle request = {handle, flags, slot, 0, point};
    const int64_t result = sys_ioctl(fd, drm::DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &request);
    return result != 0 ? result : request.handle;
}

int64_t prime_to_fd(uint64_t fd, uint32_t handle) {
    drm::drm_prime_handle request = {handle, drm::DRM_CLOEXEC | drm::DRM_RDWR, -1};
    const int64_t result = sys_ioctl(fd, drm::DRM_IOCTL_PRIME_HANDLE_TO_FD, &request);
    return result != 0 ? result : request.fd;
}

int64_t prime_from_fd(uint64_t fd, int32_t slot) {
    drm::drm_prime_handle request = {0, 0, slot};
    const int64_t result = sys_ioctl(fd, drm::DRM_IOCTL_PRIME_FD_TO_HANDLE, &request);
    return result != 0 ? result : request.handle;
}

// poll() on one fd slot for POLLIN|POLLOUT; the revents, or -errno.
int64_t poll_slot(int32_t slot, uint64_t timeoutMs) {
    std::PollFD one = {static_cast<std::int64_t>(handle_of(slot)), 0x1 | 0x4, 0};
    const int64_t result = static_cast<int64_t>(
        _syscall_impl(static_cast<uint64_t>(std::Syscall::Poll), reinterpret_cast<uint64_t>(&one), 1, timeoutMs));
    return result < 0 ? result : one.revents;
}

int32_t sync_status(int32_t slot) {
    drm::sync_file_info info = {};
    return sys_ioctl(handle_of(slot), drm::SYNC_IOC_FILE_INFO, &info) == 0 ? info.status : -1000;
}

uint64_t open_render() {
    return _syscall_impl(static_cast<uint64_t>(std::Syscall::Open), reinterpret_cast<uint64_t>("/dev/dri/renderD128"),
                         O_RDWR | O_CLOEXEC, 0);
}

// One batch with a fence array or timeline fences, through EXECBUFFER2_WR
// (rsvd2 comes back: the FENCE_OUT sync_file's slot in its upper half).
int64_t submit_ext(uint64_t fd, const Obj* objs, uint32_t count, uint64_t ring, uint32_t context,
                   drm::drm_i915_gem_exec_fence* fences, const uint64_t* points, uint32_t fenceCount,
                   uint64_t extraFlags = 0, uint64_t rsvd2 = 0, uint64_t* rsvd2Out = nullptr) {
    drm::drm_i915_gem_exec_object2 list[4] = {};
    for (uint32_t i = 0; i < count; ++i) {
        list[i].handle = objs[i].handle;
        list[i].offset = objs[i].va;
        list[i].flags = drm::EXEC_OBJECT_PINNED | drm::EXEC_OBJECT_SUPPORTS_48B_ADDRESS | objs[i].flags;
    }
    drm::drm_i915_gem_execbuffer_ext_timeline_fences timeline = {};
    timeline.base.name = drm::DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
    timeline.fence_count = fenceCount;
    timeline.handles_ptr = reinterpret_cast<uint64_t>(fences);
    timeline.values_ptr = reinterpret_cast<uint64_t>(points);
    drm::drm_i915_gem_execbuffer2 exec = {};
    exec.buffers_ptr = reinterpret_cast<uint64_t>(list);
    exec.buffer_count = count;
    exec.flags = ring | drm::I915_EXEC_NO_RELOC | drm::I915_EXEC_HANDLE_LUT | extraFlags;
    exec.rsvd1 = context;
    exec.rsvd2 = rsvd2;
    if (fenceCount) {
        exec.flags |= drm::I915_EXEC_USE_EXTENSIONS;
        exec.cliprects_ptr = reinterpret_cast<uint64_t>(&timeline);
    }
    const int64_t result = sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_EXECBUFFER2_WR, &exec);
    if (rsvd2Out) {
        *rsvd2Out = exec.rsvd2;
    }
    return result;
}

} // namespace

bool sync_test(bool cpuOnly) {
    gFailures = 0;
    gName = "sync";
    const uint64_t opened = open_render();
    const uint64_t second = open_render();
    if (static_cast<int64_t>(opened) < 0 || static_cast<int64_t>(second) < 0) {
        out("sync: cannot open /dev/dri/renderD128 twice\n");
        return false;
    }
    const uint64_t fd = opened;

    drm::drm_get_cap timelineCap = {drm::DRM_CAP_SYNCOBJ_TIMELINE, 0};
    drm::drm_get_cap primeCap = {drm::DRM_CAP_PRIME, 0};
    check(sys_ioctl(fd, drm::DRM_IOCTL_GET_CAP, &timelineCap) == 0 && timelineCap.value == 1 &&
              sys_ioctl(fd, drm::DRM_IOCTL_GET_CAP, &primeCap) == 0 && primeCap.value == 3 &&
              getparam(fd, drm::I915_PARAM_HAS_EXEC_TIMELINE_FENCES) == 1 && getparam(fd, drm::I915_PARAM_HAS_EXEC_FENCE) == 1,
          "caps: timeline sync objects, PRIME import and export; HAS_EXEC_TIMELINE_FENCES, HAS_EXEC_FENCE");

    // A timeline on the CPU alone.
    const uint32_t tl = syncobj_create(fd, false);
    check(tl && timeline_query(fd, tl) == 0 && timeline_signal(fd, tl, 3) == 0 && timeline_query(fd, tl) == 3 &&
              timeline_query(fd, tl, drm::DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) == 3,
          "TIMELINE_SIGNAL point 3; QUERY reaches 3");
    check(timeline_wait(fd, tl, 2, 0) == 0 && timeline_wait(fd, tl, 3, 0) == 0 && timeline_wait(fd, tl, 5, 0) == -22 &&
              timeline_wait(fd, tl, 5, 20ull * 1000 * 1000, drm::DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) == -62,
          "TIMELINE_WAIT: 2 and 3 reached, 5 not submitted (EINVAL), WAIT_FOR_SUBMIT runs out (ETIME)");
    check(timeline_signal(fd, tl, 5) == 0 && timeline_query(fd, tl) == 5 &&
              timeline_wait(fd, tl, 4, 0, drm::DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) == 0,
          "point 5 signaled; WAIT_AVAILABLE for 4 is met");
    const uint32_t bin = syncobj_create(fd, false);
    check(bin && transfer(fd, tl, 5, bin, 0) == 0 && syncobj_wait(fd, bin, 0) == 0 && transfer(fd, bin, 0, tl, 7) == 0 &&
              timeline_query(fd, tl) == 7 && transfer(fd, tl, 9, bin, 0) == -22,
          "TRANSFER: point 5 into a binary sync object, that into point 7; a missing point EINVAL");
    check(syncobj_reset(fd, tl) == 0 && timeline_query(fd, tl) == 0, "RESET drops the timeline");

    // Sync objects as fds: one object behind two handles (of two files).
    const int64_t opaque = syncobj_to_fd(fd, tl);
    const int64_t imported = opaque >= 0 ? syncobj_from_fd(second, static_cast<int32_t>(opaque)) : -1;
    check(opaque >= 0 && imported > 0 && timeline_signal(second, static_cast<uint32_t>(imported), 9) == 0 &&
              timeline_query(fd, tl) == 9,
          "HANDLE_TO_FD / FD_TO_HANDLE: another file's handle to the same timeline");
    if (opaque >= 0) {
        std::close(handle_of(static_cast<int32_t>(opaque)));
    }

    // sync_files.
    const uint32_t empty = syncobj_create(fd, false);
    check(syncobj_to_fd(fd, empty, drm::DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE) == -22,
          "EXPORT_SYNC_FILE of a sync object without a fence is EINVAL");
    const int64_t sf = syncobj_to_fd(fd, tl, drm::DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE |
                                                 drm::DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE, 9);
    check(sf >= 0 && poll_slot(static_cast<int32_t>(sf), 0) == 0x1 && sync_status(static_cast<int32_t>(sf)) == 1,
          "EXPORT_SYNC_FILE of point 9: poll readable, FILE_INFO signaled");
    check(sf >= 0 && syncobj_from_fd(fd, static_cast<int32_t>(sf), empty, drm::DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) == empty &&
              syncobj_wait(fd, empty, 0) == 0,
          "IMPORT_SYNC_FILE into a binary sync object");
    drm::sync_merge_data merge = {};
    merge.fd2 = static_cast<int32_t>(sf);
    check(sf >= 0 && sys_ioctl(handle_of(static_cast<int32_t>(sf)), drm::SYNC_IOC_MERGE, &merge) == 0 &&
              sync_status(merge.fence) == 1,
          "SYNC_IOC_MERGE: a signaled sync_file");

    // dma-bufs: export, size, map, import in the other file, CPU sync.
    const uint32_t bo = create(fd, 3 * 4096);
    volatile uint32_t* boMap = map_object(fd, bo, drm::I915_MMAP_OFFSET_WB, 3 * 4096);
    if (boMap) {
        boMap[0] = 0xD3ABF001;
        boMap[1024 * 2 + 5] = 0xD3ABF002;
    }
    const int64_t dmabuf = prime_to_fd(fd, bo);
    const uint64_t dmaHandle = dmabuf >= 0 ? handle_of(static_cast<int32_t>(dmabuf)) : 0;
    const uint64_t size = dmabuf >= 0 ? _syscall_impl(static_cast<uint64_t>(std::Syscall::Seek), dmaHandle, 0, 2) : 0;
    check(boMap && dmabuf >= 0 && size == 3 * 4096, "PRIME_HANDLE_TO_FD: a dma-buf fd; lseek(SEEK_END) is its size");
    volatile uint32_t* viaFd = dmabuf >= 0 ? static_cast<volatile uint32_t*>(map(dmaHandle, 4096, 2 * 4096)) : nullptr;
    check(viaFd && viaFd[1024 + 5] == 0xD3ABF002, "mmap of the dma-buf (from its second page) shows the object");
    const int64_t same = dmabuf >= 0 ? prime_from_fd(fd, static_cast<int32_t>(dmabuf)) : -1;
    const int64_t other = dmabuf >= 0 ? prime_from_fd(second, static_cast<int32_t>(dmabuf)) : -1;
    volatile uint32_t* otherMap = other > 0 ? map_object(second, static_cast<uint32_t>(other), drm::I915_MMAP_OFFSET_WB, 4096) : nullptr;
    check(same == bo && other > 0 && otherMap && otherMap[0] == 0xD3ABF001,
          "PRIME_FD_TO_HANDLE: the same handle in its own file, the same pages in another");
    drm::dma_buf_sync start = {drm::DMA_BUF_SYNC_START | drm::DMA_BUF_SYNC_RW};
    drm::dma_buf_sync end = {drm::DMA_BUF_SYNC_END | drm::DMA_BUF_SYNC_RW};
    drm::dma_buf_sync bad = {0};
    check(dmabuf >= 0 && sys_ioctl(dmaHandle, drm::DMA_BUF_IOCTL_SYNC, &start) == 0 &&
              sys_ioctl(dmaHandle, drm::DMA_BUF_IOCTL_SYNC, &end) == 0 && sys_ioctl(dmaHandle, drm::DMA_BUF_IOCTL_SYNC, &bad) == -22,
          "DMA_BUF_IOCTL_SYNC start and end; no direction EINVAL");
    drm::dma_buf_export_sync_file exported = {drm::DMA_BUF_SYNC_WRITE, -1};
    check(dmabuf >= 0 && sys_ioctl(dmaHandle, drm::DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) == 0 &&
              sync_status(exported.fd) == 1 && poll_slot(static_cast<int32_t>(dmabuf), 0) == (0x1 | 0x4),
          "an idle dma-buf: its fences export as a signaled sync_file; poll readable and writable");

    if (!cpuOnly) {
        const uint32_t spin = create(fd, 4096);
        const uint32_t store = create(fd, 4096);
        const uint32_t target = create(fd, kTargetBytes);
        volatile uint32_t* spinMap = map_object(fd, spin, drm::I915_MMAP_OFFSET_WB, 4096);
        volatile uint32_t* storeMap = map_object(fd, store, drm::I915_MMAP_OFFSET_WB, 4096);
        volatile uint32_t* t = map_object(fd, target, drm::I915_MMAP_OFFSET_WB, kTargetBytes);
        const uint32_t gpuTl = syncobj_create(fd, false);
        bool ready = spinMap && storeMap && t && gpuTl;

        // Timeline fences: render spinner signals point 1, the blitter waits
        // for it and signals point 2.
        if (ready) {
            write_spinner(spinMap);
            write_store(storeMap, kTargetVa48, 0x7177E002);
            t[0] = 0;
            Obj spinObjs[1] = {{spin, kSpinVa, 0}};
            Obj storeObjs[2] = {{target, kTargetVaCanonical, drm::EXEC_OBJECT_WRITE | drm::EXEC_OBJECT_ASYNC}, {store, kBatchVa, 0}};
            drm::drm_i915_gem_exec_fence sig1 = {gpuTl, drm::I915_EXEC_FENCE_SIGNAL};
            uint64_t one = 1;
            drm::drm_i915_gem_exec_fence waitSig[2] = {{gpuTl, drm::I915_EXEC_FENCE_WAIT}, {gpuTl, drm::I915_EXEC_FENCE_SIGNAL}};
            uint64_t points[2] = {1, 2};
            const bool queued = submit_ext(fd, spinObjs, 1, drm::I915_EXEC_RENDER, 0, &sig1, &one, 1) == 0 &&
                                submit_ext(fd, storeObjs, 2, drm::I915_EXEC_BLT, 0, waitSig, points, 2) == 0;
            check(queued && timeline_query(fd, gpuTl, drm::DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) == 2 &&
                      timeline_query(fd, gpuTl) == 0 && timeline_wait(fd, gpuTl, 1, 20ull * 1000 * 1000) == -62,
                  "execbuffer timeline fences: points 1 and 2 submitted, none reached while the spinner runs");
            std::sleep(20);
            const bool held = t[0] == 0;
            end_spinner(spinMap);
            check(held && timeline_wait(fd, gpuTl, 2, 1000ull * 1000 * 1000) == 0 && timeline_query(fd, gpuTl) == 2 &&
                      t[0] == 0x7177E002,
                  "the blitter batch waited for point 1, then reached point 2");
        }

        // FENCE_OUT and FENCE_IN: a sync_file of the spinner gates a blitter batch.
        if (ready) {
            write_spinner(spinMap);
            write_store(storeMap, kTargetVa48, 0xF0E1CE01);
            t[0] = 0;
            Obj spinObjs[1] = {{spin, kSpinVa, 0}};
            Obj storeObjs[2] = {{target, kTargetVaCanonical, drm::EXEC_OBJECT_WRITE | drm::EXEC_OBJECT_ASYNC}, {store, kBatchVa, 0}};
            uint64_t rsvd2 = 0;
            const int64_t q = submit_ext(fd, spinObjs, 1, drm::I915_EXEC_RENDER, 0, nullptr, nullptr, 0,
                                         drm::I915_EXEC_FENCE_OUT, 0, &rsvd2);
            const int32_t outFd = static_cast<int32_t>(rsvd2 >> 32);
            const int64_t q2 = q == 0 ? submit_ext(fd, storeObjs, 2, drm::I915_EXEC_BLT, 0, nullptr, nullptr, 0,
                                                   drm::I915_EXEC_FENCE_IN, static_cast<uint32_t>(outFd))
                                      : q;
            const bool pending = q == 0 && poll_slot(outFd, 0) == 0 && sync_status(outFd) == 0;
            std::sleep(20);
            const bool held = t[0] == 0;
            end_spinner(spinMap);
            const int64_t polled = q == 0 ? poll_slot(outFd, 1000) : -1;
            check(q == 0 && q2 == 0 && pending && held, "FENCE_OUT: a sync_file of the spinner, not signaled; FENCE_IN holds a batch on it");
            check(polled == 0x1 && sync_status(outFd) == 1 && wait_object(fd, target, 1000ll * 1000 * 1000) == 0 &&
                      t[0] == 0xF0E1CE01,
                  "poll wakes when the spinner ends; the gated batch ran");
            Obj plain[1] = {{spin, kSpinVa, 0}};
            check(submit(fd, plain, 1, drm::I915_EXEC_RENDER) == 0 &&
                      [&] {
                          drm::drm_i915_gem_execbuffer2 exec = {};
                          drm::drm_i915_gem_exec_object2 o = {};
                          o.handle = spin;
                          o.offset = kSpinVa;
                          o.flags = drm::EXEC_OBJECT_PINNED;
                          exec.buffers_ptr = reinterpret_cast<uint64_t>(&o);
                          exec.buffer_count = 1;
                          exec.flags = drm::I915_EXEC_RENDER | drm::I915_EXEC_FENCE_OUT;
                          return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec) == -22;
                      }(),
                  "FENCE_OUT through plain EXECBUFFER2 (nothing comes back) is EINVAL");
            wait_object(fd, spin);
        }

        // Implicit fences across files: the other file's blitter write to
        // the shared object waits for this file's spinner writing it.
        if (ready && other > 0) {
            write_spinner(spinMap);
            const uint32_t store2 = create(second, 4096);
            volatile uint32_t* store2Map = map_object(second, store2, drm::I915_MMAP_OFFSET_WB, 4096);
            if (store2Map && otherMap) {
                write_store(store2Map, 0x40000, 0x5A4ED001);
                otherMap[0] = 0;
            }
            Obj spinObjs[2] = {{bo, 0x40000, drm::EXEC_OBJECT_WRITE}, {spin, kSpinVa, 0}};
            Obj otherObjs[2] = {{static_cast<uint32_t>(other), 0x40000, drm::EXEC_OBJECT_WRITE}, {store2, kBatchVa, 0}};
            const bool queued = store2Map && submit(fd, spinObjs, 2, drm::I915_EXEC_RENDER) == 0 &&
                                submit(second, otherObjs, 2, drm::I915_EXEC_BLT) == 0;
            drm::dma_buf_export_sync_file busyFences = {drm::DMA_BUF_SYNC_WRITE, -1};
            const bool exportedBusy = sys_ioctl(dmaHandle, drm::DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &busyFences) == 0 &&
                                      sync_status(busyFences.fd) == 0 && (poll_slot(static_cast<int32_t>(dmabuf), 0) & 0x4) == 0;
            std::sleep(20);
            const bool held = otherMap && otherMap[0] == 0;
            end_spinner(spinMap);
            check(queued && exportedBusy && held, "a shared object busy in one file: its sync_file pending, the dma-buf not writable, the other file's write waits");
            check(wait_object(second, static_cast<uint32_t>(other), 1000ll * 1000 * 1000) == 0 && otherMap &&
                      otherMap[0] == 0x5A4ED001 && sync_status(busyFences.fd) == 1,
                  "it ran after the spinner; the exported fences signaled");
        }

        // A hang with nobody waiting: the timer catches it.
        if (ready) {
            const uint32_t ctx = context_create(fd);
            set_param(fd, ctx, drm::I915_CONTEXT_PARAM_RECOVERABLE, 0);
            write_spinner(spinMap);
            Obj spinObjs[1] = {{spin, kSpinVa, 0}};
            const bool queued = submit(fd, spinObjs, 1, drm::I915_EXEC_RENDER, ctx) == 0;
            std::sleep(3000);   // no ioctl meanwhile
            drm::drm_i915_reset_stats st = {};
            st.ctx_id = ctx;
            const bool caught = sys_ioctl(fd, drm::DRM_IOCTL_I915_GET_RESET_STATS, &st) == 0 && st.batch_active == 1;
            end_spinner(spinMap);
            check(queued && caught, "a hang nobody waits for is caught by the timer within 3 s");
        }
    }

    std::close(second);
    std::close(fd);
    if (gFailures != 0 && !cpuOnly) {
        print_state();
    }
    out(gFailures == 0 ? "sync: all checks passed\n" : "sync: some checks FAILED\n");
    return gFailures == 0;
}

// gpu-test dev: step 6d-3, finding the GPU the way libdrm does (xf86drm.c's
// drmGetDevice2 and drmGetDevices2, which Mesa's loader and intel_device_info
// call): /dev/dri lists renderD128; its fstat and stat are char 226:128;
// /sys/dev/char/226:128/device/drm exists; the realpath of .../subsystem ends
// in "pci"; uevent's PCI_SLOT_NAME; the vendor, device, subsystem and
// revision files, and the config space, agree with what the node reports.
// `cpu-only` (QEMU) checks the layout only (no device, ids zero).
namespace {

void strcpy_n(char* out, const char* in, uint64_t room) {
    uint64_t i = 0;
    for (; i + 1 < room && in[i]; ++i) {
        out[i] = in[i];
    }
    out[i] = 0;
}

const char* find_text(const char* haystack, const char* needle) {
    const uint64_t n = std::strlen(needle);
    for (; *haystack; ++haystack) {
        if (std::strncmp(haystack, needle, n) == 0) {
            return haystack;
        }
    }
    return nullptr;
}

// The text of a small file, or an empty string.
uint64_t read_text(const char* path, char* out, uint64_t room) {
    const uint64_t handle = std::open(path, 0);
    if (static_cast<int64_t>(handle) < 0 || handle == ~0ull) {
        out[0] = '\0';
        return 0;
    }
    const uint64_t got = std::read(handle, out, room - 1);
    std::close(handle);
    const uint64_t n = static_cast<int64_t>(got) < 0 || got >= room ? 0 : got;
    out[n] = '\0';
    return n;
}

uint64_t parse_hex(const char* text) {
    uint64_t value = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    for (; *text; ++text) {
        const char c = *text;
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) {
            break;
        }
        value = value * 16 + static_cast<uint64_t>(digit);
    }
    return value;
}

int64_t readlink_of(const char* path, char* out, uint64_t room) {
    const int64_t n = static_cast<int64_t>(_syscall_impl(static_cast<uint64_t>(std::Syscall::Readlink),
                                                         reinterpret_cast<uint64_t>(path), reinterpret_cast<uint64_t>(out), room - 1));
    out[n > 0 ? n : 0] = '\0';
    return n;
}

// realpath's last component (what libdrm's drmParseSubsystemType takes),
// following links by hand as a libc's realpath does.
void real_basename(const char* path, char* out, uint64_t room) {
    char current[256];
    strcpy_n(current, path, sizeof(current));
    for (int hops = 0; hops < 8; ++hops) {
        char target[256];
        if (readlink_of(current, target, sizeof(target)) <= 0) {
            break;   // not a link: the path itself
        }
        strcpy_n(current, target, sizeof(current));   // the links are absolute
    }
    const char* last = current;
    for (const char* p = current; *p; ++p) {
        if (*p == '/') {
            last = p + 1;
        }
    }
    strcpy_n(out, last, room);
}

} // namespace

bool dev_test(bool cpuOnly) {
    gFailures = 0;
    gName = "dev";

    std::DirEntry entries[8] = {};
    const uint64_t listed = std::readdir("/dev/dri", entries, 8);
    bool hasRender = false;
    for (uint64_t i = 0; i < listed && i < 8; ++i) {
        hasRender = hasRender || std::strcmp(entries[i].name, "renderD128") == 0;
    }
    check(static_cast<int64_t>(listed) > 0 && hasRender, "/dev/dri lists renderD128 (drmGetDevices2's scan)");
    std::Stat st = {};
    const bool statted = std::stat("/dev/dri/renderD128", &st) == 0;
    check(statted && (st.st_mode & 0170000) == 0020000 && st.st_rdev == ((226u << 8) | 128u),
          "stat: a character device, 226:128");

    const uint64_t fd = _syscall_impl(static_cast<uint64_t>(std::Syscall::Open), reinterpret_cast<uint64_t>("/dev/dri/renderD128"),
                                      O_RDWR | O_CLOEXEC, 0);
    std::Stat fst = {};
    const bool fstatted = static_cast<int64_t>(fd) >= 0 &&
                          static_cast<int64_t>(_syscall_impl(static_cast<uint64_t>(std::Syscall::Fstat), fd,
                                                             reinterpret_cast<uint64_t>(&fst))) == 0;
    check(fstatted && fst.st_rdev == ((226u << 8) | 128u) && (fst.st_mode & 0170000) == 0020000,
          "fstat of the open node: 226:128 (drmGetNodeTypeFromFd)");

    std::Stat drmDir = {};
    check(std::stat("/sys/dev/char/226:128/device/drm", &drmDir) == 0 && (drmDir.st_mode & 0170000) == 0040000,
          "/sys/dev/char/226:128/device/drm is a directory (drmNodeIsDRM)");
    char base[64];
    real_basename("/sys/dev/char/226:128/device/subsystem", base, sizeof(base));
    check(std::strcmp(base, "pci") == 0, "realpath of .../device/subsystem ends in pci (drmParseSubsystemType)");
    char uevent[256];
    read_text("/sys/dev/char/226:128/device/uevent", uevent, sizeof(uevent));
    const char* slot = find_text(uevent, "PCI_SLOT_NAME=");
    out("dev: uevent ");
    out(uevent);
    check(slot && std::strncmp(slot + 14, "0000:00:02.0", 12) == 0, "uevent: PCI_SLOT_NAME=0000:00:02.0 (drmParsePciBusInfo)");

    char text[32];
    read_text("/sys/dev/char/226:128/device/vendor", text, sizeof(text));
    const uint64_t vendor = parse_hex(text);
    read_text("/sys/dev/char/226:128/device/device", text, sizeof(text));
    const uint64_t device = parse_hex(text);
    read_text("/sys/dev/char/226:128/device/subsystem_vendor", text, sizeof(text));
    const uint64_t subVendor = parse_hex(text);
    read_text("/sys/dev/char/226:128/device/subsystem_device", text, sizeof(text));
    const uint64_t subDevice = parse_hex(text);
    read_text("/sys/dev/char/226:128/device/revision", text, sizeof(text));
    const uint64_t revision = parse_hex(text);
    uint8_t config[64] = {};
    const uint64_t cfg = std::open("/sys/dev/char/226:128/device/config", 0);
    const uint64_t cfgBytes = static_cast<int64_t>(cfg) >= 0 && cfg != ~0ull ? std::read(cfg, config, sizeof(config)) : 0;
    if (static_cast<int64_t>(cfg) >= 0 && cfg != ~0ull) {
        std::close(cfg);
    }
    out("dev: vendor ");
    out_hex(vendor);
    out(", device ");
    out_hex(device);
    out(", subsystem ");
    out_hex(subVendor);
    out(":");
    out_hex(subDevice);
    out(", revision ");
    out_hex(revision);
    out("\n");
    const uint64_t cfgVendor = config[0] | (config[1] << 8);
    const uint64_t cfgDevice = config[2] | (config[3] << 8);
    check(cfgBytes == 64 && cfgVendor == vendor && cfgDevice == device && config[8] == revision &&
              (config[44] | (config[45] << 8)) == subVendor,
          "config: its first 64 bytes agree with the id files (drmParsePciDeviceInfo's fallback)");
    char renderDev[16];
    read_text("/sys/class/drm/renderD128/dev", renderDev, sizeof(renderDev));
    check(std::strcmp(renderDev, "226:128\n") == 0, "/sys/class/drm/renderD128/dev is 226:128");
    std::Stat nodeDir = {};
    check(std::stat("/sys/dev/char/226:128/device/drm/renderD128", &nodeDir) == 0,
          "the device's drm/renderD128 (drmGetRenderDeviceNameFromFd's lookup)");

    if (!cpuOnly) {
        check(vendor == 0x8086, "vendor 0x8086 (Intel)");
        check(static_cast<int64_t>(device) == getparam(fd, drm::I915_PARAM_CHIPSET_ID) &&
                  static_cast<int64_t>(revision) == getparam(fd, drm::I915_PARAM_REVISION),
              "device and revision match GETPARAM CHIPSET_ID and REVISION");
    }
    if (static_cast<int64_t>(fd) >= 0) {
        std::close(fd);
    }
    out(gFailures == 0 ? "dev: all checks passed\n" : "dev: some checks FAILED\n");
    return gFailures == 0;
}

// gpu-test window [frames] [cpu-only]: a compositor window whose pixels are
// GPU buffers (step 7c-2a). Two GEM objects, exported as dma-bufs, become
// surfaces (SurfaceFromBuffer); each frame draws into the one the compositor
// let go (SurfaceWaitIdle), attaches and commits it. The blitter draws (an
// XY_COLOR_BLT background and a bar that moves); `cpu-only` (QEMU) draws
// with the CPU instead. The last frame stays up for 3 s
// ("window: holding" first), so a check can look at the screen.
namespace {

constexpr uint32_t kWinWidth = 640;
constexpr uint32_t kWinHeight = 400;
constexpr uint32_t kWinPitch = kWinWidth * 4;
constexpr uint32_t kBarWidth = 48;
constexpr uint64_t kWinTargetVa = 0x100000000ull;

uint32_t frame_colour(uint64_t frame) {
    return (frame & 1) ? 0xFF20A060u : 0xFF2060A0u;   // ARGB: green, blue
}

uint32_t bar_x(uint64_t frame) {
    return static_cast<uint32_t>((frame * 16) % (kWinWidth - kBarWidth));
}

// XY_COLOR_BLT (Gen8+: 7 dwords, a 64-bit address): fill a rectangle of a
// linear 32 bpp image.
uint32_t emit_fill(volatile uint32_t* batch, uint32_t n, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2,
                   uint32_t colour) {
    batch[n++] = (2u << 29) | (0x50u << 22) | (1u << 21) | (1u << 20) | 5;
    batch[n++] = (3u << 24) | (0xF0u << 16) | kWinPitch;
    batch[n++] = (y1 << 16) | x1;
    batch[n++] = (y2 << 16) | x2;
    batch[n++] = static_cast<uint32_t>(kWinTargetVa);
    batch[n++] = static_cast<uint32_t>(kWinTargetVa >> 32);
    batch[n++] = colour;
    return n;
}

int64_t blit_frame(uint64_t fd, uint32_t batch, volatile uint32_t* batchMap, uint32_t target, uint64_t frame) {
    uint32_t n = 0;
    n = emit_fill(batchMap, n, 0, 0, kWinWidth, kWinHeight, frame_colour(frame));
    const uint32_t x = bar_x(frame);
    n = emit_fill(batchMap, n, x, 0, x + kBarWidth, kWinHeight, 0xFFFFFFFFu);
    batchMap[n++] = kBatchEnd;
    batchMap[n++] = 0;
    drm::drm_i915_gem_exec_object2 objects[2] = {};
    objects[0].handle = target;
    objects[0].offset = kWinTargetVa;
    objects[0].flags = drm::EXEC_OBJECT_PINNED | drm::EXEC_OBJECT_WRITE | drm::EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
    objects[1].handle = batch;
    objects[1].offset = kBatchVa;
    objects[1].flags = drm::EXEC_OBJECT_PINNED | drm::EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
    drm::drm_i915_gem_execbuffer2 exec = {};
    exec.buffers_ptr = reinterpret_cast<uint64_t>(objects);
    exec.buffer_count = 2;
    exec.batch_len = n * 4;
    exec.flags = drm::I915_EXEC_BLT | drm::I915_EXEC_NO_RELOC | drm::I915_EXEC_HANDLE_LUT;
    return sys_ioctl(fd, drm::DRM_IOCTL_I915_GEM_EXECBUFFER2, &exec);
}

void cpu_frame(volatile uint32_t* pixels, uint64_t frame) {
    const uint32_t colour = frame_colour(frame);
    const uint32_t x = bar_x(frame);
    for (uint32_t row = 0; row < kWinHeight; ++row) {
        volatile uint32_t* line = pixels + row * (kWinPitch / 4);
        for (uint32_t col = 0; col < kWinWidth; ++col) {
            line[col] = (col >= x && col < x + kBarWidth) ? 0xFFFFFFFFu : colour;
        }
    }
}

} // namespace

bool window_test(uint64_t frames, bool cpuOnly) {
    gFailures = 0;
    gName = "window";
    if (frames < 2) {
        frames = 120;
    }
    if (cpuOnly) {
        char unused[16];
        std::gpu_test(std::GpuTestCommand::DrmWithoutGpu, unused, sizeof(unused));
    }
    const uint64_t fd = open_render();
    if (static_cast<int64_t>(fd) < 0) {
        out_errno("window: cannot open /dev/dri/renderD128,", static_cast<int64_t>(fd));
        out("\n");
        gem_flush();
        return false;
    }
    const uint64_t size = (static_cast<uint64_t>(kWinPitch) * kWinHeight + 4095) & ~4095ull;
    uint32_t objects[2] = {create(fd, size), create(fd, size)};
    check(objects[0] != 0 && objects[1] != 0, "two GEM objects for the window's buffers");
    volatile uint32_t* pixels[2] = {map_object(fd, objects[0], drm::I915_MMAP_OFFSET_WB, size),
                                    map_object(fd, objects[1], drm::I915_MMAP_OFFSET_WB, size)};
    check(pixels[0] && pixels[1], "both mapped for the CPU");
    int64_t slots[2] = {prime_to_fd(fd, objects[0]), prime_to_fd(fd, objects[1])};
    check(slots[0] >= 0 && slots[1] >= 0, "both exported as dma-bufs (PRIME)");
    if (gFailures != 0) {
        gem_flush();
        return false;
    }

    // What a bad call gets.
    check(static_cast<int64_t>(std::surface_from_buffer(handle_of(static_cast<int32_t>(slots[0])), kWinWidth,
                                                        kWinHeight, kWinWidth * 2)) < 0,
          "a pitch below width * 4 is refused");
    check(static_cast<int64_t>(std::surface_from_buffer(handle_of(static_cast<int32_t>(slots[0])), kWinWidth,
                                                        kWinHeight * 4, kWinPitch)) < 0,
          "an image bigger than the buffer is refused");
    check(static_cast<int64_t>(std::surface_from_buffer(fd, kWinWidth, kWinHeight, kWinPitch)) < 0,
          "the render node itself is not a buffer");

    std::Handle surfaces[2];
    for (int i = 0; i < 2; ++i) {
        surfaces[i] = std::surface_from_buffer(handle_of(static_cast<int32_t>(slots[i])), kWinWidth, kWinHeight,
                                               kWinPitch);
    }
    check(static_cast<int64_t>(surfaces[0]) >= 0 && static_cast<int64_t>(surfaces[1]) >= 0,
          "SurfaceFromBuffer: two surfaces on the dma-bufs");
    check(std::surface_wait_idle(surfaces[0], 0) == 0, "a surface no window shows is idle");
    const std::Handle compositor = std::service_connect("graphics.compositor");
    const std::Handle window = static_cast<int64_t>(compositor) >= 0
        ? std::compositor_create_window(compositor, kWinWidth, kWinHeight)
        : static_cast<std::Handle>(-1);
    check(static_cast<int64_t>(window) >= 0, "a compositor window");
    if (gFailures != 0) {
        gem_flush();
        return false;
    }
    std::window_set_title(window, "GPU window");

    uint32_t batch = 0;
    volatile uint32_t* batchMap = nullptr;
    if (!cpuOnly) {
        batch = create(fd, 4096);
        batchMap = map_object(fd, batch, drm::I915_MMAP_OFFSET_WB, 4096);
        check(batch != 0 && batchMap, "a batch for the blitter");
    }
    bool waitsOk = true;
    bool drawsOk = true;
    bool busyWhileShown = true;
    uint64_t waitNs = 0;
    const uint64_t start = now_ns();
    for (uint64_t frame = 0; frame < frames; ++frame) {
        const int b = static_cast<int>(frame & 1);
        if (frame >= 2) {
            const uint64_t t0 = now_ns();
            const int64_t idle = static_cast<int64_t>(std::surface_wait_idle(surfaces[b], 2000));
            waitNs += now_ns() - t0;
            if (idle != 0) {
                out_errno("window: SurfaceWaitIdle on the buffer to draw", idle);
                out("\n");
                waitsOk = false;
                break;
            }
        }
        if (cpuOnly) {
            cpu_frame(pixels[b], frame);
        } else {
            const int64_t result = blit_frame(fd, batch, batchMap, objects[b], frame);
            if (result != 0) {
                out_errno("window: execbuffer", result);
                out("\n");
                drawsOk = false;
                break;
            }
        }
        std::window_attach_surface(window, surfaces[b]);
        std::surface_commit(surfaces[b], 0, 0, kWinWidth, kWinHeight);   // waits for the blitter's writes
        if (frame == 1 && std::surface_wait_idle(surfaces[b], 0) == 0) {
            busyWhileShown = false;   // the shown one must not count as idle
        }
    }
    const uint64_t elapsedNs = now_ns() - start;
    check(waitsOk, "every buffer came back (SurfaceWaitIdle) before it was drawn again");
    check(drawsOk, cpuOnly ? "every frame drawn by the CPU" : "every frame drawn by the blitter");
    check(busyWhileShown, "the surface a window shows is not idle");
    const uint64_t last = frames - 1;
    const volatile uint32_t* shown = pixels[last & 1];
    const uint32_t x = bar_x(last);
    const uint32_t mid = kWinHeight / 2 * (kWinPitch / 4);
    check(shown[mid + x + kBarWidth / 2] == 0xFFFFFFFFu &&
              shown[mid + (x + kBarWidth + 8) % kWinWidth] == frame_colour(last),
          "the last frame's pixels are in its buffer after the commit (bar and background)");
    out("window: ");
    out_dec(frames);
    out(" frames in ");
    out_dec(elapsedNs / 1000000);
    out(" ms (");
    out_dec(elapsedNs ? frames * 1000000000ull / elapsedNs : 0);
    out(" per second), ");
    out_dec(waitNs / 1000000);
    out(" ms waiting for buffers; last bar at x=");
    out_dec(x);
    out("\n");
    out("window: holding\n");
    gem_flush();
    std::sleep(3000);

    std::close(window);
    std::close(surfaces[0]);
    std::close(surfaces[1]);
    if (pixels[0]) {
        unmap(const_cast<uint32_t*>(pixels[0]), size);
    }
    if (pixels[1]) {
        unmap(const_cast<uint32_t*>(pixels[1]), size);
    }
    std::close(fd);
    out(gFailures == 0 ? "window: all checks passed\n" : "window: some checks FAILED\n");
    gem_flush();
    return gFailures == 0;
}
