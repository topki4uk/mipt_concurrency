template<typename T>
class Future {
    std::shared_ptr<SharedState<T>> state_;

    // Только Promise может создавать Future (через get_future)
    explicit Future(std::shared_ptr<SharedState<T>> state)
        : state_(std::move(state)) {}

    friend class Promise<T>;

public:
    Future() = default;

    // Некопируемый — владение результатом уникально
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) = default;
    Future& operator=(Future&&) = default;

    // Блокирующее получение результата (можно вызвать только раз)
    T get() {
        if (!state_) throw std::runtime_error("no shared state");
        auto state = std::move(state_); // после get() future невалиден
        return state->get();
    }

    bool valid() const { return state_ != nullptr; }

    bool is_ready() const {
        if (!state_) return false;
        return state_->is_ready();
    }

    // Ждём без получения результата
    void wait() const {
        state_->get(); // упрощённо
    }
};