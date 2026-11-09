#ifndef CORE_LIBS_PRX_LIBKERNEL_PTHREAD_INCLUDE_WAKEEVENT_HPP
#define CORE_LIBS_PRX_LIBKERNEL_PTHREAD_INCLUDE_WAKEEVENT_HPP

// Auto-reset event used to wake a parked guest thread (umtx waits, interruptible
// mutex locks, exception hand-off). On Windows this is a plain event HANDLE; on
// Linux it is a small mutex/condition pair with the same auto-reset semantics.

#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unistd.h>
#include <sys/syscall.h>
#endif

namespace aps5 {

constexpr unsigned WakeEventInfinite = 0xFFFFFFFFu;

#ifdef _WIN32

inline void* WakeEventCreate() { return CreateEventW(nullptr, FALSE, FALSE, nullptr); }
inline void WakeEventSet(void* event) { SetEvent(static_cast<HANDLE>(event)); }
inline void WakeEventReset(void* event) { ResetEvent(static_cast<HANDLE>(event)); }
// Returns true when the event was signaled, false on timeout.
inline bool WakeEventWait(void* event, unsigned milliseconds) {
    return WaitForSingleObject(static_cast<HANDLE>(event), milliseconds == WakeEventInfinite ? INFINITE : milliseconds) == WAIT_OBJECT_0;
}
inline unsigned long CurrentThreadIdForLog() { return GetCurrentThreadId(); }

#else

struct WakeEventState {
    std::mutex lock;
    std::condition_variable changed;
    bool signaled = false;
};

inline void* WakeEventCreate() { return new WakeEventState(); }
inline void WakeEventSet(void* event) {
    auto* state = static_cast<WakeEventState*>(event);
    {
        std::lock_guard guard(state->lock);
        state->signaled = true;
    }
    state->changed.notify_one();
}
inline void WakeEventReset(void* event) {
    auto* state = static_cast<WakeEventState*>(event);
    std::lock_guard guard(state->lock);
    state->signaled = false;
}
inline bool WakeEventWait(void* event, unsigned milliseconds) {
    auto* state = static_cast<WakeEventState*>(event);
    std::unique_lock guard(state->lock);
    if (milliseconds == WakeEventInfinite) {
        state->changed.wait(guard, [state] { return state->signaled; });
    } else if (!state->changed.wait_for(guard, std::chrono::milliseconds(milliseconds), [state] { return state->signaled; })) {
        return false;
    }
    state->signaled = false;
    return true;
}
inline unsigned long CurrentThreadIdForLog() { return static_cast<unsigned long>(::syscall(SYS_gettid)); }

#endif

}

#endif
