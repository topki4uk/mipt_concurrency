## Thread pool с блокирующей очередью. Реализация с помощью `condition_variable`.

Пул потоков — это паттерн, где заранее создаётся фиксированное число потоков, 
которые берут задачи из общей очереди и выполняют их. 
Это избавляет от дорогостоящего создания/уничтожения потока на каждую задачу.

### Блокирующая очередь

Сначала строим главный кирпич — `BlockingQueue`. 
Она блокирует поток при попытке взять задачу из пустой очереди (потоки «спят» и не жгут CPU):

```cpp
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>

template<typename T>
class BlockingQueue {
public:
    // Добавить задачу (вызывается снаружи)
    void push(T item) {
        {
            std::unique_lock lock(mtx_);
            queue_.push(std::move(item));
        }
        cv_.notify_one(); // будим одного спящего рабочего
    }

    // Взять задачу (вызывается рабочими потоками)
    // Возвращает nullopt если очередь остановлена и пуста
    std::optional<T> pop() {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [this] {
            return !queue_.empty() || stopped_;
        });

        if (queue_.empty()) return std::nullopt; // сигнал завершения

        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    // Остановить очередь — разбудить всех ждущих
    void stop() {
        {
            std::unique_lock lock(mtx_);
            stopped_ = true;
        }
        cv_.notify_all(); // будим ВСЕХ, чтобы все увидели флаг
    }

private:
    std::queue<T>           queue_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    bool                    stopped_ = false;
};
```

Ключевой момент: `stop()` выставляет флаг и вызывает `notify_all()` — это единственный способ 
гарантированно разбудить все спящие потоки, иначе они заснут навсегда.

### Thread Pool

```cpp
#include <thread>
#include <vector>
#include <functional>

class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(size_t num_threads) {
        workers_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // Добавить задачу в пул
    void submit(Task task) {
        queue_.push(std::move(task));
    }

    ~ThreadPool() {
        queue_.stop();                        // сигнал завершения
        for (auto& t : workers_) t.join();   // ждём всех рабочих
    }

private:
    void worker_loop() {
        while (true) {
            auto task = queue_.pop();   // блокируемся здесь до появления задачи
            if (!task) break;           // nullopt = пора заканчивать
            (*task)();                  // выполняем задачу
        }
    }

    BlockingQueue<Task>      queue_;
    std::vector<std::thread> workers_;
};
```

#### Использование

```cpp
int main() {
    ThreadPool pool(4); // 4 рабочих потока

    for (int i = 0; i < 20; ++i) {
        pool.submit([i] {
            std::cout << "Task " << i
                      << " on thread " << std::this_thread::get_id() << "\n";
        });
    }
    // Деструктор: stop() + join() — все задачи доделываются
}
```

Важная деталь деструктора: сначала `stop()`, потом `join()`. 
Если сделать наоборот — `join()` заблокируется навсегда, ведь потоки ещё спят в `cv_.wait()` 
и никогда не узнают, что пора заканчивать.