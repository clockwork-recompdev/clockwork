#pragma once
#include <SDL3/SDL.h>

#include <string>

namespace frontend {
// Runs the launcher in `window` until Play (true) or Quit/close (false); saves the settings.
// `gpu` describes the GPU in use (empty if none), shown on the Display tab.
bool run_launcher(SDL_Window *window, SDL_Renderer *renderer, const std::string &gpu);
} // namespace frontend
