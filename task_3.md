## Проблема доступа к общим данным. Примитивы синхронизации: std::mutex, std::shared_mutex, std::recursive_mutex, std::unique_lock, std::shared_lock. Функция std::lock.

### Проблема доступа к общим данным
Когда несколько потоков одновременно обращаются к одной переменной, причём хотя бы один поток пишет в неё, 
возникает гонка данных (data race). Это undefined behavior в C++: результат зависит от порядка планирования потоков и может быть непредсказуемым.

```cpp
int counter = 0;

void increment() {
    for (int i = 0; i < 100000; ++i)
        counter++;  // НЕ атомарно! Это три инструкции: read → add → write
}

// Два потока одновременно → counter < 200000 (потеря обновлений)
```

### `std::mutex`

`std::mutex `— объект взаимного исключения: только один поток может захватить его (вызвать `lock()`) в каждый момент времени. 
Остальные блокируются до освобождения.

```cpp
#include <mutex>
std::mutex mtx;
int counter = 0;

void safe_increment() {
    for (int i = 0; i < 100000; ++i) {
        mtx.lock();
        counter++;
        mtx.unlock();
    }
}
```

Прямой вызов `lock()/unlock()` опасен: если между ними бросится исключение, 
мьютекс никогда не освободится (**дедлок**). Поэтому используют RAII-обёртки.

### `std::unique_lock` и `std::lock_guard`

`std::lock_guard` — самая простая обёртка. Захватывает мьютекс в конструкторе, освобождает в деструкторе. Нельзя освободить досрочно.

`std::unique_lock` — гибкая обёртка. 

Позволяет:
* освобождать и повторно захватывать мьютекс (`unlock()/lock()`)
* захватывать с ожиданием (`try_lock()`, `try_lock_for()`)
* работать с `std::condition_variable`
* передавать владение (`movable`)

```cpp
std::mutex mtx;
int counter = 0;

void safe_increment() {
    for (int i = 0; i < 100000; ++i) {
        std::unique_lock<std::mutex> lock(mtx); // захватили
        counter++;
    } // деструктор → освобождение, даже при исключении
}
```

### `std::shared_mutex` — читатели и писатели

Обычный мьютекс не различает чтение и запись: даже два потока, которые только читают, 
блокируют друг друга. `std::shared_mutex` решает эту проблему через два режима захвата:

| Режим                | Захват        | Кто блокируется |
|----------------------|---------------|-----------------|
| Shared (читатель)    | `shared_lock` | Только писатели |
| Exclusive (писатель) | `unique_lock` | Все             |

```cpp
#include <shared_mutex>
std::shared_mutex rw_mtx;
std::map<int, std::string> data;

// Чтение — много потоков одновременно
std::string read(int key) {
    std::shared_lock lock(rw_mtx);  // shared захват
    return data.at(key);
}

// Запись — только один поток
void write(int key, std::string value) {
    std::unique_lock lock(rw_mtx);  // exclusive захват
    data[key] = value;
}
```

### `std::recursive_mutex`

Обычный `std::mutex` нельзя захватить дважды из одного потока — это дедлок. 
`std::recursive_mutex` позволяет одному потоку захватывать его несколько раз (с подсчётом), 
освобождая столько раз, сколько захватывал.

```cpp
std::recursive_mutex rmtx;

void foo() {
    std::unique_lock lock(rmtx);
    bar();  // bar тоже захватывает rmtx — с обычным mutex был бы дедлок
}

void bar() {
    std::unique_lock lock(rmtx); // OK — тот же поток, счётчик = 2
    // ...
}                                // счётчик = 1
// foo завершается → счётчик = 0, мьютекс освобождён
```

### `std::shared_lock`

Пара к `std::unique_lock` для `std::shared_mutex`. Захватывает мьютекс в shared-режиме (для чтения). 
Несколько потоков могут держать shared_lock одновременно.

```cpp
std::shared_lock<std::shared_mutex> lock(rw_mtx);
// эквивалентно: rw_mtx.lock_shared()
```

`std::lock` — захват нескольких мьютексов без дедлока

```
Поток 1: lock(A) → ждёт B
Поток 2: lock(B) → ждёт A  ← дедлок!
```

`std::lock(m1, m2, ...)` захватывает все переданные мьютексы атомарно, 
используя алгоритм без дедлока (при неудаче освобождает уже захваченные и пробует снова в другом порядке).

```cpp
std::mutex mtx_a, mtx_b;

void transfer(Account& from, Account& to, int amount) {
    // Безопасно: std::lock гарантирует отсутствие дедлока
    std::unique_lock lock_a(mtx_a, std::defer_lock); // не захватываем сразу
    std::unique_lock lock_b(mtx_b, std::defer_lock);
    
    std::lock(lock_a, lock_b); // захватываем оба атомарно
    
    from.balance -= amount;
    to.balance += amount;
}   // оба lock освобождаются в деструкторе
```

`std::defer_lock` — тег, говорящий unique_lock: «создай обёртку, но мьютекс пока не захватывай».
В C++17 также появился std::scoped_lock, который делает то же самое одной строкой:

```cpp
std::scoped_lock lock(mtx_a, mtx_b); // C++17: захват без дедлока, RAII
```