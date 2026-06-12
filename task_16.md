## Stackful и stateless корутины. Сходства и различия с файберами. Корутины в C++, их внутреннее устройство (в общих чертах). Оператор co_await. Ключевое слово co_return.

### Stackful vs Stackless корутины

Главное различие — где хранится состояние при приостановке.

#### Stackful (с отдельным стеком)

Это фактически **файберы**. Каждая корутина имеет собственный стек в куче. Приостановка = сохранение всего стека.

`yield()` можно вызвать из любой вложенной функции — весь стек сохраняется как есть.

#### Stackless (без отдельного стека)

Корутина не имеет своего стека. Её локальные переменные хранятся в специальном объекте на куче — **coroutine frame**. 
При приостановке стек просто разматывается:

```cpp
Stackless корутина:

  Стек потока (обычный):          Coroutine Frame (в куче, ~байты):
  ┌─────────────┐                 ┌──────────────────┐
  │  resume()   │ ────вызывает──► │ local vars: x, y │
  │  ...        │                 │ suspend point: 3 │
  └─────────────┘                 │ promise object   │
                                  └──────────────────┘
  yield → стек разматывается,     frame остаётся в куче
  frame сохраняет только locals
```

`co_await` можно вызвать только непосредственно в теле корутины, не из вложенных функций.

### Сравнение с файберами

|                            | Файберы        | Stackful       | Stackless (C++20)     |
|----------------------------|----------------|----------------|-----------------------|
| Свой стек                  | ✅ (64KB+)      | ✅ (64KB+)      | ❌ (только frame)      |
| yield из вложенных функций | ✅              | ✅              | ❌                     |
| Расход памяти              | Высокий        | Высокий        | Минимальный           |
| Поддержка в стандарте      | Нет            | Нет            | C++20                 |
| Context switch             | Asm (регистры) | Asm (регистры) | Обычный вызов функции |

### Корутины C++20: внутреннее устройство

Корутина в C++ — это функция, тело которой может быть приостановлено и возобновлено. 
Компилятор трансформирует её в машину состояний.

Что делает компилятор

```cpp
Task my_coroutine() {
    int x = 10;
    co_await some_awaitable();
    int y = x + 20;
    co_await another_awaitable();
    co_return x + y;
}
```

Компилятор генерирует примерно следующее:

```cpp
// То во что компилятор превращает корутину (псевдокод):
struct my_coroutine_frame {
    int x, y;

    Task::promise_type promise;

    int suspend_point = 0;

    void resume() {
        switch (suspend_point) {
        case 0:
            x = 10;
            if (!some_awaitable().await_ready()) {
                suspend_point = 1;
                some_awaitable().await_suspend(this_handle);
                return;
            }
        case 1:
            y = x + 20;
            if (!another_awaitable().await_ready()) {
                suspend_point = 2;
                another_awaitable().await_suspend(this_handle);
                return;
            }
        case 2:
            promise.return_value(x + y);
        }
    }
};
```

### Оператор `co_await`

`co_await expr` работает через концепцию Awaitable — объект должен иметь три метода:

```cpp
struct MyAwaitable {
    bool await_ready() { return false; }

    void await_suspend(std::coroutine_handle<> handle) {
        thread_pool.post([handle]() mutable {
            do_work();
            handle.resume();
        });
    }

    int await_resume() { return result_; }

    int result_;
};

// Использование:
Task example() {
    int x = co_await MyAwaitable{};
}
```

### Ключевое слово `co_return`

`co_return` сигнализирует о завершении корутины и передаёт результат через `promise`:

```cpp
Task<int> compute() {
    int result = 42;
    co_return result;  // promise.return_value(42)
}

Task<void> do_work() {
    do_something();
    co_return;  // promise.return_void()
}
```

Без `co_return` (корутина завалилась с конца) — компилятор вставляет `promise.return_void()` автоматически.

### Promise type: связующее звено

`promise_type` — это класс, который управляет поведением корутины. Он вкладывается в тип возвращаемого значения:

```cpp
template<typename T>
struct Task {
    struct promise_type {
        T value_;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never  initial_suspend() { return {}; }

        std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(T val) { value_ = std::move(val); }

        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle_;

    T get() {
        handle_.resume(); // если не запущена
        return handle_.promise().value_;
    }

    ~Task() { if (handle_) handle_.destroy(); }
};
```