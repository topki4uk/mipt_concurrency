## Понятие data race. Store buffering. Класс std::atomic и его основные методы. Спинлок. Реализация mutex через спинлок.

### Data Race

**Data race** — ситуация, когда два потока одновременно обращаются к одной памяти, 
хотя бы одно обращение — запись, и нет синхронизации между ними. Результат — неопределённое поведение (UB) по стандарту C++.

```cpp
int counter = 0;

void thread1() { counter++; } // читает, прибавляет, пишет — 3 операции!
void thread2() { counter++; } // то же самое параллельно → data race
```

`counter++` на уровне машинного кода — это три инструкции:

```asm
MOV  eax, [counter]   ; читаем
ADD  eax, 1           ; прибавляем
MOV  [counter], eax   ; пишем
```

Между любыми двумя из них планировщик может переключить поток — и оба потока запишут одно и то же значение, потеряв один инкремент.

### Store Buffering

Современные процессоры не пишут в память сразу — запись сначала попадает в store buffer (буфер записи) 
конкретного ядра и только потом сбрасывается в общую память (cache coherency). Из-за этого два ядра могут видеть 
разные «версии» памяти:

```text
// Начальное состояние: x = 0, y = 0

// Поток 1 (ядро 1)       // Поток 2 (ядро 2)
x = 1;                    y = 1;
int r1 = y;               int r2 = x;
```

Интуитивно кажется, что `r1==1 || r2==1` всегда. Но store buffering допускает результат `r1==0 && r2==0`: 
оба ядра записали в свой буфер, но прочитали из памяти до того, как чужая запись туда дошла.

Это называется **слабой моделью памяти**. Чтобы её запретить, нужны барьеры памяти (memory barriers / fences), 
которые заставляют сбросить store buffer.

### `std::atomic`

`std::atomic<T>` — обёртка, гарантирующая что операции над `T` атомарны и имеют чётко определённую видимость между потоками. 
Никакого UB, никакого store buffering без контроля.

#### Основные методы

```cpp
std::atomic<int> a{0};

a.store(42);           // атомарная запись
int v = a.load();      // атомарное чтение
int old = a.exchange(10);  // атомарно: записать 10, вернуть старое

// Compare-And-Swap (CAS) — основа всех lock-free алгоритмов
int expected = 5;
bool ok = a.compare_exchange_strong(expected, 99);
// Если a == expected → записать 99, вернуть true
// Если a != expected → записать текущее значение в expected, вернуть false

// Арифметика (только для целых)
a.fetch_add(1);  // атомарный ++, возвращает старое значение
a.fetch_sub(1);  // атомарный --
```

### `compare_exchange_weak` vs `strong`

`weak` может ложно вернуть `false` (spurious failure) даже если `a == expected` — допускается на архитектурах 
с LL/SC (ARM). Зато работает чуть быстрее в цикле:

```cpp
// weak — для цикла (spurious failure не страшен)
while (!a.compare_exchange_weak(expected, new_val)) {}

// strong — для одиночной попытки (гарантирован корректный результат)
if (a.compare_exchange_strong(expected, new_val)) { /* точно сработало */ }
```

### Memory order

Каждая операция принимает необязательный параметр `memory_order`:

| `memory_order` | 	Что гарантирует                                          |
|----------------|-----------------------------------------------------------|
| `relaxed`      | 	Только атомарность, никаких барьеров                     |
| `acquire`      | 	Всё написанное до `release` в другом потоке — видно      |
| `release`      | Все мои предыдущие записи видны тому, кто сделает acquire |
| `seq_cst`      | Полный барьер, глобальный порядок (по умолчанию)          |

```cpp
// Типичная пара: producer/consumer без мьютекса
std::atomic<bool> ready{false};
int data = 0;

// Производитель
data = 42;
ready.store(true, std::memory_order_release); // "публикую" data

// Потребитель
while (!ready.load(std::memory_order_acquire)); // "подписываюсь"
assert(data == 42); // гарантированно 42
```

### Спинлок

Спинлок — простейший мьютекс: поток не засыпает, а крутится в цикле (spin), 
пока не захватит замок. Реализуется через `std::atomic_flag` — единственный тип, 
гарантированно lock-free на любой платформе:

```cpp
#include <atomic>

class Spinlock {
public:
    void lock() {
        // test_and_set: атомарно ставит флаг в true, возвращает старое значение
        // Если вернул false — мы захватили. Если true — крутимся дальше.
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // Подсказка процессору: мы в spin-wait, можно снизить приоритет
            // На x86 это инструкция PAUSE — уменьшает энергопотребление
            // и не мешает другому гиперпотоку на том же ядре
        }
    }

    void unlock() {
        flag_.clear(std::memory_order_release); // сбросить флаг
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};
```

`memory_order_acquire` в `lock()` и `memory_order_release` в `unlock()` вместе образуют acquire-release 
пару — гарантию, что все записи до `unlock()` видны после `lock()` в другом потоке.

### Реализация `mutex` через спинлок

Чистый спинлок плох при долгом ожидании — сжигает CPU. Настоящий `mutex` добавляет передачу управления ОС (`yield` или сон):

```cpp
#include <atomic>
#include <thread>

class Mutex {
public:
    void lock() {
        int spin_count = 0;
        while (flag_.test_and_set(std::memory_order_acquire)) {
            ++spin_count;
            if (spin_count < 16) {
                // Фаза 1: активный спин (надеемся, что замок скоро освободится)
                // _mm_pause() / __asm__("yield") на ARM
            } else {
                // Фаза 2: уступаем квант времени ОС (перестаём жечь CPU)
                std::this_thread::yield();
            }
        }
    }

    void unlock() {
        flag_.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};
```

### Сравнение реализаций

|                   | Спинлок                  | Mutex (гибрид)           | std::mutex            |
|-------------------|--------------------------|--------------------------|-----------------------|
| Короткое ожидание | ✅ очень быстро           | ✅ быстро                 | ✅ (futex в userspace) |
| Долгое ожидание   | ❌ 100% CPU               | ✅ yield снижает нагрузку | ✅ поток засыпает      |
| Пробуждение       | мгновенное               | мгновенное               | overhead syscall      |
| Применение        | spinlock в ядре ОС, NUMA | общий случай             | общий случай          |