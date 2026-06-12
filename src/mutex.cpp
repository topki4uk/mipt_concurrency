#include <atomic>
#include <thread>

class Mutex {
public:
    void lock() {
        int spin_count = 0;
        while (flag_.test_and_set(std::memory_order_acquire)) {
            ++spin_count;
            if (spin_count < 16) {
                // Фаза 1: активный спин
            } else {
                std::this_thread::yield();
            }
        }
    }

    void unlock() {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};