## Реализация классов std::future и std::promise. В чем их неудобство?

### Внутреннее устройство: Shared State

И `future`, и `promise` — это просто два handle на один общий объект (shared state), который живёт в куче:

```text
std::promise<T>          std::future<T>
      │                        │
      └──────► SharedState ◄───┘
               ┌────────────┐
               │ value: T   │
               │ exception  │
               │ mutex      │
               │ condvar    │
               │ ready: bool│
               │ ref_count  │
               └────────────┘
```

Когда `promise` умирает — он уведомляет `shared state`. 
Когда `future` умирает — shared `state` освобождается если `ref_count = 0`.

### Неудобства `std::future/std::promise`

* `get()` можно вызвать только один раз

    ```cpp
    auto f = p.get_future();
    int x = f.get();
    int y = f.get(); // исключение

    // Решение — std::shared_future
    std::shared_future<int> sf = p.get_future().share();
    int x = sf.get(); // ок
    int y = sf.get(); // ок
    ```

* Нет `.then()` в стандарте

    ```cpp
    // Хочется:
    f.then([](int x) { return x * 2; })
    .then([](int x) { return std::to_string(x); });

    // Реальность в стандартном C++:
    auto f2 = std::async([f1 = std::move(f1)]() mutable {
        return f1.get() * 2;
    });
    auto f3 = std::async([f2 = std::move(f2)]() mutable {
        return std::to_string(f2.get());
    });
    ```

* Каждый `std::async` создаёт поток

    ```cpp
    for (int i = 0; i < 1000; i++) {
        futures.push_back(std::async(std::launch::async, task));
    }
    ```

* Нет отмены (cancellation)

    Нельзя отменить вычисление, которое уже запущено:

    ```cpp
    auto f = std::async(task);
    // Хочу отменить — но не могу!
    // f.cancel() — не существует
    ```