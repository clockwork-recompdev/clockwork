#pragma once
// HLE of the 3DS kernel for one application process.
//
// Threads: each guest thread runs on its own host thread, but only the thread the scheduler
// picked ("current") executes guest code. Switches happen at SVCs, like the 3DS application
// core: a blocking SVC hands the CPU to the highest-priority ready thread, and a thread woken
// by a higher-priority event takes over at the running thread's next SVC.
//
// Kernel state is protected by one mutex (`lock`), held only while handling an SVC or a
// host-side event (timers, service interrupts), never while guest code runs.

#include "loader.h"
#include "recomp.h"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hle {
struct Service;
}

namespace kernel {

using Handle = uint32_t;
constexpr Handle CURRENT_THREAD = 0xFFFF8000;
constexpr Handle CURRENT_PROCESS = 0xFFFF8001;

// result codes
constexpr uint32_t RESULT_OK = 0;
constexpr uint32_t RESULT_TIMEOUT = 0x09401BFE;
constexpr uint32_t RESULT_INVALID_HANDLE = 0xD8E007F7;
constexpr uint32_t RESULT_INVALID_COMBINATION = 0xE0E01BEE;
constexpr uint32_t RESULT_OUT_OF_MEMORY = 0xD86007F3;
constexpr uint32_t RESULT_NOT_FOUND = 0xD88007FA;
constexpr uint32_t RESULT_NOT_AUTHORIZED = 0xD9001BEA;

constexpr uint64_t TICKS_PER_SECOND = 268111856; // ARM11 clock

// conversions split into whole seconds so they don't overflow 64 bits (a plain
// ns * TICKS_PER_SECOND would after 68.8 seconds); results saturate near UINT64_MAX
constexpr uint64_t ns_to_ticks(uint64_t ns) {
    uint64_t s = ns / 1000000000ull;
    if (s > UINT64_MAX / TICKS_PER_SECOND / 2) return UINT64_MAX / 2;
    return s * TICKS_PER_SECOND + (ns % 1000000000ull) * TICKS_PER_SECOND / 1000000000ull;
}
constexpr uint64_t ticks_to_ns(uint64_t t) {
    uint64_t s = t / TICKS_PER_SECOND;
    if (s > UINT64_MAX / 1000000000ull / 2) return UINT64_MAX / 2;
    return s * 1000000000ull + (t % TICKS_PER_SECOND) * 1000000000ull / TICKS_PER_SECOND;
}
uint64_t ticks();

struct Thread;

struct Object : std::enable_shared_from_this<Object> {
    virtual ~Object() = default;
    virtual const char *type_name() const = 0;
};

// An object threads can wait on with WaitSynchronization.
struct WaitObject : Object {
    std::vector<Thread *> waiters;
    virtual bool should_wait(const Thread *t) const = 0;
    virtual void acquire(Thread *t) = 0;
    // wakes whichever waiters can now proceed (call with the kernel lock held)
    void wake_waiters();
};

enum class ResetType : uint32_t { OneShot = 0, Sticky = 1, Pulse = 2 };

struct Event : WaitObject {
    ResetType reset;
    bool signaled = false;
    std::string name;
    Event(ResetType r, std::string n) : reset(r), name(std::move(n)) {}
    const char *type_name() const override { return "Event"; }
    bool should_wait(const Thread *) const override { return !signaled; }
    void acquire(Thread *) override {
        if (reset == ResetType::OneShot) signaled = false;
    }
    void signal();
    void clear() { signaled = false; }
};

struct Mutex : WaitObject {
    Thread *owner = nullptr;
    uint32_t lock_count = 0;
    const char *type_name() const override { return "Mutex"; }
    bool should_wait(const Thread *t) const override { return owner && owner != t; }
    void acquire(Thread *t) override; // kernel.cpp: the owner remembers it, to release it on exit
};

struct Semaphore : WaitObject {
    int32_t count, max;
    Semaphore(int32_t c, int32_t m) : count(c), max(m) {}
    const char *type_name() const override { return "Semaphore"; }
    bool should_wait(const Thread *) const override { return count <= 0; }
    void acquire(Thread *) override { count--; }
};

struct Timer : WaitObject {
    ResetType reset;
    bool signaled = false;
    uint64_t interval_ns = 0;
    uint64_t generation = 0; // bumps on Set/Cancel so stale timer-queue entries are ignored
    explicit Timer(ResetType r) : reset(r) {}
    const char *type_name() const override { return "Timer"; }
    bool should_wait(const Thread *) const override { return !signaled; }
    void acquire(Thread *) override {
        if (reset == ResetType::OneShot) signaled = false;
    }
    void fire();
};

struct AddressArbiter : Object {
    const char *type_name() const override { return "AddressArbiter"; }
};

struct ResourceLimit : Object {
    const char *type_name() const override { return "ResourceLimit"; }
};

// A memory block shared between the game and a service (or created by the game itself).
// Service-owned blocks get their guest address when the game maps them, since the game
// picks the address; the service writes to `address` once `mapped` is set.
struct SharedMemory : Object {
    uint32_t address = 0;
    uint32_t size = 0;
    bool mapped = false;
    std::string name;
    const char *type_name() const override { return "SharedMemory"; }
};

// A client session to an HLE service; SendSyncRequest on it calls the service.
struct Session : Object {
    std::shared_ptr<hle::Service> service;
    explicit Session(std::shared_ptr<hle::Service> s) : service(std::move(s)) {}
    const char *type_name() const override { return "Session"; }
};

enum class ThreadState { Ready, Running, Waiting, Dead };

struct Thread : WaitObject {
    Ctx ctx{};
    uint32_t id = 0;
    int32_t priority = 0;
    uint32_t entry = 0;
    ThreadState state = ThreadState::Ready;
    std::condition_variable cv;
    std::thread host;

    // current wait
    std::vector<std::shared_ptr<WaitObject>> wait_objects;
    bool wait_all = false;
    uint64_t wait_id = 0; // distinguishes waits for timeout bookkeeping
    uint32_t wait_result = RESULT_OK;
    int32_t wait_index = -1;
    uint32_t arbiter_addr = 0; // non-zero while waiting on an address arbiter
    AddressArbiter *arbiter = nullptr;
    uint32_t last_svc = 0, last_svc_addr = 0, last_svc_lr = 0; // for thread dumps
    // mutexes it has locked: released when it exits, as the 3DS kernel does (a thread that ends
    // holding one would otherwise leave its waiters stuck for good)
    std::vector<std::weak_ptr<Mutex>> held;

    const char *type_name() const override { return "Thread"; }
    bool should_wait(const Thread *) const override { return state != ThreadState::Dead; }
    void acquire(Thread *) override {}
};

// ---- process-wide kernel state ----

struct Kernel {
    std::mutex lock;
    Thread *current = nullptr;
    std::vector<std::shared_ptr<Thread>> threads;
    std::map<Handle, std::shared_ptr<Object>> handles;
    Handle next_handle = 0x00010000;
    uint32_t next_thread_id = 1;
    uint64_t next_wait_id = 1;

    // heap bookkeeping for ControlMemory / QueryMemory
    uint32_t heap_size = 0;
    uint32_t next_shared = 0x10000000; // where unplaced shared memory blocks get mapped
    uint32_t linear_used = 0;
    struct Region {
        uint32_t size;
        uint32_t perm;
        uint32_t state;
    };
    std::map<uint32_t, Region> regions; // by base address

    // timer queue: deadline (ticks) -> action
    std::multimap<uint64_t, std::function<void()>> timers;
    std::condition_variable timer_cv;
    std::thread timer_thread;
};

Kernel &k();

void init(const GameImage &game);
// runs the main thread from the entry point on the calling host thread; doesn't return
[[noreturn]] void run(const GameImage &game);

// ---- for SVCs and services (call with k().lock held) ----

Handle add_handle(std::shared_ptr<Object> obj);
std::shared_ptr<Object> get_handle(Handle h);
template <typename T> std::shared_ptr<T> get(Handle h) { return std::dynamic_pointer_cast<T>(get_handle(h)); }
bool close_handle(Handle h);

Thread *current_thread();
// marks a waiting thread ready (e.g. its wait finished); higher priority ones run at the next SVC
void make_ready(Thread *t);
// blocks the current thread until something makes it ready again; releases and retakes the lock
void block_current(std::unique_lock<std::mutex> &lk);
// if a higher-priority thread is ready, let it run (end of every SVC)
void maybe_preempt(std::unique_lock<std::mutex> &lk);
// Lets other guest threads run while the current thread does long host-side work (like a
// real 3DS running other threads while the GPU draws). Call with the kernel lock held, then
// unlock for the work; reacquire_cpu (lock held) waits until the thread is scheduled again.
void release_cpu();
void reacquire_cpu(std::unique_lock<std::mutex> &lk, Thread *t);
// gives up the CPU to another ready thread of the same or higher priority
void yield(std::unique_lock<std::mutex> &lk);
// wakes the current thread after `ns` with RESULT_TIMEOUT unless its wait ends first
void arm_wait_timeout(Thread *t, int64_t ns);
// runs `fn` under the kernel lock after `ns` nanoseconds
void schedule_timer(uint64_t ns, std::function<void()> fn);
// runs `fn` every `ns` on a fixed schedule (late runs catch up rather than drift; after a
// stall of more than 8 periods the schedule restarts from now); kernel lock held
void schedule_periodic(uint64_t ns, std::function<void()> fn);

// prints every guest thread and what it's waiting on (debugging hangs)
void dump_threads(std::FILE *to = stderr);

std::shared_ptr<Thread> create_thread(uint32_t entry, uint32_t arg, uint32_t stack_top, int32_t priority);
[[noreturn]] void exit_current_thread(std::unique_lock<std::mutex> &lk);

} // namespace kernel
