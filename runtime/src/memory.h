#pragma once
#include <cstdint>

// The guest's 32-bit address space: 4 GiB reserved up front at recomp_mem, with pages
// committed only where the 3DS would have memory mapped, so stray accesses fault.
namespace mem {

constexpr uint32_t PAGE = 0x1000;

// 3DS application address map
constexpr uint32_t CODE_BASE = 0x00100000;
constexpr uint32_t HEAP_BASE = 0x08000000;
constexpr uint32_t STACK_TOP = 0x10000000;
constexpr uint32_t LINEAR_BASE = 0x14000000; // FCRAM linear heap (old 3DS mapping)
constexpr uint32_t VRAM_BASE = 0x1F000000;
constexpr uint32_t VRAM_SIZE = 0x00600000;
constexpr uint32_t CONFIG_PAGE = 0x1FF80000; // kernel config memory (read-only to apps)
constexpr uint32_t SHARED_PAGE = 0x1FF81000;
constexpr uint32_t TLS_BASE = 0x1FF82000; // 0x200 bytes per thread

void init();
// commits [addr, addr+size) read/write, zero-filled; page-aligned
void commit(uint32_t addr, uint32_t size);
void decommit(uint32_t addr, uint32_t size);
bool is_committed(uint32_t addr);

uint8_t *ptr(uint32_t addr);
uint32_t read32(uint32_t addr);
void write32(uint32_t addr, uint32_t value);

// guest address of a host pointer into guest memory, for fault reporting; false if outside
bool guest_address(const void *host, uint32_t *out);

inline uint32_t page_down(uint32_t a) { return a & ~(PAGE - 1); }
inline uint64_t page_up(uint64_t a) { return (a + PAGE - 1) & ~uint64_t(PAGE - 1); }

} // namespace mem
