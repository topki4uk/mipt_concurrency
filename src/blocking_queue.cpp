#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

template<typename T>
class BlockingQueue {
public:
    // Добавить задачу
    void push(T item) {
        {
            std::unique_lock lock(mtx_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Взять задачу
    std::optional<T> pop() {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [this] {
            return !queue_.empty() || stopped_;
        });

        if (queue_.empty()) return std::nullopt;

        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    // Остановить очередь
    void stop() {
        {
            std::unique_lock lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all();
    }

private:
    std::queue<T>           queue_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    bool                    stopped_ = false;
};