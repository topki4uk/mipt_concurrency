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

### Полная картина работы

```text
Promise<int> p;
Future<int>  f = p.get_future();

        SharedState (в куче):
        ┌─────────────────────┐
        │ ready = false       │
        │ value = nullopt     │
        │ mutex + condvar     │
p ─────►│                     │◄───── f
        └─────────────────────┘

p.set_value(42):
  lock(mutex)
  value = 42, ready = true
  condvar.notify_all()
  unlock

f.get():
  lock(mutex)
  condvar.wait(..., ready == true)  // просыпается
  return move(*value)               // = 42
  state_ = nullptr                  // future стал невалидным
```

### Неудобства `std::future/std::promise`

* `get()` можно вызвать только один раз
  
После `get()` future становится невалидным. Нельзя дать несколько потребителей на один результат:

```cpp
auto f = p.get_future();
int x = f.get(); // ок
int y = f.get(); // исключение: no shared state!

// Решение — std::shared_future (но нужно явно конвертировать):
std::shared_future<int> sf = p.get_future().share();
int x = sf.get(); // ок
int y = sf.get(); // тоже ок
```

* Нет `.then()` в стандарте

Continuations не вошли в C++20 (только в `std::experimental`). 
Для chaining нужны библиотеки или ручной `std::async`:

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
// Каждый .then() создаёт новый поток — дорого
```

* Каждый `std::async` создаёт поток

Нет встроенного пула потоков — `std::async(launch::async, ...)` гарантированно создаёт новый поток:

```cpp
// Это создаёт 1000 потоков:
for (int i = 0; i < 1000; i++) {
    futures.push_back(std::async(std::launch::async, task));
}
```

* `std::async` с `launch::deferred` — ловушка

```cpp
auto f = std::async(std::launch::deferred, heavy_task);
// Задача НЕ запущена — она запустится только при f.get()
// Это не асинхронность — это просто ленивый вызов!

f.get(); // только здесь запускается heavy_task — в текущем потоке
```

* Нет отмены (cancellation)

Нельзя отменить вычисление, которое уже запущено:

```cpp
auto f = std::async(task);
// Хочу отменить — но не могу!
// f.cancel() — не существует
// Поток будет работать до конца
```

* `broken_promise` — молчаливая потеря результата

Если `promise` уничтожается без `set_value` — `future::get()` бросает исключение. Это легко получить случайно:

```cpp
std::future<int> f;
{
    std::promise<int> p;
    f = p.get_future();
    // забыли вызвать p.set_value()
} // p уничтожен → broken promise

f.get(); // бросает std::future_error: broken promise
```

### Сводка недостатков

| Проблема                    | Обходной путь                                |
|-----------------------------|----------------------------------------------|
| `get()` один раз            | `shared_future`                              |
| Нет `.then()`               | `folly::Future`, `boost::future`, coroutines |
| Новый поток на каждый async | Тред-пул + promise вручную                   |
| Нет отмены                  | `std::stop_token` (C++20) + ручная проверка  |
| deferred не асинхронный     | Явно указывать `launch::async`               |
| `broken_promise`            | RAII-обёртка над promise                     |