// SVC (system call) dispatch. Register usage follows the 3DS kernel ABI: arguments in r0-r5,
// result code in r0, outputs from r1 up.

#include "hle/service.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <cstring>

namespace kernel {
namespace {

using Lock = std::unique_lock<std::mutex>;

int64_t s64(uint32_t lo, uint32_t hi) { return int64_t((uint64_t(hi) << 32) | lo); }

// ---- memory ----

constexpr uint32_t MEMOP_FREE = 1, MEMOP_COMMIT = 3, MEMOP_PROTECT = 6; // MAP (4) / UNMAP (5) not supported
constexpr uint32_t MEMOP_LINEAR = 0x10000;
constexpr uint32_t MEMSTATE_FREE = 0, MEMSTATE_PRIVATE = 5, MEMSTATE_CONTINUOUS = 6;

uint32_t control_memory(Ctx *c) {
    uint32_t op = c->r[0], addr0 = c->r[1], size = c->r[3], perm = c->r[4];
    uint32_t kind = op & 0xFF;
    size = uint32_t(mem::page_up(size));
    auto &kk = k();
    switch (kind) {
    case MEMOP_COMMIT: {
        uint32_t addr;
        if (op & MEMOP_LINEAR) {
            // linear heap: physically contiguous memory the GPU/DSP can use
            addr = addr0 ? addr0 : mem::LINEAR_BASE + kk.linear_used;
            if (!addr0) kk.linear_used += size;
            kk.regions[addr] = {size, perm, MEMSTATE_CONTINUOUS};
        } else {
            addr = addr0;
            if (addr < mem::HEAP_BASE || addr + size > mem::STACK_TOP - 0x01000000) return RESULT_INVALID_COMBINATION;
            kk.regions[addr] = {size, perm, MEMSTATE_PRIVATE};
            kk.heap_size = std::max(kk.heap_size, addr + size - mem::HEAP_BASE);
        }
        mem::commit(addr, size);
        c->r[1] = addr;
        LOG_SVC("ControlMemory commit %08x+%08x%s", addr, size, op & MEMOP_LINEAR ? " (linear)" : "");
        return RESULT_OK;
    }
    case MEMOP_FREE:
        mem::decommit(addr0, size);
        kk.regions.erase(addr0);
        c->r[1] = addr0;
        LOG_SVC("ControlMemory free %08x+%08x", addr0, size);
        return RESULT_OK;
    case MEMOP_PROTECT:
        c->r[1] = addr0;
        return RESULT_OK;
    default:
        fatal("ControlMemory op %#x (addr0=%08x addr1=%08x size=%08x) not implemented", op, addr0, c->r[2], size);
    }
}

uint32_t query_memory(Ctx *c) {
    uint32_t addr = c->r[2];
    for (auto &[base, r] : k().regions) {
        if (addr >= base && addr < base + r.size) {
            c->r[1] = base;
            c->r[2] = r.size;
            c->r[3] = r.perm;
            c->r[4] = r.state;
            c->r[5] = 0;
            return RESULT_OK;
        }
    }
    // free: report the gap up to the next region
    auto next = k().regions.upper_bound(addr);
    uint32_t base = mem::page_down(addr);
    c->r[1] = base;
    c->r[2] = next == k().regions.end() ? mem::PAGE : next->first - base;
    c->r[3] = 0;
    c->r[4] = MEMSTATE_FREE;
    c->r[5] = 0;
    return RESULT_OK;
}

// ---- waiting ----

uint32_t wait_on(Lock &lk, std::vector<std::shared_ptr<WaitObject>> objs, bool wait_all, int64_t timeout,
                 int32_t *index) {
    Thread *t = current_thread();
    auto ready = [t](auto &o) { return !o->should_wait(t); };
    if (wait_all ? std::all_of(objs.begin(), objs.end(), ready) : std::any_of(objs.begin(), objs.end(), ready)) {
        if (wait_all) {
            for (auto &o : objs) o->acquire(t);
            *index = 0;
        } else {
            auto it = std::find_if(objs.begin(), objs.end(), ready);
            (*it)->acquire(t);
            *index = int32_t(it - objs.begin());
        }
        return RESULT_OK;
    }
    if (timeout == 0) return RESULT_TIMEOUT;
    for (auto &o : objs) o->waiters.push_back(t);
    t->wait_objects = std::move(objs);
    t->wait_all = wait_all;
    t->wait_id = k().next_wait_id++;
    arm_wait_timeout(t, timeout);
    block_current(lk);
    *index = t->wait_index;
    return t->wait_result;
}

uint32_t wait_sync1(Lock &lk, Ctx *c) {
    auto obj = get<WaitObject>(c->r[0]);
    if (!obj) return RESULT_INVALID_HANDLE;
    int32_t index;
    return wait_on(lk, {obj}, false, s64(c->r[2], c->r[3]), &index);
}

uint32_t wait_sync_n(Lock &lk, Ctx *c) {
    uint32_t list = c->r[1], count = c->r[2];
    bool wait_all = c->r[3] != 0;
    int64_t timeout = s64(c->r[0], c->r[4]);
    std::vector<std::shared_ptr<WaitObject>> objs;
    for (uint32_t i = 0; i < count; i++) {
        auto obj = get<WaitObject>(mem::read32(list + i * 4));
        if (!obj) return RESULT_INVALID_HANDLE;
        objs.push_back(obj);
    }
    if (objs.empty()) {
        // no handles: just a sleep
        if (timeout != 0) {
            Thread *t = current_thread();
            t->wait_id = k().next_wait_id++;
            arm_wait_timeout(t, timeout);
            block_current(lk);
        }
        return RESULT_TIMEOUT;
    }
    int32_t index = -1;
    uint32_t r = wait_on(lk, std::move(objs), wait_all, timeout, &index);
    c->r[1] = uint32_t(index);
    return r;
}

// ---- address arbiter ----

uint32_t arbitrate(Lock &lk, Ctx *c) {
    auto arb = get<AddressArbiter>(c->r[0]);
    if (!arb) return RESULT_INVALID_HANDLE;
    uint32_t addr = c->r[1], type = c->r[2];
    int32_t value = int32_t(c->r[3]);
    int64_t timeout = s64(c->r[4], c->r[5]);
    Thread *t = current_thread();
    switch (type) {
    case 0: { // signal: wake up to `value` waiters (all if negative), highest priority first
        std::vector<Thread *> ws;
        for (auto &th : k().threads)
            if (th->state == ThreadState::Waiting && th->arbiter == arb.get() && th->arbiter_addr == addr)
                ws.push_back(th.get());
        std::stable_sort(ws.begin(), ws.end(), [](Thread *a, Thread *b) { return a->priority < b->priority; });
        if (value >= 0 && ws.size() > size_t(value)) ws.resize(size_t(value));
        for (Thread *w : ws) {
            w->wait_result = RESULT_OK;
            make_ready(w);
        }
        return RESULT_OK;
    }
    case 1: case 2: case 3: case 4: {
        int32_t current = int32_t(mem::read32(addr));
        if (current >= value) return RESULT_OK;
        if (type == 2 || type == 4) mem::write32(addr, uint32_t(current - 1));
        t->arbiter = arb.get();
        t->arbiter_addr = addr;
        t->wait_id = k().next_wait_id++;
        if (type >= 3) arm_wait_timeout(t, timeout);
        block_current(lk);
        return t->wait_result;
    }
    default:
        fatal("ArbitrateAddress type %u", type);
    }
}

// ---- info ----

uint32_t resource_limit_values(Ctx *c, bool current) {
    uint32_t out = c->r[0], names = c->r[2], count = c->r[3];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t name = mem::read32(names + i * 4);
        int64_t v = 0;
        switch (name) {
        case 0: v = current ? 0 : 0x18; break;                        // priority
        case 1: v = current ? k().heap_size + k().linear_used : 0x04000000; break; // commit (memory)
        case 2: v = current ? int64_t(k().threads.size()) : 32; break; // threads
        case 3: v = current ? 0 : 32; break;                          // events
        case 4: v = current ? 0 : 32; break;                          // mutexes
        case 5: v = current ? 0 : 8; break;                           // semaphores
        case 6: v = current ? 0 : 8; break;                           // timers
        case 7: v = current ? 0 : 16; break;                          // shared memory
        case 8: v = current ? 0 : 2; break;                           // address arbiters
        case 9: v = current ? 0 : 1000; break;                        // cpu time
        default: break;
        }
        std::memcpy(mem::ptr(out + i * 8), &v, 8);
    }
    return RESULT_OK;
}

std::string read_cstring(uint32_t addr, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = char(mem::ptr(addr)[i]);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

uint32_t dispatch(Lock &lk, Ctx *c, uint32_t svc, uint32_t addr) {
    switch (svc) {
    case 0x01: return control_memory(c);
    case 0x02: return query_memory(c);
    case 0x03:
        LOG("process exited (svcExitProcess)");
        std::fflush(nullptr);
    std::_Exit(0);
    case 0x08: {
        int32_t prio = int32_t(c->r[0]);
        auto t = create_thread(c->r[1], c->r[2], c->r[3], prio);
        LOG_SVC("CreateThread entry=%08x arg=%08x stack=%08x prio=%#x core=%d -> thread %u", c->r[1], c->r[2],
                c->r[3], prio, int32_t(c->r[4]), t->id);
        c->r[1] = add_handle(t);
        return RESULT_OK;
    }
    case 0x09: exit_current_thread(lk);
    case 0x0A: {
        int64_t ns = s64(c->r[0], c->r[1]);
        if (ns <= 0) {
            yield(lk);
        } else {
            Thread *t = current_thread();
            t->wait_id = k().next_wait_id++;
            arm_wait_timeout(t, ns);
            block_current(lk);
        }
        return RESULT_OK;
    }
    case 0x0B: {
        auto t = get<Thread>(c->r[1]);
        if (!t) return RESULT_INVALID_HANDLE;
        c->r[1] = uint32_t(t->priority);
        return RESULT_OK;
    }
    case 0x0C: {
        auto t = get<Thread>(c->r[0]);
        if (!t) return RESULT_INVALID_HANDLE;
        t->priority = int32_t(c->r[1]);
        return RESULT_OK;
    }
    case 0x13: {
        auto m = std::make_shared<Mutex>();
        if (c->r[1]) m->acquire(current_thread());
        c->r[1] = add_handle(m);
        return RESULT_OK;
    }
    case 0x14: {
        auto m = get<Mutex>(c->r[0]);
        if (!m) return RESULT_INVALID_HANDLE;
        if (m->owner != current_thread()) return RESULT_NOT_AUTHORIZED;
        if (--m->lock_count == 0) {
            m->owner = nullptr;
            m->wake_waiters();
        }
        return RESULT_OK;
    }
    case 0x15: {
        auto s = std::make_shared<Semaphore>(int32_t(c->r[1]), int32_t(c->r[2]));
        c->r[1] = add_handle(s);
        return RESULT_OK;
    }
    case 0x16: {
        auto s = get<Semaphore>(c->r[1]);
        if (!s) return RESULT_INVALID_HANDLE;
        c->r[1] = uint32_t(s->count);
        s->count = std::min(s->max, s->count + int32_t(c->r[2]));
        s->wake_waiters();
        return RESULT_OK;
    }
    case 0x17: {
        c->r[1] = add_handle(std::make_shared<Event>(ResetType(c->r[1]), "guest"));
        return RESULT_OK;
    }
    case 0x18: {
        auto e = get<Event>(c->r[0]);
        if (!e) return RESULT_INVALID_HANDLE;
        e->signal();
        return RESULT_OK;
    }
    case 0x19: {
        auto e = get<Event>(c->r[0]);
        if (!e) return RESULT_INVALID_HANDLE;
        e->clear();
        return RESULT_OK;
    }
    case 0x1A: {
        c->r[1] = add_handle(std::make_shared<Timer>(ResetType(c->r[1])));
        return RESULT_OK;
    }
    case 0x1B: {
        auto t = get<Timer>(c->r[0]);
        if (!t) return RESULT_INVALID_HANDLE;
        int64_t initial = s64(c->r[2], c->r[3]), interval = s64(c->r[1], c->r[4]);
        t->interval_ns = uint64_t(std::max<int64_t>(interval, 0));
        uint64_t gen = ++t->generation;
        std::weak_ptr<Timer> weak = t;
        // fires, then re-arms itself every interval
        auto tick = std::make_shared<std::function<void()>>();
        *tick = [weak, gen, tick] {
            auto tm = weak.lock();
            if (!tm || tm->generation != gen) return;
            tm->fire();
            if (tm->interval_ns) schedule_timer(tm->interval_ns, *tick);
        };
        schedule_timer(uint64_t(std::max<int64_t>(initial, 0)), *tick);
        return RESULT_OK;
    }
    case 0x1C: {
        auto t = get<Timer>(c->r[0]);
        if (!t) return RESULT_INVALID_HANDLE;
        t->generation++;
        return RESULT_OK;
    }
    case 0x1D: {
        auto t = get<Timer>(c->r[0]);
        if (!t) return RESULT_INVALID_HANDLE;
        t->signaled = false;
        return RESULT_OK;
    }
    case 0x1E: {
        // CreateMemoryBlock: the game's own memory becomes shareable where it is
        auto sm = std::make_shared<SharedMemory>();
        sm->address = c->r[1];
        sm->size = c->r[2];
        sm->mapped = true;
        sm->name = "guest";
        c->r[1] = add_handle(sm);
        return RESULT_OK;
    }
    case 0x1F: {
        auto sm = get<SharedMemory>(c->r[0]);
        if (!sm) return RESULT_INVALID_HANDLE;
        uint32_t at = c->r[1];
        if (sm->mapped) {
            if (at && at != sm->address)
                fatal("MapMemoryBlock of %s at %08x, but it is already at %08x (aliasing not supported)",
                      sm->name.c_str(), at, sm->address);
        } else {
            // a service's block: place it where the game asks, or pick a spot in the shared region
            if (!at) {
                at = k().next_shared;
                k().next_shared += uint32_t(mem::page_up(sm->size));
            }
            uint32_t bytes = uint32_t(mem::page_up(sm->size));
            mem::commit(at, bytes);
            k().regions[at] = {bytes, c->r[2], 6};
            sm->address = at;
            sm->mapped = true;
        }
        LOG_SVC("MapMemoryBlock %s at %08x+%08x", sm->name.c_str(), sm->address, sm->size);
        return RESULT_OK;
    }
    case 0x20: return RESULT_OK;
    case 0x21: {
        c->r[1] = add_handle(std::make_shared<AddressArbiter>());
        return RESULT_OK;
    }
    case 0x22: return arbitrate(lk, c);
    case 0x23: return close_handle(c->r[0]) ? RESULT_OK : RESULT_INVALID_HANDLE;
    case 0x24: return wait_sync1(lk, c);
    case 0x25: return wait_sync_n(lk, c);
    case 0x27: {
        auto obj = get_handle(c->r[1]);
        if (!obj) return RESULT_INVALID_HANDLE;
        c->r[1] = add_handle(obj);
        return RESULT_OK;
    }
    case 0x28: {
        uint64_t t = ticks();
        c->r[0] = uint32_t(t);
        c->r[1] = uint32_t(t >> 32);
        return c->r[0]; // GetSystemTick returns the tick in r0:r1, not a result
    }
    case 0x2A: { // GetSystemInfo(type, param): zero for anything we don't track
        LOG_SVC("GetSystemInfo type %u param %u", c->r[1], c->r[2]);
        c->r[1] = 0;
        c->r[2] = 0;
        return RESULT_OK;
    }
    case 0x2B: { // GetProcessInfo(process, type)
        uint32_t type = c->r[2];
        LOG_SVC("GetProcessInfo type %u", type);
        uint64_t value = 0; // anything we don't track
        // the linear heap's offset to physical memory: newer SDKs (Yo-kai Watch 3, Blasters)
        // convert addresses for the GPU and DSP with it
        if (type == 20) value = 0x20000000u - mem::LINEAR_BASE;
        c->r[1] = uint32_t(value);
        c->r[2] = uint32_t(value >> 32);
        return RESULT_OK;
    }
    case 0x2D: {
        std::string name = read_cstring(c->r[1], 12);
        auto service = hle::find_port(name);
        if (!service) fatal("ConnectToPort(\"%s\"): unknown port", name.c_str());
        c->r[1] = add_handle(std::make_shared<Session>(service));
        LOG_SVC("ConnectToPort(\"%s\")", name.c_str());
        return RESULT_OK;
    }
    case 0x32: {
        auto s = get<Session>(c->r[0]);
        if (!s) return RESULT_INVALID_HANDLE;
        hle::handle_request(lk, c, *s->service);
        return RESULT_OK;
    }
    case 0x35: c->r[1] = 1; return RESULT_OK; // GetProcessId
    case 0x37: {
        auto t = get<Thread>(c->r[1]);
        if (!t) return RESULT_INVALID_HANDLE;
        c->r[1] = t->id;
        return RESULT_OK;
    }
    case 0x38: {
        c->r[1] = add_handle(std::make_shared<ResourceLimit>());
        return RESULT_OK;
    }
    case 0x39: return resource_limit_values(c, false);
    case 0x3A: return resource_limit_values(c, true);
    case 0x3C: fatal("game called svcBreak(%u) at %08x (lr=%08x)", c->r[0], addr, c->r[14]);
    case 0x3D:
        LOG("[game] %.*s", int(c->r[1]), reinterpret_cast<const char *>(mem::ptr(c->r[0])));
        return RESULT_OK;
    default:
        fatal("unimplemented svc 0x%02x at %08x (r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x)", svc, addr, c->r[0], c->r[1],
              c->r[2], c->r[3], c->r[14]);
    }
}

} // namespace
} // namespace kernel

extern "C" void recomp_svc(Ctx *ctx, uint32_t imm, uint32_t addr) {
    std::unique_lock lk(kernel::k().lock);
    if (auto *t = kernel::current_thread()) {
        t->last_svc = imm;
        t->last_svc_addr = addr;
        t->last_svc_lr = ctx->r[14];
    }
    uint32_t r = kernel::dispatch(lk, ctx, imm, addr);
    ctx->r[0] = r;
    LOG_SVC("svc %02x at %08x -> %08x", imm, addr, r);
    kernel::maybe_preempt(lk);
}
