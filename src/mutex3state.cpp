#include <atomic>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

class Mutex {
    static constexpr int FREE     = 0;
    static constexpr int LOCKED   = 1;
    static constexpr int SLEEPING = 2;

    std::atomic<int> state_{FREE};

    static void futex_wait(std::atomic<int>* addr, int expected) {
        syscall(SYS_futex, addr, FUTEX_WAIT, expected, nullptr);
    }

    static void futex_wake(std::atomic<int>* addr, int count) {
        syscall(SYS_futex, addr, FUTEX_WAKE, count, nullptr);
    }

public:
    void lock() {
        int expected = FREE;
        if (state_.compare_exchange_strong(
                expected, LOCKED,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            return;
        }

        while (state_.exchange(SLEEPING,
                               std::memory_order_acquire) != FREE) {
            futex_wait(&state_, SLEEPING);
        }
    }

    void unlock() {
        int prev = state_.fetch_sub(1, std::memory_order_release);

        if (prev == SLEEPING) {
            state_.store(FREE, std::memory_order_release);
            futex_wake(&state_, 1);
        }
    }
};