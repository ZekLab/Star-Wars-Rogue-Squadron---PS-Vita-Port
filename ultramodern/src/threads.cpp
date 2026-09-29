#include <cstdio>
#include <thread>
#include <cassert>
#include <string>
#if defined(__GLIBC__)
#include <cxxabi.h>
#endif

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "blockingconcurrentqueue.h"

#include "ultramodern/threads.hpp"

// Native APIs only used to set thread names for easier debugging
#ifdef _WIN32
#include <Windows.h>
#include <process.h>   // _endthreadex
#endif

static ultramodern::threads::callbacks_t threads_callbacks;

void ultramodern::threads::set_callbacks(const callbacks_t& callbacks) {
    threads_callbacks = callbacks;
}

std::string ultramodern::threads::get_game_thread_name(const OSThread* t) {
    if (threads_callbacks.get_game_thread_name == nullptr) {
        return "Game Thread " + std::to_string(t->id);
    }
    return threads_callbacks.get_game_thread_name(t);
}

extern "C" void bootproc();

thread_local bool is_main_thread = false;
// Whether this thread is part of the game (i.e. the start thread or one spawned by osCreateThread)
thread_local bool is_game_thread = false;
thread_local PTR(OSThread) thread_self = NULLPTR;

void ultramodern::set_main_thread() {
    ::is_game_thread = true;
    is_main_thread = true;
}

bool ultramodern::is_game_thread() {
    return ::is_game_thread;
}

#if 0
int main(int argc, char** argv) {
    ultramodern::set_main_thread();

    bootproc();
}
#endif

#if 1
void run_thread_function(uint8_t* rdram, uint64_t addr, uint64_t sp, uint64_t arg);
#else
#define run_thread_function(func, sp, arg) func(arg)
#endif

#if defined(_WIN32)
namespace {
    // Cached env-var read for thread-name registration logging. The [NAME]
    // lines were added during a Win32 thread-naming crash investigation
    // (wstring construction failing, SetThreadDescription COM init issues)
    // and stay around for future debugging. Default off — they emit ~3
    // lines per thread × ~20 threads at startup. ROGUESQ_LOG_THREADS=1
    // (or ROGUESQ_LOG_ALL=1) to enable.
    bool log_threads_native() {
        static const bool v = []{
            const char *a = std::getenv("ROGUESQ_LOG_ALL");
            if (a && *a && *a != '0') return true;
            const char *e = std::getenv("ROGUESQ_LOG_THREADS");
            return e && *e && *e != '0';
        }();
        return v;
    }
}

void ultramodern::set_native_thread_name(const std::string& name) {
    if (log_threads_native()) {
        printf("[NAME] set_native_thread_name('%s') begin\n", name.c_str()); fflush(stdout);
    }
    std::wstring wname{name.begin(), name.end()};
    if (log_threads_native()) {
        printf("[NAME] wstring constructed ok, len=%zu\n", wname.size()); fflush(stdout);
    }

    HRESULT r;
    r = SetThreadDescription(
        GetCurrentThread(),
        wname.c_str()
    );
    if (log_threads_native()) {
        printf("[NAME] SetThreadDescription done (hr=0x%08X)\n", (unsigned)r); fflush(stdout);
    }
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    int nPriority = THREAD_PRIORITY_NORMAL;

    // Convert ThreadPriority to Win32 priority
    switch (pri) {
        case ThreadPriority::Low:
            nPriority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case ThreadPriority::Normal:
            nPriority = THREAD_PRIORITY_NORMAL;
            break;
        case ThreadPriority::High:
            nPriority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case ThreadPriority::VeryHigh:
            nPriority = THREAD_PRIORITY_HIGHEST;
            break;
        case ThreadPriority::Critical:
            nPriority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        default:
            throw std::runtime_error("Invalid thread priority!");
            break;
    }
    // SetThreadPriority(GetCurrentThread(), nPriority);
}
#elif defined(__linux__)
#include <sys/prctl.h>

void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Linux only accepts up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    prctl(PR_SET_NAME, name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    // TODO linux thread priority
    // printf("set_native_thread_priority unimplemented\n");
    // int nPriority = THREAD_PRIORITY_NORMAL;

    // // Convert ThreadPriority to Win32 priority
    // switch (pri) {
    //     case ThreadPriority::Low:
    //         nPriority = THREAD_PRIORITY_BELOW_NORMAL;
    //         break;
    //     case ThreadPriority::Normal:
    //         nPriority = THREAD_PRIORITY_NORMAL;
    //         break;
    //     case ThreadPriority::High:
    //         nPriority = THREAD_PRIORITY_ABOVE_NORMAL;
    //         break;
    //     case ThreadPriority::VeryHigh:
    //         nPriority = THREAD_PRIORITY_HIGHEST;
    //         break;
    //     case ThreadPriority::Critical:
    //         nPriority = THREAD_PRIORITY_TIME_CRITICAL;
    //         break;
    //     default:
    //         throw std::runtime_error("Invalid thread priority!");
    //         break;
    // }
}
#elif defined(__APPLE__)
void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Macs seem to only accept up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    pthread_setname_np(name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {}
#elif defined(__vita__)
#include <cstdlib>
#include <mutex>
#include <vector>
#include <psp2/kernel/threadmgr.h>

// Vita threads can't be renamed after creation, so names are kept in a registry the host uses for per-thread CPU stats.
static std::mutex vita_thread_names_mutex;
static std::vector<std::pair<int, std::string>> vita_thread_names;

void ultramodern::set_native_thread_name(const std::string& name) {
    std::lock_guard lock{ vita_thread_names_mutex };
    vita_thread_names.emplace_back(sceKernelGetThreadId(), name);
}

std::vector<std::pair<int, std::string>> ultramodern::vita_named_threads() {
    std::lock_guard lock{ vita_thread_names_mutex };
    return vita_thread_names;
}

// Vita user priorities run from 64 (highest) to 191; threads start near 160. With the display-list parser keeping a core busy for
// tens of milliseconds, equal priorities let it delay the VI, timer, audio and game threads that share its cores.
// ROGUESQ_VITA_THREAD_PRIORITIES=0 leaves every thread at the default.
void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    static const bool enabled = []() {
        const char* e = std::getenv("ROGUESQ_VITA_THREAD_PRIORITIES");
        return !(e && (e[0] == '0'));
    }();
    if (!enabled) {
        return;
    }
    int priority = 160;
    switch (pri) {
        case ThreadPriority::Low: priority = 170; break;
        case ThreadPriority::Normal: priority = 160; break;
        case ThreadPriority::High: priority = 150; break;
        case ThreadPriority::VeryHigh: priority = 140; break;
        case ThreadPriority::Critical: priority = 130; break;
    }
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), priority);
}
#endif

[[noreturn]] static void end_deleted_thread(UltraThreadContext* cur_context);

void wait_for_resumed(RDRAM_ARG UltraThreadContext* thread_context) {
    if (thread_context == nullptr) {
        printf("[CRASH] wait_for_resumed called with NULL context! thread_self=0x%08X\n", (uint32_t)ultramodern::this_thread()); fflush(stdout);
        std::abort();
    }
    thread_context->running.wait();
    // If this thread's context was replaced/deleted by another thread destroying
    // us, terminate this host thread directly. The old path called
    // osDestroyThread(NULL), which throws thread_terminated to unwind out to
    // _thread_func's catch; in optimized Release that unwind fails to traverse the
    // recompiled frames and escapes to std::terminate (the medal / level-complete
    // abort). The destroying thread already rescheduled, so no run_next_thread is
    // owed here — just clean up and exit, matching _thread_func's deleted tail.
    if (TO_PTR(OSThread, ultramodern::this_thread())->context != thread_context) {
        end_deleted_thread(thread_context);
    }
}

// Validate t->context isn't a torn/corrupted pointer. The OSThread struct
// lives in MIPS RDRAM, but its context field is a host pointer — recompiled
// game code that scribbles near that field corrupts the host pointer. The
// sentinel at offset 0 in UltraThreadContext lets us catch garbage values
// before dereferencing inside signal(). Returns true if context looks live.
//
// On Windows we wrap the magic load in SEH so an unmapped pointer surfaces
// as a soft skip rather than an access violation.
#ifdef _WIN32
static bool context_magic_ok_seh(UltraThreadContext* ctx) {
    // SEH may not catch faults from non-canonical pointers (e.g. 0xFFFFFFFFFFFFFFFF
    // is in the kernel-space high half and can bypass user-mode SEH). Reject
    // obvious junk values up-front.
    uintptr_t u = (uintptr_t)ctx;
    if (u < 0x10000ULL || u >= 0x800000000000ULL) {
        return false;
    }
    // PATCH (2026-05-08): SEH __try has been observed to NOT catch some user-mode
    // AVs (e.g. address 0x1FF4DB80B0 — within bounds but not committed). Probe
    // the page state with VirtualQuery before dereferencing.
    MEMORY_BASIC_INFORMATION mbi{};
    SIZE_T qres = VirtualQuery(reinterpret_cast<LPCVOID>(ctx), &mbi, sizeof(mbi));
    if (qres == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ
        | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & kReadable) == 0) {
        return false;
    }
    // Also ensure the magic field doesn't span past the end of the committed region.
    uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t end = base + static_cast<uintptr_t>(mbi.RegionSize);
    if (u + sizeof(uint64_t) > end) {
        return false;
    }
    __try {
        return ctx->magic == UltraThreadContext::kMagic;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

static bool context_looks_live(const char* who, OSThread* t) {
    if (t->context == nullptr) {
        return false;
    }
#ifdef _WIN32
    bool ok = context_magic_ok_seh(t->context);
#else
    bool ok = (t->context->magic == UltraThreadContext::kMagic);
#endif
    if (!ok) {
        static std::atomic<uint64_t> n{0};
        uint64_t i = ++n;
        if (i == 1 || (i & (i - 1)) == 0) {
            fprintf(stderr, "[ultramodern] %s: thread %d has corrupt/unmapped context %p (#%llu) — skipping signal\n",
                who, t->id, (void*)t->context, (unsigned long long)i);
            fflush(stderr);
        }
        return false;
    }
    return true;
}

void resume_thread(OSThread* t) {
    debug_printf("[Thread] Resuming execution of thread %d\n", t->id);
    if (!context_looks_live("resume_thread", t)) {
        return;
    }
    t->context->running.signal();
}

void run_next_thread(RDRAM_ARG1) {
    if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
        throw std::runtime_error("No threads left to run!\n");
    }

    OSThread* to_run = TO_PTR(OSThread, ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue));
    debug_printf("[Scheduling] Resuming execution of thread %d\n", to_run->id);
    to_run->context->running.signal();
}

// Enqueue this thread's context for the cleaner (which joins this host thread)
// and exit it, matching _thread_func's deleted-context tail. See wait_for_resumed.
[[noreturn]] static void end_deleted_thread(UltraThreadContext* cur_context) {
    ultramodern::cleanup_thread(cur_context);
#ifdef _WIN32
    _endthreadex(0);
#else
    pthread_exit(nullptr);
#endif
    std::abort(); // unreachable
}

void ultramodern::run_next_thread_and_wait(RDRAM_ARG1) {
    UltraThreadContext* cur_context = TO_PTR(OSThread, thread_self)->context;
    if (cur_context == nullptr) {
        printf("[CRASH] run_next_thread_and_wait: cur thread 0x%08X has NULL context!\n", (uint32_t)thread_self); fflush(stdout);
        std::abort();
    }
    run_next_thread(PASS_RDRAM1);
    wait_for_resumed(PASS_RDRAM cur_context);
}

void ultramodern::resume_thread_and_wait(RDRAM_ARG OSThread *t) {
    UltraThreadContext* cur_context = TO_PTR(OSThread, thread_self)->context;
    if (cur_context == nullptr) {
        printf("[CRASH] resume_thread_and_wait: cur thread 0x%08X has NULL context!\n", (uint32_t)thread_self); fflush(stdout);
        std::abort();
    }
    resume_thread(t);
    wait_for_resumed(PASS_RDRAM cur_context);
}

static void _thread_func(RDRAM_ARG PTR(OSThread) self_, PTR(thread_func_t) entrypoint, PTR(void) arg, UltraThreadContext* thread_context) {
    OSThread *self = TO_PTR(OSThread, self_);
    // [Thread] entry/name lines emit twice per thread × ~15 threads at
    // startup. Useful for thread-system crash investigation; default off.
    // Honors ROGUESQ_LOG_THREAD_LIFECYCLE / ROGUESQ_LOG_ALL.
    static const bool log_t = []{
        const char *a = std::getenv("ROGUESQ_LOG_ALL");
        if (a && *a && *a != '0') return true;
        const char *e = std::getenv("ROGUESQ_LOG_THREAD_LIFECYCLE");
        return e && *e && *e != '0';
    }();
    if (log_t) {
        printf("[Thread] _thread_func: self_=0x%08X self=%p id=%d\n",
               (uint32_t)self_, (void*)self, self ? self->id : -1); fflush(stdout);
    }
    debug_printf("[Thread] Thread created: %d\n", self->id);
    thread_self = self_;
    is_game_thread = true;

    // Set the thread name
    auto _thread_name = ultramodern::threads::get_game_thread_name(self);
    if (log_t) {
        printf("[Thread] about to set name '%s'\n", _thread_name.c_str()); fflush(stdout);
    }
    ultramodern::set_native_thread_name(_thread_name);
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::High);

    // Signal the initialized semaphore to indicate that this thread can be started.
    thread_context->initialized.signal();

    debug_printf("[Thread] Thread waiting to be started: %d\n", self->id);

    // Wait until the thread is marked as running.
    try {
        wait_for_resumed(PASS_RDRAM thread_context);
    } catch (ultramodern::thread_terminated& terminated) {
    }

    // Make sure the thread wasn't replaced or destroyed before it was started.
    if (self->context == thread_context) {
        debug_printf("[Thread] Thread started: %d\n", self->id);
        try {
            // Run the thread's function with the provided argument.
            run_thread_function(PASS_RDRAM entrypoint, self->sp, arg);
        } catch (ultramodern::thread_terminated& terminated) {
#if defined(__GLIBC__)
        } catch (abi::__forced_unwind&) {
            // glibc's pthread_exit (end_deleted_thread) unwinds with this; swallowing it aborts the process.
            throw;
#endif
        } catch (const std::exception& e) {
            fprintf(stderr, "[Thread] EXCEPTION on thread id=%d entry=0x%08X: %s\n",
                self->id, (uint32_t)entrypoint, e.what());
            fflush(stderr);
        } catch (...) {
            fprintf(stderr, "[Thread] UNKNOWN EXCEPTION on thread id=%d entry=0x%08X\n",
                self->id, (uint32_t)entrypoint);
            fflush(stderr);
        }
    }
    else {
        debug_printf("[Thread] Thread destroyed before being started: %d\n", self->id);
    }

    // Check if the thread hasn't been destroyed or replaced. If so, then the thread terminated or destroyed itself,
    // so mark this thread as destroyed and run the next queued thread.
    if (self->context == thread_context) {
        self->context = nullptr;
        run_next_thread(PASS_RDRAM1);
    }

    // Dispose of this thread now that it's completed or terminated.
    ultramodern::cleanup_thread(thread_context);
}

extern "C" void osStartThread(RDRAM_ARG PTR(OSThread) t_) {
    OSThread* t = TO_PTR(OSThread, t_);
    debug_printf("[os] Start Thread %d\n", t->id);

    // If this is a game thread, insert the new thread into the running queue and then check the running queue.
    if (thread_self) {
        ultramodern::schedule_running_thread(PASS_RDRAM t_);
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
    // Otherwise, immediately start the thread and terminate this one.
    else {
        t->state = OSThreadState::QUEUED;
        resume_thread(t);
        //throw ultramodern::thread_terminated{};
    }
}

extern "C" void osCreateThread(RDRAM_ARG PTR(OSThread) t_, OSId id, PTR(thread_func_t) entrypoint, PTR(void) arg, PTR(void) sp, OSPri pri) {
    debug_printf("[os] Create Thread %d\n", id);
    OSThread *t = TO_PTR(OSThread, t_);
    
    t->next = NULLPTR;
    t->queue = NULLPTR;
    t->priority = pri;
    t->id = id;
    t->state = OSThreadState::STOPPED;
    t->sp = sp - 0x10; // Set up the first stack frame

    // Spawn a new thread, which will immediately pause itself and wait until it's been started.
    // Pass the context as an argument to the thread function to ensure that it can't get cleared before the thread captures its value.
    UltraThreadContext* context = new UltraThreadContext{};
    t->context = context;
    context->host_thread = std::thread{_thread_func, PASS_RDRAM t_, entrypoint, arg, t->context};

    // Wait until the thread is initialized to indicate that it's ready to be started.
    context->initialized.wait();
    debug_printf("[os] Thread %d is ready to be started\n", t->id);
}

extern "C" void osStopThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    // Check if the thread is stopping itself (arg is null or thread_self).
    if (t_ == thread_self) {
        ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
    }
    else {
        // Stopping another thread: remove it from whatever queue it's on and mark
        // it STOPPED. The host std::thread is already parked inside wait_for_resumed,
        // so it will stay parked until a later osStartThread signals running again.
        OSThread* t = TO_PTR(OSThread, t_);
        if (t->state != OSThreadState::STOPPED) {
            if (t->queue != NULLPTR) {
                ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
            }
            t->state = OSThreadState::STOPPED;
        }
    }
}

extern "C" void osDestroyThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    // Check if the thread is destroying itself (arg is null or thread_self)
    if (t_ == thread_self) {
        throw ultramodern::thread_terminated{};
    }
    // Otherwise if the thread isn't stopped, remove it from its currrent queue., 
    if (t->state != OSThreadState::STOPPED) {
        ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
    }
    // Check if the thread has already been destroyed to prevent destroying it again.
    UltraThreadContext* cur_context = t->context;
    if (cur_context != nullptr) {
        // Mark the target thread as destroyed and resume it. When it starts it'll check this and terminate itself instead of resuming.
        t->context = nullptr;
        cur_context->running.signal();
    }
}

extern "C" void osSetThreadPri(RDRAM_ARG PTR(OSThread) t_, OSPri pri) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);

    if (t->priority != pri) {
        t->priority = pri;

        if (t_ != ultramodern::this_thread() && t->state != OSThreadState::STOPPED) {
            ultramodern::thread_queue_remove(PASS_RDRAM t->queue, t_);
            ultramodern::thread_queue_insert(PASS_RDRAM t->queue, t_);
        }

        ultramodern::check_running_queue(PASS_RDRAM1);
    }
}

extern "C" OSPri osGetThreadPri(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->priority;
}

extern "C" OSId osGetThreadId(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->id;
}

PTR(OSThread) ultramodern::this_thread() {
    return thread_self;
}

static std::thread thread_cleaner_thread;
static moodycamel::BlockingConcurrentQueue<UltraThreadContext*> deleted_threads{};
extern std::atomic_bool exited;

void thread_cleaner_func() {
    using namespace std::chrono_literals;
    while (!exited) {
        UltraThreadContext* to_delete;
        if (deleted_threads.wait_dequeue_timed(to_delete, 10ms)) {
            debug_printf("[Cleanup] Deleting thread context %p\n", to_delete);

            to_delete->host_thread.join();
            delete to_delete;
        }
    }
}

void ultramodern::init_thread_cleanup() {
    thread_cleaner_thread = std::thread{thread_cleaner_func};
}

void ultramodern::cleanup_thread(UltraThreadContext *cur_context) {
    deleted_threads.enqueue(cur_context);
}

void ultramodern::join_thread_cleaner_thread() {
    thread_cleaner_thread.join();
}
