#include "memory.h"

#include "log.h"
#include "recomp.h"

#include <bitset>
#include <cstring>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t *recomp_mem = nullptr;

namespace mem {
namespace {
constexpr uint64_t SPACE = uint64_t(1) << 32;
// one bit per guest page
std::unique_ptr<std::bitset<SPACE / PAGE>> committed;

void check_range(uint32_t addr, uint32_t size) {
    if (addr % PAGE || size % PAGE || uint64_t(addr) + size > SPACE)
        fatal("bad memory range %08x+%08x", addr, size);
}
} // namespace

void init() {
#ifdef _WIN32
    void *p = VirtualAlloc(nullptr, SPACE, MEM_RESERVE, PAGE_NOACCESS);
#else
    void *p = mmap(nullptr, SPACE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) p = nullptr;
#endif
    if (!p) fatal("could not reserve 4 GiB of address space for guest memory");
    recomp_mem = static_cast<uint8_t *>(p);
    committed = std::make_unique<std::bitset<SPACE / PAGE>>();
}

void commit(uint32_t addr, uint32_t size) {
    check_range(addr, size);
    if (!size) return;
#ifdef _WIN32
    if (!VirtualAlloc(recomp_mem + addr, size, MEM_COMMIT, PAGE_READWRITE))
#else
    if (mprotect(recomp_mem + addr, size, PROT_READ | PROT_WRITE) != 0)
#endif
        fatal("could not commit guest memory %08x+%08x", addr, size);
    for (uint64_t a = addr; a < uint64_t(addr) + size; a += PAGE) committed->set(a / PAGE);
}

void decommit(uint32_t addr, uint32_t size) {
    check_range(addr, size);
    if (!size) return;
#ifdef _WIN32
    VirtualFree(recomp_mem + addr, size, MEM_DECOMMIT);
#else
    // drop the pages (so recommit is zero-filled), then make them inaccessible
    mmap(recomp_mem + addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
#endif
    for (uint64_t a = addr; a < uint64_t(addr) + size; a += PAGE) committed->reset(a / PAGE);
}

bool is_committed(uint32_t addr) { return committed->test(addr / PAGE); }

uint8_t *ptr(uint32_t addr) { return recomp_mem + addr; }

uint32_t read32(uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, recomp_mem + addr, 4);
    return v;
}

void write32(uint32_t addr, uint32_t value) { std::memcpy(recomp_mem + addr, &value, 4); }

bool guest_address(const void *host, uint32_t *out) {
    auto p = static_cast<const uint8_t *>(host);
    if (p < recomp_mem || uint64_t(p - recomp_mem) >= SPACE) return false;
    *out = uint32_t(p - recomp_mem);
    return true;
}

} // namespace mem
