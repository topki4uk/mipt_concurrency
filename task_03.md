## Проблема доступа к общим данным. Примитивы синхронизации: std::mutex, std::shared_mutex, std::recursive_mutex, std::unique_lock, std::shared_lock. Функция std::lock.

### Проблема доступа к общим данным

```cpp
int counter = 0;

void increment() {
    for (int i = 0; i < 100000; ++i)
        counter++;
}
```

### `std::mutex`

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

### `std::unique_lock` и `std::lock_guard`

`std::lock_guard` — самая простая обёртка.

`std::unique_lock` — гибкая обёртка. 

```cpp
std::mutex mtx;
int counter = 0;

void safe_increment() {
    for (int i = 0; i < 100000; ++i) {
        std::unique_lock<std::mutex> lock(mtx);
        counter++;
    }
}
```

### `std::shared_mutex` — читатели и писатели

| Режим                | Захват        | Кто блокируется |
|----------------------|---------------|-----------------|
| Shared (читатель)    | `shared_lock` | Только писатели |
| Exclusive (писатель) | `unique_lock` | Все             |

```cpp
#include <shared_mutex>
std::shared_mutex rw_mtx;
std::map<int, std::string> data;

std::string read(int key) {
    std::shared_lock lock(rw_mtx);
    return data.at(key);
}

void write(int key, std::string value) {
    std::unique_lock lock(rw_mtx);
    data[key] = value;
}
```

### `std::recursive_mutex`

```cpp
std::recursive_mutex rmtx;

void foo() {
    std::unique_lock lock(rmtx);
    bar();
}

void bar() {
    std::unique_lock lock(rmtx);
    // ...
}
```

### `std::shared_lock`

Несколько потоков могут держать shared_lock одновременно.

```cpp
std::shared_lock<std::shared_mutex> lock(rw_mtx);
// эквивалентно: rw_mtx.lock_shared()
```

### `std::lock` — захват нескольких мьютексов без дедлока

```
Поток 1: lock(A) → ждёт B
Поток 2: lock(B) → ждёт A  ← дедлок!
```

`std::lock(m1, m2, ...)` захватывает все переданные мьютексы атомарно, 
используя алгоритм без дедлока (при неудаче освобождает уже захваченные и пробует снова в другом порядке).

```cpp
std::mutex mtx_a, mtx_b;

void transfer(Account& from, Account& to, int amount) {
    std::unique_lock lock_a(mtx_a, std::defer_lock);
    std::unique_lock lock_b(mtx_b, std::defer_lock);
    
    std::lock(lock_a, lock_b);
    
    from.balance -= amount;
    to.balance += amount;
}   // оба lock освобождаются в деструкторе
```

В C++17 также появился `std::scoped_lock`, который делает то же самое одной строкой:

```cpp
std::scoped_lock lock(mtx_a, mtx_b); // C++17: захват без дедлока, RAII
```