#include <cstdint>
#include "prx/common/StderrLog.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include "SceTypes.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Pthread/include/WakeEvent.hpp"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using GuestHandler = void (APS5_VABI *)(int, void*);
constexpr int MaxSignals = 128;
std::atomic<GuestHandler> handlers[MaxSignals];

#ifdef _WIN32
constexpr std::size_t XStateBytes = 1024;
struct alignas(64) ExceptionFrame {
    alignas(64) char xstate[XStateBytes];
    CONTEXT context;
    int signum;
    void* mcontext;
    char mcontextStorage[0x400];
};
struct NtAlert { using Fn = LONG (NTAPI *)(HANDLE); };
bool IsGuestAddress(DWORD64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module) || !module) return false;
    if (module == GetModuleHandleW(nullptr)) return true;
    wchar_t name[MAX_PATH] = {};
    GetModuleFileNameW(module, name, MAX_PATH);
    const std::wstring path(name);
    return path.find(L".guest.prx") != std::wstring::npos;
}
void NudgeThread(Pthread thread) {
    static const auto alertById = reinterpret_cast<LONG (NTAPI *)(ULONG_PTR)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAlertThreadByThreadId"));
    if (alertById) alertById(GetThreadId(static_cast<HANDLE>(thread->nativeHandle)));
}
#endif

unsigned XMaskLow() {
    unsigned a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if ((c & (1u << 27)) == 0) return 0;
    unsigned low = 0, high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return low & 7u;
}
}
extern "C" unsigned DeliverXMaskLow;
extern "C" unsigned DeliverXMaskHigh;
unsigned DeliverXMaskLow = XMaskLow();
unsigned DeliverXMaskHigh = 0;
#ifdef _WIN32
namespace {
void DeliverExceptionImpl(ExceptionFrame* frame) {
    auto handler = handlers[frame->signum].load();
    if (handler) handler(frame->signum, frame->mcontext);
    if (DeliverXMaskLow != 0) {
        const unsigned low = DeliverXMaskLow, high = DeliverXMaskHigh;
        __asm__ volatile("xrstor64 (%0)" : : "r"(frame->xstate), "a"(low), "d"(high) : "memory");
    }
    RtlRestoreContext(&frame->context, nullptr);
}

}

extern "C" void DeliverExceptionThunk();
extern "C" void* DeliverExceptionTarget;
void* DeliverExceptionTarget = reinterpret_cast<void*>(&DeliverExceptionImpl);
__asm__(".text\n"
        ".globl DeliverExceptionThunk\n"
        "DeliverExceptionThunk:\n"
        "movl DeliverXMaskLow(%rip), %eax\n"
        "testl %eax, %eax\n"
        "jz 1f\n"
        "movl DeliverXMaskHigh(%rip), %edx\n"
        "xsave64 (%rbx)\n"
        "1:\n"
        "movq %rbx, %rcx\n"
        "jmp *DeliverExceptionTarget(%rip)\n");
#endif

static std::atomic<unsigned> gcedHanded, gcedInjected, gcedSkipped, gcedRun;

static unsigned gcWaitMillis() {
    static const unsigned value = [] { const char* text = std::getenv("APS5_GC_WAIT_MS"); return text ? static_cast<unsigned>(std::strtoul(text, nullptr, 10)) : 1000u; }();
    return value;
}

void RunExceptionHandlerInline(int signum) {
    const auto handler = handlers[signum].load();
    if (!handler) return;
    aps5::LogErr("[gced] ran signum=%d run=%u\n", signum, gcedRun.fetch_add(1) + 1);
    alignas(16) char mcontext[0x400] = {};
    *reinterpret_cast<void**>(mcontext + 0xf8) = mcontext;
    handler(signum, mcontext);
}

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <link.h>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>

// Linux counterpart of the SuspendThread/SetThreadContext injection used on
// Windows. The target thread is interrupted with a real-time signal; the handler
// runs on the target thread and either
//   * runs the guest exception handler right there when the thread was
//     executing guest code (the kernel restores the full register and extended
//     state when the handler returns, like RtlRestoreContext + xrstor on Windows),
//   * leaves the request to the wait path when the thread is parked in an
//     interruptible wait (the raiser then sets the thread's wake event), or
//   * declines when the thread is in host code, and the raiser retries until the
//     APS5_GC_WAIT_MS budget is spent.
extern "C" Pthread APS5_VABI scePthreadSelf();

namespace {

enum : int { AckNone = 0, AckRan = 1, AckHanded = 2, AckRetry = 3, AckConsumed = 4 };

struct GuestRange { std::uintptr_t begin; std::uintptr_t end; };
constexpr int MaxGuestRanges = 512;
GuestRange guestRanges[MaxGuestRanges];
std::atomic<int> guestRangeCount{0};
std::mutex raiseMutex;

bool IsGuestModuleName(const char* name) {
    if (!name || !*name) return true; // main executable: the relinked eboot
    return std::strstr(name, ".guest.prx") != nullptr || std::strstr(name, "sce_module") != nullptr;
}

int CollectGuestRanges(dl_phdr_info* info, std::size_t, void* data) {
    auto* count = static_cast<int*>(data);
    if (!IsGuestModuleName(info->dlpi_name)) return 0;
    for (int index = 0; index < info->dlpi_phnum && *count < MaxGuestRanges; ++index) {
        const auto& header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;
        const std::uintptr_t begin = info->dlpi_addr + header.p_vaddr;
        guestRanges[(*count)++] = {begin, begin + header.p_memsz};
    }
    return 0;
}

void RefreshGuestRanges() {
    int count = 0;
    dl_iterate_phdr(CollectGuestRanges, &count);
    guestRangeCount.store(count, std::memory_order_release);
}

bool IsGuestAddressSignalSafe(std::uintptr_t address) {
    const int count = guestRangeCount.load(std::memory_order_acquire);
    for (int index = 0; index < count; ++index)
        if (address >= guestRanges[index].begin && address < guestRanges[index].end) return true;
    return false;
}

int InterruptSignal() {
    static const int value = SIGRTMIN + 6;
    return value;
}

void InterruptSignalHandler(int, siginfo_t*, void* rawContext) {
    const int savedErrno = errno;
    Pthread self = scePthreadSelf();
    if (!self) { errno = savedErrno; return; }
    const int requested = self->pendingException.load();
    if (requested == 0) {
        self->signalAck.store(AckConsumed);
    } else if (self->inWait.load()) {
        self->signalAck.store(AckHanded);
    } else {
        const auto* context = static_cast<const ucontext_t*>(rawContext);
        const auto rip = static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
        const int taken = self->pendingException.exchange(0);
        if (taken == 0) {
            self->signalAck.store(AckConsumed);
        } else if (IsGuestAddressSignalSafe(rip)) {
            self->signalAck.store(AckRan);
            if (const auto handler = handlers[taken].load()) {
                alignas(16) char mcontext[0x400] = {};
                *reinterpret_cast<void**>(mcontext + 0xf8) = mcontext;
                handler(taken, mcontext);
            }
        } else {
            self->signalAck.store(AckRetry);
        }
    }
    errno = savedErrno;
}

void InstallInterruptSignal() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction action{};
        action.sa_sigaction = InterruptSignalHandler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(InterruptSignal(), &action, nullptr);
    });
}

int RaiseOnOtherThreadLinux(Pthread thread, int signum) {
    if (!thread->nativeThreadReady.load(std::memory_order_acquire)) return static_cast<int>(0x80020003);
    InstallInterruptSignal();
    std::lock_guard serialize(raiseMutex);
    RefreshGuestRanges();
    const pthread_t native = thread->nativeThread;
    const auto giveUpAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(gcWaitMillis());
    for (int attempt = 0;; ++attempt) {
        if (attempt >= 4096 && std::chrono::steady_clock::now() >= giveUpAt) break;
        thread->pendingException.store(signum);
        if (thread->inWait.load()) {
            if (thread->wakeEvent) aps5::WakeEventSet(thread->wakeEvent);
            aps5::LogErr("[gced] hand h=%u inj=%u skip=%u\n", gcedHanded.fetch_add(1) + 1, gcedInjected.load(), gcedSkipped.load());
            return 0;
        }
        thread->signalAck.store(AckNone);
        if (pthread_kill(native, InterruptSignal()) != 0) {
            thread->pendingException.store(0);
            return static_cast<int>(0x80020003);
        }
        int ack = AckNone;
        const auto ackDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(gcWaitMillis());
        // Only the handler's acknowledgement counts: it clears pendingException before it
        // answers AckRetry, so a cleared request alone does not mean it was delivered.
        while ((ack = thread->signalAck.load()) == AckNone) {
            if (std::chrono::steady_clock::now() >= ackDeadline) break;
            sched_yield();
        }
        if (ack == AckRan || ack == AckConsumed) {
            const unsigned injections = gcedInjected.fetch_add(1) + 1;
            if ((injections & 63) == 1)
                aps5::LogErr("[gced] inj n=%u h=%u skip=%u\n", injections, gcedHanded.load(), gcedSkipped.load());
            return 0;
        }
        if (ack == AckHanded) {
            if (thread->wakeEvent) aps5::WakeEventSet(thread->wakeEvent);
            aps5::LogErr("[gced] hand h=%u inj=%u skip=%u\n", gcedHanded.fetch_add(1) + 1, gcedInjected.load(), gcedSkipped.load());
            return 0;
        }
        if (ack == AckNone) break;
        if (attempt < 64) sched_yield();
        else { const timespec pause{0, attempt < 4096 ? 0L : 1000000L}; nanosleep(&pause, nullptr); }
    }
    int expected = signum;
    thread->pendingException.compare_exchange_strong(expected, 0);
    aps5::LogErr("[gced] skip h=%u inj=%u skip=%u\n", gcedHanded.load(), gcedInjected.load(), gcedSkipped.fetch_add(1) + 1);
    return static_cast<int>(0x80020003);
}

}
#endif

extern "C" {

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
    if (signum < 0 || signum >= MaxSignals) return static_cast<int>(0x80020016);
    aps5::LogErr( "[EXC] install handler signum=%d handler=%p\n", signum, handler);
    handlers[signum].store(reinterpret_cast<GuestHandler>(handler));
    return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
    if (signum < 0 || signum >= MaxSignals) return static_cast<int>(0x80020016);
    handlers[signum].store(nullptr);
    return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
    if (signum < 0 || signum >= MaxSignals) return static_cast<int>(0x80020016);
    const auto handler = handlers[signum].load();
    if (!thread || thread->threadId == std::this_thread::get_id()) {
        if (handler) { alignas(16) char mcontext[0x400] = {}; *reinterpret_cast<void**>(mcontext + 0xf8) = mcontext; handler(signum, mcontext); }
        return 0;
    }
    if (!handler) return 0;
#ifndef _WIN32
    return RaiseOnOtherThreadLinux(thread, signum);
#else
    HANDLE native = static_cast<HANDLE>(thread->nativeHandle);

    CONTEXT context{};
    bool interrupted = false;
    bool handed = false;

    const auto giveUpAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(gcWaitMillis());
    for (int attempt = 0;; ++attempt) {
        if (attempt >= 4096 && std::chrono::steady_clock::now() >= giveUpAt) break;
        if (SuspendThread(native) == static_cast<DWORD>(-1)) return static_cast<int>(0x80020003);
        context = {};
        context.ContextFlags = CONTEXT_ALL;
        const bool readable = GetThreadContext(native, &context) != 0;

        thread->pendingException.store(signum);
        if (thread->inWait.load()) {
            handed = true;
            break;
        }
        thread->pendingException.store(0);
        if (readable && IsGuestAddress(context.Rip)) {
            interrupted = true;
            break;
        }
        ResumeThread(native);
        if (!readable) return static_cast<int>(0x80020003);
        if (attempt < 64) SwitchToThread(); else if (attempt < 4096) Sleep(0); else Sleep(1);
    }

    if (handed) {
        ResumeThread(native);
        if (thread->wakeEvent) SetEvent(static_cast<HANDLE>(thread->wakeEvent));
        aps5::LogErr("[gced] hand tid=%u h=%u inj=%u skip=%u\n", GetThreadId(native),
                     gcedHanded.fetch_add(1) + 1, gcedInjected.load(), gcedSkipped.load());
        return 0;
    }
    if (!interrupted) {

        {
            char where[400] = {};
            int used = 0;
            DWORD64 candidates[8] = {context.Rip};
            int found = 1;
            const auto* stack = reinterpret_cast<const DWORD64*>(context.Rsp);
            for (int slot = 0; slot < 128 && found < 8; ++slot) {
                DWORD64 value = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), stack + slot, &value, sizeof(value), nullptr)) break;
                HMODULE owner = nullptr;
                if (value > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(value), &owner) && owner != nullptr) candidates[found++] = value;
            }
            for (int index = 0; index < found && used < static_cast<int>(sizeof(where)) - 80; ++index) {
                HMODULE owner = nullptr;
                wchar_t name[MAX_PATH] = {};
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(candidates[index]), &owner) && owner != nullptr) GetModuleFileNameW(owner, name, MAX_PATH);
                const wchar_t* base = wcsrchr(name, L'\\');
                used += std::snprintf(where + used, sizeof(where) - used, " %ls+0x%llx", base ? base + 1 : name, static_cast<unsigned long long>(candidates[index] - reinterpret_cast<DWORD64>(owner)));
            }
            aps5::LogErr("[gced] skipped tid=%u at%s\n", GetThreadId(native), where);
        }
        aps5::LogErr("[gced] skip tid=%u h=%u inj=%u skip=%u\n", GetThreadId(native),
                     gcedHanded.load(), gcedInjected.load(), gcedSkipped.fetch_add(1) + 1);
        return static_cast<int>(0x80020003);
    }

    auto top = (context.Rsp - 0x200 - sizeof(ExceptionFrame)) & ~static_cast<DWORD64>(0x3f);
    auto* frame = reinterpret_cast<ExceptionFrame*>(top);
    std::memset(&frame->context, 0, sizeof(*frame) - offsetof(ExceptionFrame, context));
    std::memset(frame->xstate, 0, 576);
    frame->context = context;
    frame->signum = signum;
    frame->mcontext = frame->mcontextStorage;

    *reinterpret_cast<void**>(frame->mcontextStorage + 0xf8) = frame;
    CONTEXT redirected = context;
    redirected.Rsp = top - 0x28;
    redirected.Rip = reinterpret_cast<DWORD64>(&DeliverExceptionThunk);
    redirected.Rbx = reinterpret_cast<DWORD64>(frame);
    const bool ok = SetThreadContext(native, &redirected) != 0;
    ResumeThread(native);

    if (ok) NudgeThread(thread);
    const unsigned injections = gcedInjected.fetch_add(1) + 1;
    if ((injections & 63) == 1)
        aps5::LogErr("[gced] inj n=%u h=%u skip=%u\n", injections, gcedHanded.load(), gcedSkipped.load());
    return ok ? 0 : static_cast<int>(0x80020003);
#endif
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseException c1=%d c2=%d", c1, c2);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseExceptionOnReleaseMode c1=%d c2=%d", c1, c2);
}

}
