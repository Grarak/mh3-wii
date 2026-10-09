// Guest OS threads (guest_threads.h). SelectThread and OSExitThread are native
// ports of the SDK's (RMHP08 0x804D4360, 0x804D4860; decompiled with Ghidra):
// they keep the run queues, thread states and switch callback exactly, and
// replace OSSaveContext/OSLoadContext with a baton between host threads. The
// thread switched out keeps its registers on its own host thread and waits
// there until SelectThread picks it again.
#include "guest_threads.h"

#include "abi_bridge.h"
#include "fatal.h"
#include "guest_call.h"
#include "memory.h"

#ifdef MH3_DEVELOPMENT
#include "dev_options.h"
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <unordered_map>
#include <vector>

void PumpDevices(CpuContext* cpu); // os_sleep.cpp

namespace {
using GuestThreads::Clock;

// OS globals.
enum : uint32_t {
    kCurrentContext = 0x800000D4,       // OSContext* OS_CURRENT_CONTEXT
    kActiveHead = 0x800000DC,           // OSThread* the active thread list's head
    kActiveTail = 0x800000E0,           // and tail
    kCurrentThread = 0x800000E4,        // OSThread* __OSCurrentThread
    kRunQueue = 0x8074FE70,             // OSThreadQueue[32]: head, tail per priority
    kIdleContext = 0x8074FF70,          // the context the idle loop runs on
    kSwitchThreadCallback = 0x80796480, // void (*)(OSThread* from, OSThread* to)
    kReschedule = 0x80797850,           // s32: > 0 holds thread switches
    kRunQueueHint = 0x80797854,         // s32: a switch may be due
    kRunQueueBits = 0x80797858,         // bit 31 - p: run queue p is not empty
};

// OSThread (its OSContext comes first).
enum : uint32_t {
    kContextCr = 0x80,
    kContextLr = 0x84,
    kContextCtr = 0x88,
    kContextXer = 0x8C,
    kContextSrr0 = 0x198,
    kContextSrr1 = 0x19C,
    kState = 0x2C8,      // u16
    kAttributes = 0x2CA, // u16: bit 0 detached
    kPriority = 0x2D0,   // s32 effective priority
    kExitValue = 0x2D8,
    kQueue = 0x2DC, // OSThreadQueue* the thread is on
    kLinkNext = 0x2E0,
    kLinkPrev = 0x2E4,
    kJoinQueue = 0x2E8,
    kActiveNext = 0x2FC,
    kActivePrev = 0x300,
};

enum : uint16_t {
    kReady = 1,
    kRunning = 2,
    kWaiting = 4,
    kMoribund = 8
};

enum : uint32_t {
    OSSetCurrentContext = 0x804CD630,
    OSClearContext = 0x804CD870,
    UnlockAllMutex = 0x804D2750, // __OSUnlockAllMutex
    OSSleepThread = 0x804D50B0,
    OSWakeupThread = 0x804D51A0,
};

// A guest thread that ran on a host thread.
struct Record {
    uint32_t thread = 0;       // OSThread
    bool game = false;         // the boot thread, on the game thread
    bool returning = false;    // its entry function returned (OSExitThread follows)
    CpuContext* cpu = nullptr; // its context, current again when it resumes
    CpuContext seed{};         // a new thread's first registers
};

// A sleep (OSSleepTicks, Donate) or a blocking request (BlockUntil): the
// guest thread waits on `queue`, an OSThreadQueue on its stack.
struct Sleeper {
    Clock::time_point deadline;
    uint32_t queue;
    bool donation; // also over when no other thread is runnable
};

struct Waiter {
    std::function<bool()> ready;
    uint32_t queue;
};

std::mutex lock;
std::condition_variable changed;
Record* owner = nullptr; // whose host thread runs guest code
bool idling = false;     // the baton came to the game thread to idle
bool notified = false;   // Notify: a waiter may be ready
std::atomic<bool> started{false};
std::unordered_map<uint32_t, Record*> records; // by OSThread
Record* game = nullptr;
thread_local Record* self = nullptr;

// Only the baton holder touches these.
std::vector<Sleeper> sleepers;
std::vector<Waiter> waiters;

uint32_t Read32(uint32_t address) {
    return Memory::Read32(address);
}

void Write32(uint32_t address, uint32_t value) {
    Memory::Write32(address, value);
}

bool OnGameHost() {
    return !self || self->game;
}

// Calls a guest function, keeping the caller's registers.
uint32_t Call(CpuContext* cpu, uint32_t fn, std::initializer_list<uint32_t> ints) {
    const CpuContext saved = *cpu;
    uint32_t r = 3;

    for (uint32_t v : ints)
        cpu->gpr[r++] = v;

    InvokeIndirectCpu(fn, cpu);
    const uint32_t result = cpu->gpr[3];
    *cpu = saved;
    return result;
}

// cntlzw of the run queue bits: the highest priority with a ready thread, 32
// for none.
uint32_t HighestReady() {
    const uint32_t bits = Read32(kRunQueueBits);
    return bits ? __builtin_clz(bits) : 32;
}

// SetRun: `thread` joins the tail of its priority's run queue.
void Enqueue(uint32_t thread) {
    const uint32_t priority = Read32(thread + kPriority);
    const uint32_t queue = kRunQueue + priority * 8;
    const uint32_t tail = Read32(queue + 4);
    Write32(thread + kQueue, queue);

    if (!tail)
        Write32(queue, thread);
    else
        Write32(tail + kLinkNext, thread);

    Write32(thread + kLinkPrev, tail);
    Write32(thread + kLinkNext, 0);
    Write32(queue + 4, thread);
    Write32(kRunQueueBits, Read32(kRunQueueBits) | (1u << (31 - priority)));
    Write32(kRunQueueHint, 1);
}

// The head of the highest non-empty run queue leaves it and runs.
uint32_t Dequeue() {
    const uint32_t priority = HighestReady();
    const uint32_t queue = kRunQueue + priority * 8;
    const uint32_t thread = Read32(queue);
    const uint32_t next = Read32(thread + kLinkNext);

    if (!next)
        Write32(queue + 4, 0);
    else
        Write32(next + kLinkPrev, 0);

    Write32(queue, next);

    if (!next)
        Write32(kRunQueueBits, Read32(kRunQueueBits) & ~(1u << (31 - priority)));

    Write32(thread + kQueue, 0);
    Memory::Write16(thread + kState, kRunning);
    return thread;
}

void HandTo(Record* target, bool idle) {
#ifdef MH3_DEVELOPMENT
    static const TraceFlag traceThreads("threads");

    if (traceThreads)
        std::fprintf(stderr, "MH3 guest threads: to 0x%08X%s\n", target->thread, idle ? " (idle)" : "");
#endif

    {
        std::lock_guard guard(lock);
        owner = target;
        idling = idle;
    }

    changed.notify_all();
}

// Waits for the baton; true when it came to idle (the game thread only).
// Timed waits: a lost wakeup costs at most a couple of milliseconds.
bool Wait(Record* me) {
    std::unique_lock guard(lock);

    while (owner != me)
        changed.wait_for(guard, std::chrono::milliseconds(2));

    const bool idle = idling;
    idling = false;
    return idle;
}

// Wakes the sleepers whose time is over and the waiters that are ready (and,
// when `idle`, the donations), with thread switches held as in an interrupt.
void Wake(CpuContext* cpu, bool idle) {
    if (sleepers.empty() && waiters.empty())
        return;

    const auto now = Clock::now();
    std::vector<uint32_t> queues;

    for (auto it = sleepers.begin(); it != sleepers.end();) {
        if (it->deadline <= now || (idle && it->donation)) {
            queues.push_back(it->queue);
            it = sleepers.erase(it);
        } else
            ++it;
    }

    for (auto it = waiters.begin(); it != waiters.end();) {
        if (it->ready()) {
            queues.push_back(it->queue);
            it = waiters.erase(it);
        } else
            ++it;
    }

    for (uint32_t queue : queues) {
        Write32(kReschedule, Read32(kReschedule) + 1);
        Call(cpu, OSWakeupThread, {queue});
        Write32(kReschedule, Read32(kReschedule) - 1);
    }
}

// The SDK's idle loop, on the game thread: device work until a thread can run.
void Idle(CpuContext* cpu) {
    while (!Read32(kRunQueueBits)) {
        Wake(cpu, true);

        if (Read32(kRunQueueBits))
            break;

        const CpuContext saved = *cpu;
        PumpDevices(cpu);
        *cpu = saved;

        if (Read32(kRunQueueBits))
            break;

        auto until = Clock::now() + std::chrono::milliseconds(1);

        for (const auto& sleeper : sleepers)
            until = std::min(until, sleeper.deadline);

        std::unique_lock guard(lock);
        changed.wait_until(guard, until, [] { return notified; });
        notified = false;
    }
}

void* HostMain(void* argument);

// The record of the guest thread `thread`; a thread that never ran gets a host
// thread, which waits for the baton.
Record* RecordFor(uint32_t thread, const CpuContext& seed) {
    if (const auto it = records.find(thread); it != records.end())
        return it->second;

    auto* record = new Record;
    record->thread = thread;
    record->seed = seed; // special registers, small-data bases, FPSCR, GQRs

    // The registers OSCreateThread's OSInitContext left in its context.
    for (unsigned i = 0; i < 32; ++i)
        record->seed.gpr[i] = Read32(thread + i * 4);

    record->seed.cr = Read32(thread + kContextCr);
    record->seed.lr = Read32(thread + kContextLr);
    record->seed.ctr = Read32(thread + kContextCtr);
    record->seed.xer = Read32(thread + kContextXer);
    record->seed.pc = Read32(thread + kContextSrr0);
    record->seed.msr = Read32(thread + kContextSrr1);
    records[thread] = record;
    started = true;

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 1u << 20);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t host;
    const int failed = pthread_create(&host, &attributes, HostMain, record);
    pthread_attr_destroy(&attributes);

    if (failed)
        MH3_THROW(std::runtime_error("Cannot start a host thread for a guest thread"));

    std::fprintf(stderr, "MH3 guest threads: thread 0x%08X starts at 0x%08X (priority %d)\n", thread, record->seed.pc, int32_t(Read32(thread + kPriority)));
    return record;
}

// OSLoadContext: the selected thread runs on, and `me` waits until it is
// selected again. An exiting thread hands over and returns to end its host.
void Resume(CpuContext* cpu, Record* me, uint32_t next);

// The rest of SelectThread once a thread is runnable: dequeue the highest,
// switch callback, current thread and context.
uint32_t RunNext(CpuContext* cpu) {
    Write32(kRunQueueHint, 0);
    const uint32_t next = Dequeue();
    Call(cpu, Read32(kSwitchThreadCallback), {Read32(kCurrentThread), next});
    Write32(kCurrentThread, next);
    Call(cpu, OSSetCurrentContext, {next});
    return next;
}

// Waits for the baton; on the game thread an idle hand-over runs the idle loop
// and the selection after it, until its own guest thread is selected.
void Await(CpuContext* cpu, Record* me) {
    for (;;) {
        const bool idle = Wait(me);
        PpcThread::state[PpcThread::Game].context = me->cpu;

        if (!idle)
            return;

        Idle(cpu);
        Call(cpu, OSClearContext, {kIdleContext});
        const uint32_t next = RunNext(cpu);
        Record* target = RecordFor(next, *cpu);

        if (target == me)
            return;

        me->cpu = PpcThread::state[PpcThread::Game].context;
        HandTo(target, false);
    }
}

// Hands the baton to `target` (to idle: the game thread) and waits until `me`
// is selected again; an exiting thread's host thread returns to end instead.
void Leave(CpuContext* cpu, Record* me, Record* target, bool idle) {
    me->cpu = PpcThread::state[PpcThread::Game].context;

    if (me->returning) {
        records.erase(me->thread);
        HandTo(target, idle);
        return;
    }

    HandTo(target, idle);
    Await(cpu, me);
}

void Resume(CpuContext* cpu, Record* me, uint32_t next) {
    Record* target = RecordFor(next, *cpu);

    if (target != me)
        Leave(cpu, me, target, false);
}

// SelectThread(yield).
void Select(CpuContext* cpu, bool yield) {
    // Guest code on the worker or the draw thread never switches threads.
    if (PpcThread::CurrentThread() != PpcThread::Game)
        return;

    if (int32_t(Read32(kReschedule)) > 0)
        return;

    const uint32_t current = Read32(kCurrentThread);

    // OSGetCurrentContext: an interrupt callback's own context switches nothing.
    if (Read32(kCurrentContext) != current)
        return;

    if (!self && current) {
        // The boot thread's first selection: the game thread runs it.
        game = new Record;
        game->thread = current;
        game->game = true;
        records[current] = game;
        self = game;
        std::lock_guard guard(lock);
        owner = game;
    }

    // A timer interrupt between the other threads' switches: sleeps that are
    // over end (the game thread checks at every device pump).
    if (!OnGameHost())
        Wake(cpu, false);

    if (current) {
        if (Memory::Read16(current + kState) == kRunning) {
            if (!yield && HighestReady() >= Read32(current + kPriority))
                return;

            Memory::Write16(current + kState, kReady);
            Enqueue(current);
        }

        // OSSaveContext: the host thread keeps the registers.
    }

    if (!Read32(kRunQueueBits)) {
        Call(cpu, Read32(kSwitchThreadCallback), {current, 0});
        Write32(kCurrentThread, 0);
        Call(cpu, OSSetCurrentContext, {kIdleContext});

        if (!OnGameHost()) {
            // The game thread does device work: it idles and selects.
            Leave(cpu, self, game, true);
            return;
        }

        Idle(cpu);
        Call(cpu, OSClearContext, {kIdleContext});
    }

    const uint32_t next = RunNext(cpu);

    if (self)
        Resume(cpu, self, next);
}

// Puts the current thread to sleep on a queue of its own until Wake wakes it.
template <class Register>
void SleepOnQueue(CpuContext* cpu, Register enlist) {
    GuestFrame frame(cpu, 32);
    const uint32_t queue = frame[8];
    Write32(queue, 0);
    Write32(queue + 4, 0);
    enlist(queue);
    cpu->gpr[3] = queue;
    InvokeIndirectCpu(OSSleepThread, cpu);
}

// Whether the current thread can switch: not held, and not in an interrupt
// callback's context.
bool CanSwitch() {
    const uint32_t current = Read32(kCurrentThread);
    return current && int32_t(Read32(kReschedule)) <= 0 && Read32(kCurrentContext) == current && PpcThread::CurrentThread() == PpcThread::Game;
}

void* HostMain(void* argument) {
    auto* record = static_cast<Record*>(argument);
    self = record;
    Wait(record);
    CpuContext cpu = record->seed;
    record->cpu = &cpu;
    PpcThread::state[PpcThread::Game].context = &cpu;
    MkwApplyHostNiMode(cpu.fpscr);
#if defined(__cpp_exceptions)
    try {
#endif
        InvokeIndirectCpu(record->seed.pc, &cpu);

        // The entry returned to the link register OSCreateThread set: OSExitThread.
        record->returning = true;
        InvokeIndirectCpu(Read32(record->thread + kContextLr), &cpu);
#if defined(__cpp_exceptions)
    } catch (const std::exception& error) {
        MH3Fatal(error.what());
    }
#endif
    delete record;
    return nullptr;
}
} // namespace

namespace GuestThreads {
bool Started() {
    return started.load(std::memory_order_relaxed);
}

void Service(CpuContext* cpu) {
    if (!OnGameHost() || !Started())
        return;

    Wake(cpu, false);

    if (Read32(kRunQueueHint) && CanSwitch()) {
        GuestFrame frame(cpu, 16);
        Select(cpu, false);
    }
}

void Donate(CpuContext* cpu, std::chrono::nanoseconds budget) {
    if (!Read32(kRunQueueBits) || !CanSwitch())
        return;

    const auto deadline = Clock::now() + budget;
    SleepOnQueue(cpu, [&](uint32_t queue) { sleepers.push_back({deadline, queue, true}); });
}

bool SleepUntil(CpuContext* cpu, Clock::time_point deadline) {
    if (!CanSwitch())
        return false;

    SleepOnQueue(cpu, [&](uint32_t queue) { sleepers.push_back({deadline, queue, false}); });
    return true;
}

void BlockUntil(CpuContext* cpu, const std::function<bool()>& ready) {
    if (ready())
        return;

    if (CanSwitch()) {
        SleepOnQueue(cpu, [&](uint32_t queue) { waiters.push_back({ready, queue}); });
        return;
    }

    while (!ready()) {
        if (OnGameHost()) {
            const CpuContext saved = *cpu;
            PumpDevices(cpu);
            *cpu = saved;
        }

        std::unique_lock guard(lock);
        changed.wait_for(guard, std::chrono::milliseconds(1), [] { return notified; });
        notified = false;
    }
}

void Notify() {
    {
        std::lock_guard guard(lock);
        notified = true;
    }

    changed.notify_all();
}

bool OnGameThread() {
    return OnGameHost();
}
} // namespace GuestThreads

extern "C" void MH3_SelectThread(CpuContext* cpu) {
    {
        GuestFrame frame(cpu, 16);
        Select(cpu, frame.Arg(3) != 0);
    }

    cpu->gpr[3] = 0;
}
REGISTER_NATIVE_FUNCTION_AS(0x804D4360, MH3_SelectThread, "SelectThread");

// OSExitThread(val).
extern "C" void MH3_OSExitThread(CpuContext* cpu) {
    GuestFrame frame(cpu, 16);
    const uint32_t msr = cpu->msr;
    cpu->msr &= ~0x8000u; // OSDisableInterrupts
    const uint32_t current = Read32(kCurrentThread);
    Call(cpu, OSClearContext, {current});

    if (!(Memory::Read16(current + kAttributes) & 1)) {
        Memory::Write16(current + kState, kMoribund);
        Write32(current + kExitValue, frame.Arg(3));
    } else {
        // Detached: it leaves the active thread list.
        const uint32_t next = Read32(current + kActiveNext), previous = Read32(current + kActivePrev);

        if (next)
            Write32(next + kActivePrev, previous);
        else
            Write32(kActiveTail, previous);

        if (previous)
            Write32(previous + kActiveNext, next);
        else
            Write32(kActiveHead, next);

        Memory::Write16(current + kState, 0);
    }

    Call(cpu, UnlockAllMutex, {current});
    Call(cpu, OSWakeupThread, {current + kJoinQueue});
    Write32(kRunQueueHint, 1);

    // A thread that called OSExitThread itself never resumes: its host thread
    // waits for good. One whose entry returned ends its host thread.
    Select(cpu, false);
    cpu->msr = msr;
}
REGISTER_NATIVE_FUNCTION_AS(0x804D4860, MH3_OSExitThread, "OSExitThread");

// OSCreateThread (translated) may reuse an OSThread whose host thread waits
// for good (a thread that exited itself or was canceled): the new thread gets
// a host thread of its own.
extern "C" void func_804D45F0(CpuContext*);
extern "C" void MH3_OSCreateThread(CpuContext* cpu) {
    const uint32_t thread = cpu->gpr[3];
    func_804D45F0(cpu);

    if (cpu->gpr[3])
        if (const auto it = records.find(thread); it != records.end() && !it->second->game)
            records.erase(it); // its old host thread is never selected again
}
REGISTER_NATIVE_FUNCTION_AS(0x804D45F0, MH3_OSCreateThread, "MH3_OSCreateThread");
