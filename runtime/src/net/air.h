#pragma once
// The air: what local wireless frames travel through between Clockwork processes. Each process
// joins a room on a relay (net/relay.h) over UDP, and the relay hands every frame to everyone else
// in the room, the way a radio frame reaches every console in range. The relay and room are chosen
// in the launcher's Rooms tab (CLOCKWORK_RELAY=host:port overrides the relay). Without one the hub
// is on this PC: the first process that needs it runs it, so two windows play together.
// See docs/PLAN.md, Rooms.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace net {

// joins the room (starting the hub here if it's local and nobody runs it); false if sockets don't
// work or the relay can't be found. From then on a thread keeps us in the room.
bool air_start();
// sends a frame to everyone else on the air
void air_send(const void *data, size_t size);
// the next frame from someone else, without waiting; false when none has arrived
bool air_recv(std::vector<uint8_t> &frame);
// whether a relay is chosen: then the game joins its room when it starts, so the others see it
bool air_wanted();

} // namespace net
