#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "futex.hpp"

class Mutex2State {
public:
    void lock() {
        if (state_.exchange(1) == 0)
            return;

        do {
            futex_wait(&state_, 1);
        } while (state_.exchange(1) != 0);
    }

    void unlock() {
        state_.store(0);
        futex_wake(&state_, 1);
    }

private:
    std::atomic<int> state_{0};
};