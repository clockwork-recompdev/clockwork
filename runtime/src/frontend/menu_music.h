#pragma once
// Music for the launcher: menu-music.wav in the data folder if the player put one there, else
// Yo-kai Watch Blasters' version-select theme, read from the player's own Blasters dump (nothing
// from the game ships with Clockwork). Silent if there's neither.

namespace frontend::menu_music {

// loads it on its own thread and plays it, looping, once loaded
void start();
bool started();
// 0-1, applied as it plays
void set_volume(float volume);
// stops it and waits for the loading to end
void stop();

} // namespace frontend::menu_music
