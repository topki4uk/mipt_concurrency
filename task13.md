## Примитивы Future и Promise. Применение для асинхронного выполнения задач. Callback hell. Future continuations, chaining и combining.

### Future и Promise: идея

**Promise** — это «обещание» поставить результат в будущем. 

**Future** — «квитанция», по которой можно этот результат получить. Это два конца одного канала передачи данных между потоками:

```text
Поток-производитель          Поток-потребитель

  Promise<int> p;   ←──────→  Future<int> f = p.get_future()
  
  // вычисляет...             // ждёт...
  p.set_value(42)   ══════►   int x = f.get() // = 42
```

`Promise` — пишущий конец, Future — читающий. `get()` блокируется до тех пор, пока `set_value()` не будет вызван.

### `std::future` и `std::promise`

```cpp
#include <future>
#include <thread>

std::promise<int> p;
std::future<int> f = p.get_future();

std::thread producer([&p] {
    int result = heavy_computation(); // долго считаем
    p.set_value(result);              // публикуем результат
});

// Главный поток занимается другими делами...
do_other_work();

// Когда результат нужен — блокируемся
int result = f.get(); // ждёт если ещё не готово

producer.join();
```

#### Передача исключений

`Promise` умеет передавать не только значение, но и исключение:

```cpp
try {
    p.set_value(compute());
} catch (...) {
    p.set_exception(std::current_exception()); // упаковываем исключение
}

// В потребителе:
try {
    int x = f.get(); // бросит исключение если было set_exception
} catch (const std::exception& e) {
    // обрабатываем
}
```

#### `std::async` — удобная обёртка

Создаёт поток и возвращает future автоматически:

```cpp
std::future<int> f = std::async(std::launch::async, [] {
    return heavy_computation();
});

// Потом:
int result = f.get();
```

### Callback Hell

До появления `Future/Promise` асинхронный код писали через колбэки — функции, 
которые вызываются по завершении операции. При цепочке зависимых операций это приводит к callback hell:

```cpp
// Читаем файл → парсим → запрашиваем БД → обрабатываем → пишем ответ

read_file("data.txt", [](std::string content) {
    parse(content, [](ParsedData data) {
        db_query(data.id, [](DbResult row) {
            process(row, [](Result r) {
                write_response(r, [](bool ok) {
                    if (!ok) {
                        // обработка ошибки на 5-м уровне вложенности
                    }
                    // ещё логика...
                });
            });
        });
    });
});
// "Pyramid of doom" — код уходит вправо
```

Проблемы callback hell:
* Код читается снизу вверх и справа налево — нечитаемо

* Обработка ошибок в каждом колбэке отдельно

* Стек вызовов разорван — сложно отлаживать

* Нельзя использовать `try/catch` через границы колбэков

### Future Continuations: `.then()`

Решение — продолжения (continuations): цепочка `.then()`, где каждый шаг получает результат предыдущего. 
В стандартном C++ это появилось только в `std::experimental`, поэтому используют библиотеки (folly, boost) 
или реализуют сами.

**Идея:**

```cpp
// Вместо pyramid of doom — линейная цепочка:
read_file("data.txt")
    .then([](std::string content)  { return parse(content); })
    .then([](ParsedData data)      { return db_query(data.id); })
    .then([](DbResult row)         { return process(row); })
    .then([](Result r)             { return write_response(r); })
    .catch_([](std::exception& e)  { log_error(e); }); // одна точка ошибок
```

Каждый `.then()` принимает future, ждёт его результата и передаёт в следующий шаг.

### Простая реализация `.then()`

```cpp
template<typename T>
struct Future {
    std::shared_future<T> inner_;

    template<typename F>
    auto then(F&& func) -> Future<std::invoke_result_t<F, T>> {
        using R = std::invoke_result_t<F, T>;

        // Запускаем новый поток, который ждёт нас и применяет func
        auto shared = inner_;
        return Future<R>{
            std::async(std::launch::async, [shared, func] {
                T value = shared.get();   // ждём предыдущего
                return func(value);       // применяем следующий шаг
            }).share()
        };
    }
};
```

### Chaining: последовательные зависимости

**Chaining** — когда результат одного future является входом следующего. Это именно то, что делает `.then()`:

```cpp
Future<string>  f1 = async_read("file.txt");
Future<Data>    f2 = f1.then(parse);           // f2 зависит от f1
Future<Result>  f3 = f2.then(process);         // f3 зависит от f2
Future<bool>    f4 = f3.then(write_response);  // f4 зависит от f3

f4.get(); // ждём конца всей цепочки
```

```text
f1 ──► f2 ──► f3 ──► f4
(read) (parse)(proc)(write)
  последовательно, но без блокировки потоков
```

### Combining: параллельные зависимости

**Combining** — когда нужно дождаться нескольких futures одновременно.

#### `when_all` — ждём всех

```cpp
Future<A> fa = async_fetch_user();
Future<B> fb = async_fetch_orders();
Future<C> fc = async_fetch_settings();

// Запускаются параллельно, ждём все три
when_all(fa, fb, fc)
    .then([](A a, B b, C c) {
        return build_response(a, b, c);
    });
```

```text
fa ──────────────────┐
fb ────────┐         ├──► then(build_response)
fc ──────────────────┘
  параллельно        when_all ждёт последнего
```

#### `when_any` — ждём первого

```cpp
Future<Result> f1 = query_server_1();
Future<Result> f2 = query_server_2(); // дублирующий запрос

// Берём тот, кто ответил быстрее
when_any(f1, f2)
    .then([](Result r) {
        use(r); // первый результат
    });
```

#### Реализация `when_all`

```cpp
template<typename... Futures>
auto when_all(Futures&&... futures) {
    return std::async(std::launch::async,
        [](auto... fs) {
            return std::make_tuple(fs.get()...); // ждём каждого
        },
        std::forward<Futures>(futures)...
    );
}
```