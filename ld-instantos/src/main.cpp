#include <common/elf.hpp>
#include <stdint.h>

extern "C" uint64_t dl_syscall(uint64_t number, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3, uint64_t arg4, uint64_t arg5);

namespace {
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

constexpr u64 PAGE_SIZE = 4096;
constexpr u64 FAIL = static_cast<u64>(-1);
constexpr u64 LIBRARY_BASE = 0x0000580000000000ULL;
constexpr u64 LIBRARY_STRIDE = 0x0000000004000000ULL;
constexpr u32 MAX_OBJECTS = 16;
constexpr u32 MAX_SEGMENTS = 16;
constexpr u32 MAX_NEEDED = 16;

enum Syscall : u64 {
    SysExit = 2,
    SysWrite = 3,
    SysRead = 4,
    SysOpen = 5,
    SysClose = 6,
    SysMmap = 12,
    SysStat = 35,
    SysMprotect = 89,
};

enum Prot : u64 {
    ProtRead = 1ULL << 0,
    ProtWrite = 1ULL << 1,
    ProtExecute = 1ULL << 2,
};

struct Stat {
    u64 st_dev;
    u64 st_ino;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    u64 st_size;
    u64 st_blksize;
    u64 st_blocks;
    u64 st_atime;
    u64 st_mtime;
    u64 st_ctime;
};

struct Segment {
    u64 address;
    u64 size;
    u64 protection;
};

struct Object {
    char name[64];
    u64 base;
    u64 entry;
    Elf::ProgramHeader64* phdr;
    u64 phnum;
    Elf::DynamicEntry64* dynamic;
    const char* strtab;
    Elf::Symbol64* symtab;
    u32 symbolCount;
    Elf::Rela64* rela;
    u64 relaCount;
    Elf::Rela64* jmprel;
    u64 jmprelCount;
    void (**initArray)();
    u64 initArrayCount;
    void (*init)();
    const char* needed[MAX_NEEDED];
    u32 neededCount;
    Segment segments[MAX_SEGMENTS];
    u32 segmentCount;
    bool relocated;
    bool initDone;
};

Object objects[MAX_OBJECTS];
u32 objectCount = 0;
u64 nextLibraryBase = LIBRARY_BASE;

u64 alignDown(u64 value, u64 alignment) {
    return value & ~(alignment - 1);
}

u64 alignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

u64 cstrLen(const char* value) {
    u64 len = 0;
    if (!value) {
        return 0;
    }
    while (value[len]) {
        len++;
    }
    return len;
}

bool cstrEqual(const char* a, const char* b) {
    if (!a || !b) {
        return false;
    }
    u64 i = 0;
    while (a[i] || b[i]) {
        if (a[i] != b[i]) {
            return false;
        }
        i++;
    }
    return true;
}

void copyBytes(void* dst, const void* src, u64 size) {
    auto* out = static_cast<u8*>(dst);
    const auto* in = static_cast<const u8*>(src);
    for (u64 i = 0; i < size; i++) {
        out[i] = in[i];
    }
}

void copyString(char* dst, u64 capacity, const char* src) {
    if (!dst || capacity == 0) {
        return;
    }
    u64 i = 0;
    while (src && src[i] && i + 1 < capacity) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void appendString(char* dst, u64 capacity, const char* src) {
    u64 pos = cstrLen(dst);
    u64 i = 0;
    while (src && src[i] && pos + 1 < capacity) {
        dst[pos++] = src[i++];
    }
    dst[pos] = '\0';
}

void writeText(const char* text) {
    if (!text) {
        return;
    }
    dl_syscall(SysWrite, 2, reinterpret_cast<u64>(text), cstrLen(text), 0, 0);
}

[[noreturn]] void fatal(const char* message) {
    writeText("[ld-instantos] ");
    writeText(message);
    writeText("\n");
    dl_syscall(SysExit, 127, 0, 0, 0, 0);
    for (;;) {
        asm volatile("");
    }
}

u64 protectionFromElf(u32 flags) {
    u64 prot = 0;
    if (flags & Elf::FlagRead) prot |= ProtRead;
    if (flags & Elf::FlagWrite) prot |= ProtWrite;
    if (flags & Elf::FlagExecute) prot |= ProtExecute;
    return prot;
}

bool validateElf(const void* data, u64 size, const Elf::Header64** outHeader) {
    if (!data || size < sizeof(Elf::Header64)) {
        return false;
    }
    const auto* header = static_cast<const Elf::Header64*>(data);
    if (header->ident[0] != Elf::Magic0 ||
        header->ident[1] != Elf::Magic1 ||
        header->ident[2] != Elf::Magic2 ||
        header->ident[3] != Elf::Magic3 ||
        header->ident[4] != Elf::Class64 ||
        header->ident[5] != Elf::DataLittleEndian ||
        header->ident[6] != Elf::VersionCurrent ||
        header->machine != Elf::MachineX86_64 ||
        header->programHeaderEntrySize != sizeof(Elf::ProgramHeader64) ||
        header->programHeaderCount == 0) {
        return false;
    }
    if (header->type != Elf::TypeExecutable && header->type != Elf::TypeDynamic) {
        return false;
    }
    const u64 phEnd = header->programHeaderOffset +
        static_cast<u64>(header->programHeaderEntrySize) * header->programHeaderCount;
    if (phEnd > size || phEnd < header->programHeaderOffset) {
        return false;
    }
    *outHeader = header;
    return true;
}

void* mapMemory(u64 address, u64 size, u64 prot) {
    u64 result = dl_syscall(SysMmap, address, size, prot, 0, 0);
    if (result == FAIL) {
        fatal("mmap failed");
    }
    return reinterpret_cast<void*>(result);
}

void protectMemory(u64 address, u64 size, u64 prot) {
    if (dl_syscall(SysMprotect, address, size, prot, 0, 0) == FAIL) {
        fatal("mprotect failed");
    }
}

void* readWholeFile(const char* path, u64* outSize) {
    Stat stat {};
    if (dl_syscall(SysStat, reinterpret_cast<u64>(path), reinterpret_cast<u64>(&stat), 0, 0, 0) == FAIL ||
        stat.st_size == 0) {
        fatal("stat failed");
    }

    void* buffer = mapMemory(0, alignUp(stat.st_size, PAGE_SIZE), ProtRead | ProtWrite);

    u64 handle = dl_syscall(SysOpen, reinterpret_cast<u64>(path), 0, 0, 0, 0);
    if (handle == FAIL) {
        fatal("open failed");
    }

    u64 read = dl_syscall(SysRead, handle, reinterpret_cast<u64>(buffer), stat.st_size, 0, 0);
    dl_syscall(SysClose, handle, 0, 0, 0, 0);
    if (read != stat.st_size) {
        fatal("read failed");
    }

    *outSize = stat.st_size;
    return buffer;
}

Object* findObject(const char* name) {
    for (u32 i = 0; i < objectCount; i++) {
        if (cstrEqual(objects[i].name, name)) {
            return &objects[i];
        }
    }
    return nullptr;
}

void parseDynamic(Object& object) {
    object.strtab = nullptr;
    object.symtab = nullptr;
    object.symbolCount = 0;
    object.rela = nullptr;
    object.relaCount = 0;
    object.jmprel = nullptr;
    object.jmprelCount = 0;
    object.initArray = nullptr;
    object.initArrayCount = 0;
    object.init = nullptr;
    object.neededCount = 0;

    if (!object.dynamic) {
        return;
    }

    u64 relaSize = 0;
    u64 relaEnt = sizeof(Elf::Rela64);
    u64 jmpRelSize = 0;
    u64 initArraySize = 0;
    u32* sysvHash = nullptr;

    for (Elf::DynamicEntry64* dyn = object.dynamic; dyn->tag != Elf::DynamicNull; ++dyn) {
        switch (static_cast<u64>(dyn->tag)) {
            case Elf::DynamicStrTab:
                object.strtab = reinterpret_cast<const char*>(object.base + dyn->pointer);
                break;
            case Elf::DynamicSymTab:
                object.symtab = reinterpret_cast<Elf::Symbol64*>(object.base + dyn->pointer);
                break;
            case Elf::DynamicHash:
                sysvHash = reinterpret_cast<u32*>(object.base + dyn->pointer);
                break;
            case Elf::DynamicRela:
                object.rela = reinterpret_cast<Elf::Rela64*>(object.base + dyn->pointer);
                break;
            case Elf::DynamicRelaSize:
                relaSize = dyn->value;
                break;
            case Elf::DynamicRelaEnt:
                relaEnt = dyn->value;
                break;
            case Elf::DynamicJmpRel:
                object.jmprel = reinterpret_cast<Elf::Rela64*>(object.base + dyn->pointer);
                break;
            case Elf::DynamicPltRelSize:
                jmpRelSize = dyn->value;
                break;
            case Elf::DynamicInitArray:
                object.initArray = reinterpret_cast<void (**)()>(object.base + dyn->pointer);
                break;
            case Elf::DynamicInitArraySize:
                initArraySize = dyn->value;
                break;
            case Elf::DynamicInit:
                object.init = reinterpret_cast<void (*)()>(object.base + dyn->pointer);
                break;
            case Elf::DynamicRel:
            case Elf::DynamicRelSize:
            case Elf::DynamicRelEnt:
                fatal("REL relocations are unsupported");
            default:
                break;
        }
    }

    if (object.dynamic && (!object.strtab || !object.symtab || !sysvHash)) {
        fatal("object is missing dynamic tables");
    }

    if (sysvHash) {
        object.symbolCount = sysvHash[1];
    }
    if (relaEnt != sizeof(Elf::Rela64)) {
        fatal("unexpected RELA entry size");
    }
    object.relaCount = relaSize / sizeof(Elf::Rela64);
    object.jmprelCount = jmpRelSize / sizeof(Elf::Rela64);
    object.initArrayCount = initArraySize / sizeof(void (*)());

    for (Elf::DynamicEntry64* dyn = object.dynamic; dyn && dyn->tag != Elf::DynamicNull; ++dyn) {
        if (static_cast<u64>(dyn->tag) == Elf::DynamicNeeded) {
            if (object.neededCount >= MAX_NEEDED) {
                fatal("too many dependencies");
            }
            object.needed[object.neededCount++] = object.strtab + dyn->value;
        }
    }
}

Object* registerObject(const char* name) {
    if (objectCount >= MAX_OBJECTS) {
        fatal("too many loaded objects");
    }
    Object* object = &objects[objectCount++];
    *object = {};
    copyString(object->name, sizeof(object->name), name);
    return object;
}

Object* loadObjectFromMemory(const char* name, const void* data, u64 size, u64 preferredBase) {
    const Elf::Header64* header = nullptr;
    if (!validateElf(data, size, &header)) {
        fatal("invalid ELF dependency");
    }

    const auto* phdrs = reinterpret_cast<const Elf::ProgramHeader64*>(
        static_cast<const u8*>(data) + header->programHeaderOffset);

    u64 loadBase = header->type == Elf::TypeDynamic ? preferredBase : 0;
    u64 maxAddress = loadBase;
    Object* object = registerObject(name);
    object->base = loadBase;
    object->entry = loadBase + header->entry;
    object->phnum = header->programHeaderCount;

    for (u16 i = 0; i < header->programHeaderCount; ++i) {
        const Elf::ProgramHeader64& ph = phdrs[i];
        if (ph.type != Elf::ProgramLoad) {
            continue;
        }
        if (ph.memorySize < ph.fileSize || ph.offset + ph.fileSize > size) {
            fatal("bad dependency segment");
        }

        const u64 segmentStart = loadBase + ph.virtualAddress;
        const u64 mapStart = alignDown(segmentStart, PAGE_SIZE);
        const u64 pageOffset = segmentStart - mapStart;
        const u64 mapSize = alignUp(pageOffset + ph.memorySize, PAGE_SIZE);
        mapMemory(mapStart, mapSize, ProtRead | ProtWrite | ProtExecute);
        copyBytes(reinterpret_cast<void*>(segmentStart),
                  static_cast<const u8*>(data) + ph.offset,
                  ph.fileSize);

        if (object->segmentCount >= MAX_SEGMENTS) {
            fatal("too many segments");
        }
        object->segments[object->segmentCount++] = {
            mapStart,
            mapSize,
            protectionFromElf(ph.flags),
        };

        if (mapStart + mapSize > maxAddress) {
            maxAddress = mapStart + mapSize;
        }
    }

    if (header->type == Elf::TypeDynamic) {
        nextLibraryBase = alignUp(maxAddress + LIBRARY_STRIDE, LIBRARY_STRIDE);
    }

    for (u16 i = 0; i < header->programHeaderCount; ++i) {
        const Elf::ProgramHeader64& ph = phdrs[i];
        if (ph.type == Elf::ProgramDynamic) {
            object->dynamic = reinterpret_cast<Elf::DynamicEntry64*>(loadBase + ph.virtualAddress);
        } else if (ph.type == Elf::ProgramPhdr) {
            object->phdr = reinterpret_cast<Elf::ProgramHeader64*>(loadBase + ph.virtualAddress);
        }
    }
    if (!object->phdr) {
        object->phdr = reinterpret_cast<Elf::ProgramHeader64*>(loadBase + header->programHeaderOffset);
    }

    parseDynamic(*object);
    return object;
}

// Map a glibc-style component SONAME to the InstantOS runtime that actually
// provides it. InstantOS ships a single unified userland runtime
// (libinstant.so) covering the C library and the math/pthread/dl/rt symbols
// that glibc splits across separate shared objects. Ported binaries (e.g. tcc,
// whose Makefile hardcodes LIBS=-lm) carry DT_NEEDED entries like "libm.so.6"
// that have no standalone file here; resolve them to libinstant.so so their
// symbols are satisfied from the global symbol table. Names we don't recognize
// pass through unchanged and are looked up as real files under /lib.
const char* resolveSoname(const char* needed) {
    struct Alias {
        const char* from;
        const char* to;
    };
    static const Alias aliases[] = {
        {"libm.so.6", "libinstant.so"},
        {"libc.so.6", "libinstant.so"},
        {"libc.so", "libinstant.so"},
        {"libpthread.so.0", "libinstant.so"},
        {"libdl.so.2", "libinstant.so"},
        {"librt.so.1", "libinstant.so"},
    };
    for (const Alias& alias : aliases) {
        if (cstrEqual(needed, alias.from)) {
            return alias.to;
        }
    }
    return needed;
}

Object* loadDependency(const char* needed) {
    // Resolve glibc SONAME aliases before the dedup check so that, e.g.,
    // "libm.so.6" and an explicit "libinstant.so" in the same DT_NEEDED list
    // collapse to a single loaded object instead of being loaded twice.
    const char* resolved = resolveSoname(needed);
    if (Object* existing = findObject(resolved)) {
        return existing;
    }

    char path[128] = {};
    copyString(path, sizeof(path), "/lib/");
    appendString(path, sizeof(path), resolved);

    u64 size = 0;
    void* data = readWholeFile(path, &size);
    return loadObjectFromMemory(resolved, data, size, nextLibraryBase);
}

u64 lookupSymbol(const char* name, bool* found) {
    for (u32 objIndex = 0; objIndex < objectCount; objIndex++) {
        Object& object = objects[objIndex];
        if (!object.symtab || !object.strtab) {
            continue;
        }

        for (u32 i = 1; i < object.symbolCount; i++) {
            Elf::Symbol64& sym = object.symtab[i];
            if (sym.sectionIndex == 0 || sym.name == 0) {
                continue;
            }
            if (cstrEqual(name, object.strtab + sym.name)) {
                *found = true;
                return object.base + sym.value;
            }
        }
    }

    *found = false;
    return 0;
}

void applyRelocation(Object& object, const Elf::Rela64& rela) {
    const u32 type = Elf::relocationType(rela.info);
    const u32 symbolIndex = Elf::relocationSymbol(rela.info);
    u64* target = reinterpret_cast<u64*>(object.base + rela.offset);

    switch (type) {
        case Elf::RelocationX86_64None:
            return;
        case Elf::RelocationX86_64Relative:
            *target = object.base + static_cast<u64>(rela.addend);
            return;
        case Elf::RelocationX86_64_64:
        case Elf::RelocationX86_64GlobDat:
        case Elf::RelocationX86_64JumpSlot: {
            if (!object.symtab || symbolIndex >= object.symbolCount) {
                fatal("bad symbol relocation");
            }
            Elf::Symbol64& sym = object.symtab[symbolIndex];
            const char* name = object.strtab + sym.name;
            bool found = false;
            u64 value = lookupSymbol(name, &found);
            if (!found) {
                if (Elf::symbolBinding(sym.info) == 2) {
                    value = 0;
                } else {
                    fatal("unresolved symbol");
                }
            }
            *target = value + static_cast<u64>(rela.addend);
            return;
        }
        default:
            fatal("unsupported relocation");
    }
}

void relocateObject(Object& object) {
    if (object.relocated) {
        return;
    }

    for (u64 i = 0; i < object.relaCount; i++) {
        applyRelocation(object, object.rela[i]);
    }
    for (u64 i = 0; i < object.jmprelCount; i++) {
        applyRelocation(object, object.jmprel[i]);
    }

    object.relocated = true;
}

void protectObject(Object& object) {
    for (u32 i = 0; i < object.segmentCount; i++) {
        protectMemory(object.segments[i].address, object.segments[i].size, object.segments[i].protection);
    }
}

void runInit(Object& object) {
    if (object.initDone) {
        return;
    }
    object.initDone = true;

    if (object.init) {
        object.init();
    }
    for (u64 i = 0; i < object.initArrayCount; i++) {
        if (object.initArray[i]) {
            object.initArray[i]();
        }
    }
}

Object* registerMainObject(u64 stack) {
    u64* words = reinterpret_cast<u64*>(stack);
    u64 argc = words[0];
    u64 index = 1 + argc + 1;
    while (words[index] != 0) {
        index++;
    }
    index++;

    Elf::AuxEntry64* aux = reinterpret_cast<Elf::AuxEntry64*>(&words[index]);
    u64 phdrAddress = 0;
    u64 phnum = 0;
    u64 entry = 0;
    for (u64 i = 0; aux[i].type != Elf::AuxNull; i++) {
        if (aux[i].type == Elf::AuxPhdr) phdrAddress = aux[i].value;
        if (aux[i].type == Elf::AuxPhnum) phnum = aux[i].value;
        if (aux[i].type == Elf::AuxEntry) entry = aux[i].value;
    }

    if (!phdrAddress || !phnum || !entry) {
        fatal("missing auxv");
    }

    auto* phdr = reinterpret_cast<Elf::ProgramHeader64*>(phdrAddress);
    u64 base = 0;
    bool foundBase = false;
    for (u64 i = 0; i < phnum; i++) {
        if (phdr[i].type == Elf::ProgramPhdr) {
            base = phdrAddress - phdr[i].virtualAddress;
            foundBase = true;
            break;
        }
    }
    if (!foundBase) {
        fatal("main object has no PT_PHDR");
    }

    Object* main = registerObject("<main>");
    main->base = base;
    main->entry = entry;
    main->phdr = phdr;
    main->phnum = phnum;

    for (u64 i = 0; i < phnum; i++) {
        if (phdr[i].type == Elf::ProgramDynamic) {
            main->dynamic = reinterpret_cast<Elf::DynamicEntry64*>(base + phdr[i].virtualAddress);
            break;
        }
    }

    parseDynamic(*main);
    return main;
}
}

extern "C" __attribute__((naked)) uint64_t dl_syscall(
    uint64_t number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5
) {
    asm volatile(
        ".intel_syntax noprefix\n"
        "push rbx\n"
        "mov rax, rdi\n"
        "mov rbx, rsi\n"
        "mov r10, rdx\n"
        "mov rdx, rcx\n"
        "syscall\n"
        "pop rbx\n"
        "ret\n"
        ".att_syntax prefix\n"
    );
}

extern "C" uint64_t ld_main(uint64_t stack) {
    Object* main = registerMainObject(stack);

    for (u32 i = 0; i < objectCount; i++) {
        Object& object = objects[i];
        for (u32 dep = 0; dep < object.neededCount; dep++) {
            loadDependency(object.needed[dep]);
        }
    }

    for (u32 i = 0; i < objectCount; i++) {
        relocateObject(objects[i]);
    }

    for (u32 i = 1; i < objectCount; i++) {
        protectObject(objects[i]);
    }

    for (u32 i = 1; i < objectCount; i++) {
        runInit(objects[i]);
    }
    runInit(*main);

    return main->entry;
}

extern "C" __attribute__((naked, noreturn)) void _start() {
    asm volatile(
        ".intel_syntax noprefix\n"
        "mov r12, rsp\n"
        "mov rdi, rsp\n"
        "and rsp, -16\n"
        "call ld_main\n"
        "mov rsp, r12\n"
        "jmp rax\n"
        ".att_syntax prefix\n"
    );
}
