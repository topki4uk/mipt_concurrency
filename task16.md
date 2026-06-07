## Stackful и stateless корутины. Сходства и различия с файберами. Корутины в C++, их внутреннее устройство (в общих чертах). Оператор co_await. Ключевое слово co_return.

### Stackful vs Stackless корутины

Главное различие — где хранится состояние при приостановке.

#### Stackful (с отдельным стеком)

Это фактически **файберы**. Каждая корутина имеет собственный стек в куче. Приостановка = сохранение всего стека:

```text
Stackful корутина:

  Стек корутины A (в куче, 64KB):
  ┌─────────────┐
  │  frame: foo │  ← вызвала bar()
  │  frame: bar │  ← вызвала baz()
  │  frame: baz │  ← здесь yield()
  └─────────────┘
  Стек сохранён целиком — можно yield() из любой глубины вызовов
```

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
| Управление                 | Планировщик    | Планировщик    | Вызывающий код        |
| Поддержка в стандарте      | Нет            | Нет            | C++20                 |
| Context switch             | Asm (регистры) | Asm (регистры) | Обычный вызов функции |

### Корутины C++20: внутреннее устройство

Корутина в C++ — это функция, тело которой может быть приостановлено и возобновлено. 
Компилятор трансформирует её в машину состояний.

Что делает компилятор

```cpp
// Исходный код:
Task my_coroutine() {
    int x = 10;
    co_await some_awaitable();   // точка приостановки 1
    int y = x + 20;
    co_await another_awaitable(); // точка приостановки 2
    co_return x + y;
}
```

Компилятор генерирует примерно следующее:

```cpp
// То во что компилятор превращает корутину (псевдокод):
struct my_coroutine_frame {
    // Локальные переменные (переживают приостановку)
    int x, y;

    // Объект promise (управляет возвратом значений)
    Task::promise_type promise;

    // Текущая точка приостановки
    int suspend_point = 0;

    // Функция возобновления
    void resume() {
        switch (suspend_point) {
        case 0:
            x = 10;
            // co_await some_awaitable():
            if (!some_awaitable().await_ready()) {
                suspend_point = 1;
                some_awaitable().await_suspend(this_handle);
                return; // ← возвращаем управление вызывающему
            }
        case 1:
            y = x + 20;
            // co_await another_awaitable():
            if (!another_awaitable().await_ready()) {
                suspend_point = 2;
                another_awaitable().await_suspend(this_handle);
                return;
            }
        case 2:
            promise.return_value(x + y); // co_return
            // уничтожить frame
        }
    }
};
```

### Жизненный цикл coroutine frame

```text
my_coroutine() вызвана:
  1. Аллоцируется frame в куче (operator new)
  2. Создаётся promise_type внутри frame
  3. promise.get_return_object() → возвращает Task вызывающему
  4. Корутина стартует (или сразу приостанавливается — initial_suspend)

co_await:
  5. Проверяется await_ready() — можно ли продолжить без ожидания
  6. Если нет — await_suspend() → управление уходит вызывающему
  7. Где-то позже: handle.resume() → корутина продолжается
  8. await_resume() → возвращает результат ожидания

co_return:
  9. promise.return_value(x) сохраняет результат
  10. final_suspend → последняя точка приостановки
  11. Frame уничтожается (если не приостановлена на final_suspend)
```

### Оператор `co_await`

`co_await expr` работает через концепцию Awaitable — объект должен иметь три метода:

```cpp
struct MyAwaitable {
    // Можно ли продолжить без приостановки?
    // true  → не приостанавливаемся, сразу берём результат
    // false → приостанавливаемся
    bool await_ready() { return false; }

    // Вызывается при приостановке.
    // handle — это ссылка на текущую корутину.
    // Можно: запустить async-операцию, сохранить handle для resume позже
    void await_suspend(std::coroutine_handle<> handle) {
        // Например: зарегистрировать в epoll и сохранить handle
        thread_pool.post([handle]() mutable {
            do_work();
            handle.resume(); // разбудить корутину когда готово
        });
    }

    // Возвращает результат co_await — то что присваивается переменной
    int await_resume() { return result_; }

    int result_;
};

// Использование:
Task example() {
    int x = co_await MyAwaitable{};  // x = await_resume()
}
```

Стандартные awaitables

```cpp
// Никогда не приостанавливается:
struct std::suspend_never {
    bool await_ready() { return true; }
    void await_suspend(std::coroutine_handle<>) {}
    void await_resume() {}
};

// Всегда приостанавливается:
struct std::suspend_always {
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<>) {}
    void await_resume() {}
};
```

### Ключевое слово `co_return`

`co_return` сигнализирует о завершении корутины и передаёт результат через `promise`:

```cpp
// co_return value → вызывает promise.return_value(value)
Task<int> compute() {
    int result = 42;
    co_return result;       // promise.return_value(42)
}

// co_return без значения → вызывает promise.return_void()
Task<void> do_work() {
    do_something();
    co_return;              // promise.return_void()
}
```

Без `co_return` (корутина завалилась с конца) — компилятор вставляет `promise.return_void()` автоматически.

### Promise type: связующее звено

`promise_type` — это класс, который управляет поведением корутины. Он вкладывается в тип возвращаемого значения:

```cpp
template<typename T>
struct Task {
    // Компилятор ищет Task::promise_type
    struct promise_type {
        T value_;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        // Приостановиться сразу при старте? (обычно нет)
        std::suspend_never  initial_suspend() { return {}; }

        // Приостановиться при завершении? (обычно да — чтобы вызывающий мог забрать результат)
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