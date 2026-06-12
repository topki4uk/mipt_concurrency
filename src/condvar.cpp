#include <atomic>
#include <mutex>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

class CondVar {
public:
    void wait(std::unique_lock<std::mutex>& lock) {
        int expected = gen_counter_.load();
        lock.unlock();

        syscall(SYS_futex, &gen_counter_, FUTEX_WAIT, expected, nullptr, nullptr, 0);

        lock.lock();
    }

    void notify_one() {
        gen_counter_.fetch_add(1);
        syscall(SYS_futex, &gen_counter_, FUTEX_WAKE, 1, nullptr, nullptr, 0);
    }

    void notify_all() {
        gen_counter_.fetch_add(1);
        syscall(SYS_futex, &gen_counter_, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
    }

private:
    std::atomic<int> gen_counter_{0};
};