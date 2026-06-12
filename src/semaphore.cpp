#include <mutex>
#include <condition_variable>
#include <stdexcept>

class Semaphore {
public:
    explicit Semaphore(int count) : count_(count) {
        if (count < 0) throw std::invalid_argument("count < 0");
    }

    void acquire() {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

    void release() {
        {
            std::unique_lock lock(mtx_);
            ++count_;
        }
        cv_.notify_one();
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    int count_;
};