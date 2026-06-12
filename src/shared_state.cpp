#include <mutex>
#include <condition_variable>
#include <exception>
#include <stdexcept>
#include <memory>
#include <optional>

template<typename T>
struct SharedState {
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::optional<T>        value_;
    std::exception_ptr      exception_;
    bool                    ready_  = false;
    bool                    future_retrieved_ = false;

    void set_value(T val) {
        std::unique_lock lock(mtx_);
        if (ready_) throw std::runtime_error("promise already satisfied");
        value_ = std::move(val);
        ready_ = true;
        cv_.notify_all();
    }

    void set_exception(std::exception_ptr ep) {
        std::unique_lock lock(mtx_);
        if (ready_) throw std::runtime_error("promise already satisfied");
        exception_ = ep;
        ready_ = true;
        cv_.notify_all();
    }

    T get() {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [this] { return ready_; });

        if (exception_) {
            std::rethrow_exception(exception_);
        }
        return std::move(*value_);
    }

    bool is_ready() {
        std::unique_lock lock(mtx_);
        return ready_;
    }
};