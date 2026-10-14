#pragma once
#include <cstdint>
#include <string>

struct GameImage {
    std::string title;
    uint32_t entry = 0;
    uint32_t data_end = 0; // end of .data; .bss follows
    uint32_t bss_size = 0;
    uint32_t stack_size = 0;
    int main_priority = 0x30;
};

// Maps the game's code (from the dump) into guest memory and clears .bss.
GameImage load_game();
