template<typename T>
class Promise {
    std::shared_ptr<SharedState<T>> state_;
    bool future_retrieved_ = false;
    bool value_set_        = false;

public:
    Promise() : state_(std::make_shared<SharedState<T>>()) {}

    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&&) = default;
    Promise& operator=(Promise&&) = default;

    ~Promise() {
        if (state_ && !value_set_) {
            state_->set_exception(
                std::make_exception_ptr(
                    std::runtime_error("broken promise")
                )
            );
        }
    }

    Future<T> get_future() {
        if (future_retrieved_) {
            throw std::runtime_error("future already retrieved");
        }
        future_retrieved_ = true;
        return Future<T>(state_);
    }

    void set_value(T value) {
        value_set_ = true;
        state_->set_value(std::move(value));
    }

    void set_exception(std::exception_ptr ep) {
        value_set_ = true;
        state_->set_exception(ep);
    }
};