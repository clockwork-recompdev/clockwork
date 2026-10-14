#include "loader.h"

#include "dump.h"

#include "log.h"
#include "memory.h"

#include <cstring>
#include <vector>

namespace {
uint32_t u32(const std::vector<uint8_t> &b, size_t at) {
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}
} // namespace

GameImage load_game() {
    const auto &exheader = dump().exheader();
    const auto &code = dump().code();

    GameImage g;
    g.title.assign(reinterpret_cast<const char *>(exheader.data()), strnlen(reinterpret_cast<const char *>(exheader.data()), 8));
    uint32_t text_addr = u32(exheader, 0x10);
    uint32_t data_addr = u32(exheader, 0x30), data_size = u32(exheader, 0x38);
    g.entry = text_addr;
    g.stack_size = u32(exheader, 0x1C);
    g.data_end = data_addr + data_size;
    g.bss_size = u32(exheader, 0x3C);
    // ARM11 local system capabilities: main thread priority at 0x200 + 0x0F
    g.main_priority = exheader[0x20F];

    if (text_addr != mem::CODE_BASE) fatal("unexpected code address %08x", text_addr);
    uint64_t code_end = mem::page_up(uint64_t(text_addr) + code.size());
    uint64_t bss_end = mem::page_up(uint64_t(g.data_end) + g.bss_size);
    mem::commit(text_addr, uint32_t(std::max(code_end, bss_end) - text_addr));
    std::memcpy(mem::ptr(text_addr), code.data(), code.size());
    // .bss shares the last .data page; make sure the tail is zero
    std::memset(mem::ptr(g.data_end), 0, size_t(bss_end - g.data_end));

    LOG("loaded %s: code %08x-%08x, .bss to %08x, stack %#x, main priority %#x", g.title.c_str(), text_addr,
        uint32_t(text_addr + code.size()), uint32_t(g.data_end + g.bss_size), g.stack_size, g.main_priority);
    return g;
}
