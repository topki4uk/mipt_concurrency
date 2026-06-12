template<typename T>
class Future {
    std::shared_ptr<SharedState<T>> state_;

    explicit Future(std::shared_ptr<SharedState<T>> state)
        : state_(std::move(state)) {}

    friend class Promise<T>;

public:
    Future() = default;

    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) = default;
    Future& operator=(Future&&) = default;

    T get() {
        if (!state_) throw std::runtime_error("no shared state");
        auto state = std::move(state_);
        return state->get();
    }

    bool valid() const { return state_ != nullptr; }

    bool is_ready() const {
        if (!state_) return false;
        return state_->is_ready();
    }

    void wait() const {
        state_->get(); // упрощённо
    }
};