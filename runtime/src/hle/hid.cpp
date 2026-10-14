// hid:USER: buttons, circle pad, touch screen and motion sensors through shared memory.
//
// The 3DS HID module updates rings of recent samples in shared memory and signals an event;
// games read the latest entry. A kernel timer does the same here, sampling the frontend's
// keyboard/controller/mouse state.
#include "service.h"

#include "frontend/frontend.h"
#include "log.h"
#include "memory.h"

#include <cstring>

namespace hle {
namespace {

constexpr uint32_t SHARED_SIZE = 0x2B0;
constexpr uint64_t UPDATE_NS = 1000000000ull / 60;

// shared memory layout (offsets into the block)
constexpr uint32_t PAD = 0x000, TOUCH = 0x0A8, ACCEL = 0x108, GYRO = 0x158;

// platform input, plus the circle-pad direction bits the HID module derives from the stick
frontend::Input current_input() {
    frontend::Input in = frontend::input();
    constexpr int THRESHOLD = 40;
    if (in.circle_x > THRESHOLD) in.buttons |= 1u << 28;
    if (in.circle_x < -THRESHOLD) in.buttons |= 1u << 29;
    if (in.circle_y > THRESHOLD) in.buttons |= 1u << 30;
    if (in.circle_y < -THRESHOLD) in.buttons |= 1u << 31;
    return in;
}

struct Hid : Service {
    std::shared_ptr<kernel::SharedMemory> shared = std::make_shared<kernel::SharedMemory>();
    std::shared_ptr<kernel::Event> pad0 = std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "HID pad 0");
    std::shared_ptr<kernel::Event> pad1 = std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "HID pad 1");
    std::shared_ptr<kernel::Event> accel = std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "HID accel");
    std::shared_ptr<kernel::Event> gyro = std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "HID gyro");
    std::shared_ptr<kernel::Event> debug = std::make_shared<kernel::Event>(kernel::ResetType::OneShot, "HID debug pad");
    uint32_t pad_index = 7, touch_index = 7, accel_index = 7, gyro_index = 7;
    uint32_t previous_buttons = 0;
    bool started = false;

    Hid() : Service("hid") {
        shared->size = SHARED_SIZE;
        shared->name = "HID";
    }

    void write32(uint32_t off, uint32_t v) { mem::write32(shared->address + off, v); }
    void write64(uint32_t off, uint64_t v) { std::memcpy(mem::ptr(shared->address + off), &v, 8); }

    // one update of every ring, like the HID module's sampling (kernel lock held)
    void update() {
        if (!shared->mapped) return;
        uint64_t now = kernel::ticks();
        frontend::Input in = current_input();

        // pad: header (ticks, index, raw state, circle pad) and 8 entries of 0x10 bytes at 0x28
        pad_index = (pad_index + 1) % 8;
        uint32_t pressed = in.buttons & ~previous_buttons, released = previous_buttons & ~in.buttons;
        previous_buttons = in.buttons;
        uint64_t last;
        std::memcpy(&last, mem::ptr(shared->address + PAD), 8);
        write64(PAD + 0x08, last);
        write64(PAD + 0x00, now);
        write32(PAD + 0x10, pad_index);
        write32(PAD + 0x1C, in.buttons);
        mem::write32(shared->address + PAD + 0x20, uint32_t(uint16_t(in.circle_x)) | (uint32_t(uint16_t(in.circle_y)) << 16));
        uint32_t e = PAD + 0x28 + pad_index * 0x10;
        write32(e + 0, in.buttons);
        write32(e + 4, pressed);
        write32(e + 8, released);
        write32(e + 12, uint32_t(uint16_t(in.circle_x)) | (uint32_t(uint16_t(in.circle_y)) << 16));

        // touch: header and 8 entries of 8 bytes at 0x20
        touch_index = (touch_index + 1) % 8;
        std::memcpy(&last, mem::ptr(shared->address + TOUCH), 8);
        write64(TOUCH + 0x08, last);
        write64(TOUCH + 0x00, now);
        write32(TOUCH + 0x10, touch_index);
        uint32_t t = TOUCH + 0x20 + touch_index * 8;
        write32(t, uint32_t(in.touch_x) | (uint32_t(in.touch_y) << 16));
        write32(t + 4, in.touching);

        // accelerometer and gyroscope: device at rest
        accel_index = (accel_index + 1) % 8;
        write64(ACCEL, now);
        write32(ACCEL + 0x10, accel_index);
        gyro_index = (gyro_index + 1) % 32;
        write64(GYRO, now);
        write32(GYRO + 0x10, gyro_index);

        pad0->signal();
        pad1->signal();
        accel->signal();
        gyro->signal();
    }

    void start_updates() {
        if (started) return;
        started = true;
        kernel::schedule_periodic(UPDATE_NS, [this] {
            update();
        });
    }

    void handle(Request &req) override {
        switch (req.id()) {
        case 0x000A: { // GetIPCHandles -> shared memory, pad 0/1, accelerometer, gyroscope, debug pad events
            auto r = req.reply(1, 7);
            r.push_handles({kernel::add_handle(shared), kernel::add_handle(pad0), kernel::add_handle(pad1),
                            kernel::add_handle(accel), kernel::add_handle(gyro), kernel::add_handle(debug)});
            start_updates();
            break;
        }
        case 0x0011: // EnableAccelerometer
        case 0x0012: // DisableAccelerometer
        case 0x0013: // EnableGyroscopeLow
        case 0x0014: // DisableGyroscopeLow
            req.reply(1, 0);
            break;
        case 0x0015: { // GetGyroscopeLowRawToDpsCoefficient
            auto r = req.reply(2, 0);
            float coefficient = 14.375f;
            uint32_t bits;
            std::memcpy(&bits, &coefficient, 4);
            r.push(bits);
            break;
        }
        case 0x0016: { // GetGyroscopeLowCalibrateParam -> 0x14 bytes
            auto r = req.reply(6, 0);
            for (int i = 0; i < 5; i++) r.push(0);
            break;
        }
        case 0x0017: { // GetSoundVolume
            auto r = req.reply(2, 0);
            r.push(0x3F);
            break;
        }
        default:
            unimplemented(*this, req);
        }
    }
};

} // namespace

void register_hid() { register_service({"hid:USER", "hid:SPVR"}, std::make_shared<Hid>()); }

} // namespace hle
