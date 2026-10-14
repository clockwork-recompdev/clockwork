#include "platform.h"

#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace platform {

std::filesystem::path data_dir() {
    static std::filesystem::path dir = [] {
        std::filesystem::path base;
        auto env = [](const char *name) -> const char * {
            const char *v = std::getenv(name);
            return v && *v ? v : nullptr;
        };
#if defined(_WIN32)
        if (auto appdata = env("APPDATA")) base = appdata;
#elif defined(__APPLE__)
        if (auto home = env("HOME")) base = std::filesystem::path(home) / "Library" / "Application Support";
#else
        if (auto xdg = env("XDG_DATA_HOME")) base = xdg;
        else if (auto home = env("HOME")) base = std::filesystem::path(home) / ".local" / "share";
#endif
        if (base.empty()) fatal("could not find a user data directory");
        auto d = base / "yw2-recomp";
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

std::filesystem::path game_dir(uint64_t program_id) {
    char id[17];
    std::snprintf(id, sizeof id, "%016llx", (unsigned long long)program_id);
    return data_dir() / "games" / id;
}

int instance_slot() {
    static const int slot = [] {
        for (int n = 0; n < 16; n++) {
            std::filesystem::path lock = data_dir() / ("instance-" + std::to_string(n) + ".lock");
#if defined(_WIN32)
            // no sharing: a second process can't open it while this one has it open
            HANDLE h = CreateFileW(lock.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) return n; // kept open until the process ends
#else
            int fd = open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
            if (fd < 0) break;
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) return n; // kept until the process ends
            close(fd);
#endif
        }
        return 0;
    }();
    return slot;
}

namespace {

uint64_t mix64(uint64_t x) { // splitmix64
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t console_id_from(uint64_t bits) { return (bits & 0x7FFFFFFFFull) | ((bits >> 32) & 0xFFFF) << 48; }

} // namespace

uint64_t console_id() {
    static const uint64_t id = [] {
        std::filesystem::path file = data_dir() / "console_id";
        uint64_t base = 0;
        if (std::ifstream in(file); in) in >> std::hex >> base;
        if (!base) {
            std::random_device rd;
            base = console_id_from(uint64_t(rd()) << 32 | rd());
            std::ofstream(file) << std::hex << base << "\n";
            LOG("made this console's id: %016llx", (unsigned long long)base);
        }
        int slot = instance_slot();
        uint64_t mine = slot ? console_id_from(mix64(base + uint64_t(slot))) : base;
        if (slot) LOG("window %d on this PC: console id %016llx", slot + 1, (unsigned long long)mine);
        return mine;
    }();
    return id;
}

} // namespace platform
