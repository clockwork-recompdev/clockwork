#include "kernel.h"

#include "game_code.h"
#include "log.h"
#include "memory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>

namespace kernel {

namespace {
struct Process : Object {
    const char *type_name() const override { return "Process"; }
};

const auto start_time = std::chrono::steady_clock::now();

RecompFn lookup(uint32_t addr) {
    addr &= ~1u;
    const RecompEntry *begin = game_code::functions(), *end = begin + game_code::function_count();
    auto it = std::lower_bound(begin, end, addr,
                               [](const RecompEntry &e, uint32_t a) { return e.addr < a; });
    return it != end && it->addr == addr ? it->fn : nullptr;
}

Thread *pick_next() {
    Thread *best = nullptr;
    for (auto &t : k().threads)
        if (t->state == ThreadState::Ready && (!best || t->priority < best->priority)) best = t.get();
    return best;
}

void switch_to(Thread *next) {
    k().current = next;
    if (next) {
        next->state = ThreadState::Running;
        next->cv.notify_one();
    }
}

void wait_until_current(Thread *t, std::unique_lock<std::mutex> &lk) {
    t->cv.wait(lk, [t] { return k().current == t; });
}

// moves t to the back of the thread list so equal-priority threads take turns
void rotate_to_back(Thread *t) {
    auto &ts = k().threads;
    auto it = std::find_if(ts.begin(), ts.end(), [t](auto &p) { return p.get() == t; });
    if (it != ts.end()) std::rotate(it, it + 1, ts.end());
}

void timer_loop() {
    std::unique_lock lk(k().lock);
    for (;;) {
        auto &timers = k().timers;
        if (timers.empty()) {
            k().timer_cv.wait(lk);
            continue;
        }
        uint64_t now = ticks(), due = timers.begin()->first;
        if (due > now) {
            auto wait = std::chrono::nanoseconds(std::min<uint64_t>(ticks_to_ns(due - now), 1000000000ull));
            k().timer_cv.wait_for(lk, wait);
            continue;
        }
        auto fn = std::move(timers.begin()->second);
        timers.erase(timers.begin());
        fn();
    }
}

void thread_main(std::shared_ptr<Thread> t) {
    std::unique_lock lk(k().lock);
    wait_until_current(t.get(), lk);
    RecompFn fn = lookup(t->entry);
    if (!fn) fatal("thread entry %08x was not recompiled", t->entry);
    lk.unlock();
    fn(&t->ctx);
    // returning from the entry function ends the thread
    lk.lock();
    exit_current_thread(lk);
}
} // namespace

Kernel &k() {
    static Kernel kernel;
    return kernel;
}

uint64_t ticks() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_time);
    return ns_to_ticks(uint64_t(ns.count()));
}

// ---- wait objects ----

void WaitObject::wake_waiters() {
    // highest priority first; copy since waking edits the lists
    std::vector<Thread *> ws = waiters;
    std::stable_sort(ws.begin(), ws.end(), [](Thread *a, Thread *b) { return a->priority < b->priority; });
    for (Thread *t : ws) {
        if (t->state != ThreadState::Waiting) continue;
        auto &objs = t->wait_objects;
        if (t->wait_all) {
            if (std::any_of(objs.begin(), objs.end(), [t](auto &o) { return o->should_wait(t); })) continue;
            for (auto &o : objs) o->acquire(t);
            t->wait_index = 0;
        } else {
            auto it = std::find_if(objs.begin(), objs.end(), [t](auto &o) { return !o->should_wait(t); });
            if (it == objs.end()) continue;
            (*it)->acquire(t);
            t->wait_index = int32_t(it - objs.begin());
        }
        for (auto &o : objs) std::erase(o->waiters, t);
        objs.clear();
        t->wait_result = RESULT_OK;
        make_ready(t);
    }
}

void Mutex::acquire(Thread *t) {
    if (owner != t) {
        std::erase_if(t->held, [t](const std::weak_ptr<Mutex> &w) {
            auto m = w.lock();
            return !m || m->owner != t; // released (or gone) since
        });
        t->held.push_back(std::static_pointer_cast<Mutex>(shared_from_this()));
    }
    owner = t;
    lock_count++;
}

void Event::signal() {
    signaled = true;
    wake_waiters();
    if (reset == ResetType::Pulse) signaled = false;
}

void Timer::fire() {
    signaled = true;
    wake_waiters();
    if (reset == ResetType::Pulse) signaled = false;
}

// ---- handles ----

Handle add_handle(std::shared_ptr<Object> obj) {
    Handle h = k().next_handle++;
    k().handles[h] = std::move(obj);
    return h;
}

std::shared_ptr<Object> get_handle(Handle h) {
    if (h == CURRENT_THREAD) {
        for (auto &t : k().threads)
            if (t.get() == k().current) return t;
        return nullptr;
    }
    if (h == CURRENT_PROCESS) {
        static auto process = std::make_shared<Process>();
        return process;
    }
    auto it = k().handles.find(h);
    return it == k().handles.end() ? nullptr : it->second;
}

bool close_handle(Handle h) { return k().handles.erase(h) > 0; }

// ---- scheduling ----

Thread *current_thread() { return k().current; }

void make_ready(Thread *t) {
    t->state = ThreadState::Ready;
    t->arbiter = nullptr;
    t->arbiter_addr = 0;
    if (!k().current) switch_to(pick_next());
}

void block_current(std::unique_lock<std::mutex> &lk) {
    Thread *cur = k().current;
    cur->state = ThreadState::Waiting;
    switch_to(pick_next());
    wait_until_current(cur, lk);
}

void release_cpu() {
    Thread *cur = k().current;
    if (!cur) return;
    cur->state = ThreadState::Waiting; // on no wait object: only reacquire_cpu makes it ready
    cur->wait_id = k().next_wait_id++; // so the timeout of an earlier wait that ended early can't wake it
    switch_to(pick_next());
}

void reacquire_cpu(std::unique_lock<std::mutex> &lk, Thread *t) {
    t->state = ThreadState::Ready;
    if (!k().current) switch_to(pick_next());
    // otherwise the running thread hands over at its next SVC if `t` has priority
    wait_until_current(t, lk);
}

void maybe_preempt(std::unique_lock<std::mutex> &lk) {
    Thread *cur = k().current;
    Thread *best = pick_next();
    if (!cur || !best || best->priority >= cur->priority) return;
    cur->state = ThreadState::Ready;
    switch_to(best);
    wait_until_current(cur, lk);
}

void yield(std::unique_lock<std::mutex> &lk) {
    Thread *cur = k().current;
    cur->state = ThreadState::Ready;
    rotate_to_back(cur);
    Thread *next = pick_next();
    if (next == cur) {
        cur->state = ThreadState::Running;
        return;
    }
    switch_to(next);
    wait_until_current(cur, lk);
}

void schedule_timer(uint64_t ns, std::function<void()> fn) {
    uint64_t due = ticks() + ns_to_ticks(ns);
    k().timers.emplace(due, std::move(fn));
    k().timer_cv.notify_one();
}

void schedule_periodic(uint64_t ns, std::function<void()> fn) {
    struct Periodic {
        uint64_t period, due;
        std::function<void()> fn;
    };
    uint64_t period = ns_to_ticks(ns);
    auto p = std::make_shared<Periodic>(Periodic{period, ticks() + period, std::move(fn)});
    auto tick = std::make_shared<std::function<void()>>();
    *tick = [p, tick] {
        p->fn();
        p->due += p->period;
        uint64_t now = ticks();
        if (now > p->due + 8 * p->period) p->due = now + p->period;
        k().timers.emplace(p->due, *tick);
    };
    k().timers.emplace(p->due, *tick);
    k().timer_cv.notify_one();
}

void arm_wait_timeout(Thread *t, int64_t ns) {
    if (ns < 0) return; // wait forever
    uint64_t id = t->wait_id;
    schedule_timer(uint64_t(ns), [t, id] {
        if (t->state != ThreadState::Waiting || t->wait_id != id) return;
        for (auto &o : t->wait_objects) std::erase(o->waiters, t);
        t->wait_objects.clear();
        t->wait_result = RESULT_TIMEOUT;
        make_ready(t);
    });
}

void dump_threads() {
    // best effort: a hung thread may hold the lock
    std::unique_lock lk(k().lock, std::try_to_lock);
    std::fprintf(stderr, "--- guest threads%s ---\n", lk.owns_lock() ? "" : " (kernel lock busy; may be inconsistent)");
    static const char *states[] = {"ready", "running", "waiting", "dead"};
    for (auto &t : k().threads) {
        std::fprintf(stderr, "thread %u prio %#x %s%s: last svc %#x at %08x (lr %08x)", t->id, t->priority,
                     states[int(t->state)], t.get() == k().current ? " [current]" : "", t->last_svc,
                     t->last_svc_addr, t->last_svc_lr);
        for (auto &o : t->wait_objects) {
            auto *e = dynamic_cast<Event *>(o.get());
            std::fprintf(stderr, " | waits on %s%s%s", o->type_name(), e ? " " : "", e ? e->name.c_str() : "");
            if (auto *m = dynamic_cast<Mutex *>(o.get()); m && m->owner)
                std::fprintf(stderr, " held by thread %u%s", m->owner->id, m->owner->state == ThreadState::Dead ? " (dead)" : "");
        }
        if (t->arbiter) std::fprintf(stderr, " | arbiter address %08x", t->arbiter_addr);
        std::fputc('\n', stderr);
    }
}

// ---- threads ----

std::shared_ptr<Thread> create_thread(uint32_t entry, uint32_t arg, uint32_t stack_top, int32_t priority) {
    auto t = std::make_shared<Thread>();
    t->id = k().next_thread_id++;
    t->priority = priority;
    t->entry = entry;
    t->ctx.r[0] = arg;
    t->ctx.r[13] = stack_top;
    t->ctx.r[15] = entry;
    t->ctx.tls = mem::TLS_BASE + (t->id - 1) * 0x200;
    if (!mem::is_committed(t->ctx.tls)) mem::commit(mem::page_down(t->ctx.tls), mem::PAGE);
    t->state = ThreadState::Ready;
    k().threads.push_back(t);
    if (entry) {
        t->host = std::thread(thread_main, t);
        t->host.detach();
    }
    return t;
}

void exit_current_thread(std::unique_lock<std::mutex> &lk) {
    Thread *cur = k().current;
    cur->state = ThreadState::Dead;
    LOG_SVC("thread %u exited", cur->id);
    for (auto &w : cur->held)
        if (auto m = w.lock(); m && m->owner == cur) {
            LOG_SVC("thread %u exited holding a mutex: released", cur->id);
            m->owner = nullptr, m->lock_count = 0;
            m->wake_waiters();
        }
    cur->held.clear();
    cur->wake_waiters();
    switch_to(pick_next());
    // the host thread parks forever; the guest stack frames above it are abandoned
    for (;;) cur->cv.wait(lk);
}

// ---- process ----

void init(const GameImage &game) {
    uint32_t stack = uint32_t(mem::page_up(game.stack_size));
    mem::commit(mem::STACK_TOP - stack, stack);
    k().regions[mem::STACK_TOP - stack] = {stack, 3, 0x0B}; // RW, Locked (stack)
    k().regions[mem::CODE_BASE] = {uint32_t(mem::page_up(game.data_end + game.bss_size)) - mem::CODE_BASE, 7, 0x09};
    // VRAM, which apps with GPU rights access directly
    mem::commit(mem::VRAM_BASE, mem::VRAM_SIZE);
    k().regions[mem::VRAM_BASE] = {mem::VRAM_SIZE, 3, 0x03};
    // config page, shared page and the first TLS page
    mem::commit(mem::CONFIG_PAGE, 3 * mem::PAGE);
    mem::write32(mem::CONFIG_PAGE + 0x40, 0x04000000); // APPMEMALLOC: 64 MiB application region
    mem::write32(mem::SHARED_PAGE + 0x04, 1);          // RUNNING_HW: product
    // WIFI_MACADDR: local wireless (Pia) won't create a network without one. Locally administered,
    // and random per run, so players sharing a room don't collide
    {
        std::random_device rd;
        uint8_t *mac = mem::ptr(mem::SHARED_PAGE + 0x60);
        for (int i = 0; i < 6; i++) mac[i] = uint8_t(rd());
        mac[0] = uint8_t((mac[0] & 0xFC) | 0x02); // unicast, locally administered
    }
    k().timer_thread = std::thread(timer_loop);
    k().timer_thread.detach();
}

void run(const GameImage &game) {
    std::unique_lock lk(k().lock);
    auto main = create_thread(0, 0, mem::STACK_TOP, game.main_priority);
    add_handle(main);
    switch_to(main.get());
    RecompFn fn = lookup(game.entry);
    if (!fn) fatal("entry point %08x was not recompiled", game.entry);
    LOG("starting main thread at %08x", game.entry);
    lk.unlock();
    fn(&main->ctx);
    LOG("main thread returned from its entry point");
    std::fflush(nullptr);
    std::_Exit(0);
}

} // namespace kernel

extern "C" {

void recomp_call_indirect(Ctx *ctx, uint32_t target) {
    RecompFn fn = kernel::lookup(target);
    if (!fn)
        fatal("jump to %08x, which was not recompiled (lr=%08x). Add it as a manual entry point.", target,
              ctx->r[14]);
    fn(ctx);
}

void recomp_unhandled(Ctx *ctx, uint32_t addr, const char *what) {
    fatal("unhandled instruction at %08x: %s (lr=%08x)", addr, what, ctx->r[14]);
}

void recomp_coproc(Ctx *ctx, uint32_t addr, uint32_t raw) {
    fatal("unhandled coprocessor access at %08x: %08x (lr=%08x)", addr, raw, ctx->r[14]);
}

} // extern "C"
