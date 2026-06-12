#include <thread>
#include <vector>
#include <functional>
#include "blocking_queue.cpp"

class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t num_threads) {
        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    void submit(Task task) {
        queue_.push(std::move(task));
    }

    ~ThreadPool() {
        queue_.stop();
        for (auto& t : workers_) t.join();
    }

private:
    void worker_loop() {
        while (true) {
            auto task = queue_.pop();
            if (!task) break;
            (*task)();
        }
    }

    BlockingQueue<Task>      queue_;
    std::vector<std::thread> workers_;
};