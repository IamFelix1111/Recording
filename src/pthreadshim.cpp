// Minimal winpthreads shim: satisfies the handful of pthread symbols that
// libstdc++'s gthread glue (eh_alloc / locale-less builds) references, using
// plain Win32 primitives. Linked as a plain object (before -lstdc++), so the
// linker resolves these here instead of pulling in libwinpthread-1.dll -
// the result is a single self-contained exe.
//
// ABI notes (winpthreads headers): pthread_once_t = long,
// pthread_mutex_t = intptr_t (pointer-sized), pthread_key_t = unsigned.
// Only pointers are exchanged, so the caller-side storage size is what
// matters and these match exactly.

#include <windows.h>

#include <cstdint>

extern "C" {

using shim_once_t = long;
using shim_mutex_t = intptr_t;
using shim_key_t = unsigned;

int pthread_once(shim_once_t* once_control, void (*init_routine)(void)) {
    volatile LONG* state = reinterpret_cast<volatile LONG*>(once_control);
    for (;;) {
        const LONG s = *state;
        if (s == 2) return 0;  // already initialized
        if (s == 0 &&
            InterlockedCompareExchange(state, 1, 0) == 0) {
            init_routine();
            *state = 2;
            return 0;
        }
        Sleep(0);  // another thread is running the init routine
    }
}

int pthread_mutex_init(shim_mutex_t* mutex, const void* /*attr*/) {
    InitializeSRWLock(reinterpret_cast<SRWLOCK*>(mutex));
    return 0;
}

int pthread_mutex_destroy(shim_mutex_t* /*mutex*/) { return 0; }  // SRWLOCK needs no teardown

int pthread_mutex_lock(shim_mutex_t* mutex) {
    AcquireSRWLockExclusive(reinterpret_cast<SRWLOCK*>(mutex));
    return 0;
}

int pthread_mutex_unlock(shim_mutex_t* mutex) {
    ReleaseSRWLockExclusive(reinterpret_cast<SRWLOCK*>(mutex));
    return 0;
}

// Thread-specific storage via fiber-local storage (FLS): per-thread slots
// with a destructor callback, matching the pthread_key semantics libstdc++
// needs for its EH/locale glue.
int pthread_key_create(shim_key_t* key, void (*destructor)(void*)) {
    const DWORD fls = FlsAlloc(reinterpret_cast<PFLS_CALLBACK_FUNCTION>(destructor));
    if (fls == FLS_OUT_OF_INDEXES) return 1;
    *key = static_cast<shim_key_t>(fls);
    return 0;
}

void* pthread_getspecific(shim_key_t key) {
    return FlsGetValue(static_cast<DWORD>(key));
}

int pthread_setspecific(shim_key_t key, const void* value) {
    return FlsSetValue(static_cast<DWORD>(key), const_cast<void*>(value)) ? 0 : 1;
}

}  // extern "C"
