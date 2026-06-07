## Понятия lock-free и wait-free. Lock-free стек (стек Трайбера) и его реализация.

### Lock-free и Wait-free: определения

Это гарантии прогресса — что происходит если поток приостановлен в середине операции.

```text
Blocking (mutex):   Один поток держит блокировку → все остальные стоят
                    Прогресс системы зависит от одного потока

Lock-free:          Хотя бы ОДИН поток из всех всегда делает прогресс
                    Отдельный поток может застрять, система — нет

Wait-free:          КАЖДЫЙ поток завершает операцию за конечное число шагов
                    Никто не может застрять — самая сильная гарантия
```

Иерархия гарантий

```text
    Wait-free  ⊂  Lock-free  ⊂  Obstruction-free  ⊂  Blocking
    (сильнее)                                          (слабее)
```

**Lock-free** — использует атомарные операции (CAS) вместо мьютексов. Если один поток завис — другие продолжают работу. Но один конкретный поток может бесконечно проигрывать CAS-гонку.

**Wait-free** — каждая операция завершается за $\mathcal{O}(N)$ шагов где N — число потоков. На практике редко реализуется из-за сложности.

### Стек Трайбера (Treiber Stack, 1986)

Классический lock-free стек на односвязном списке. 
Идея — использовать CAS (Compare-And-Swap) для атомарного обновления указателя на вершину.

#### Структура

```text
top ──► [ Node: 3 ] ──► [ Node: 7 ] ──► [ Node: 1 ] ──► nullptr
          (вершина)
```

Операции:
* `push`: создать новый узел, CAS-ом поставить его как новый top
* `pop`: прочитать top, CAS-ом переставить top на следующий узел

### Реализация

```cpp
#include <atomic>
#include <optional>
#include <memory>

template<typename T>
class TreiberStack {
    struct Node {
        T                    value;
        std::atomic<Node*>   next{nullptr};

        explicit Node(T val) : value(std::move(val)) {}
    };

    std::atomic<Node*> top_{nullptr};

public:
    // Push: O(1) амортизированно, lock-free
    void push(T value) {
        Node* new_node = new Node(std::move(value));

        // CAS-loop: пробуем поставить new_node как новый top
        Node* current_top = top_.load(std::memory_order_relaxed);
        do {
            new_node->next.store(current_top, std::memory_order_relaxed);
            // Если top не изменился — меняем на new_node
            // Если изменился  — current_top обновляется, повторяем
        } while (!top_.compare_exchange_weak(
                     current_top,           // ожидаемое значение
                     new_node,              // новое значение
                     std::memory_order_release,  // успех
                     std::memory_order_relaxed   // неудача
                 ));
    }

    // Pop: O(1) амортизированно, lock-free
    std::optional<T> pop() {
        Node* current_top = top_.load(std::memory_order_acquire);

        while (current_top != nullptr) {
            Node* next = current_top->next.load(std::memory_order_relaxed);

            // Если top не изменился — переставляем на next
            if (top_.compare_exchange_weak(
                    current_top,            // ожидаемое
                    next,                   // новое
                    std::memory_order_acquire,
                    std::memory_order_acquire)) {
                // CAS успешен — мы забрали current_top
                T value = std::move(current_top->value);
                delete current_top;         // ← ОПАСНО: проблема ABA!
                return value;
            }
            // CAS неудачен: current_top обновлён, повторяем
        }

        return std::nullopt; // стек пуст
    }

    ~TreiberStack() {
        // Очистка: pop всё
        while (pop().has_value()) {}
    }
};
```

Как работает push при конкуренции

```text
Начало: top → [7] → [1] → null

Поток A: new_node = Node(3)
         new_node->next = [7]   (current_top = [7])
         CAS(top, [7], [3])...

         Поток B (параллельно): push(5)
         new_node_B = Node(5)
         CAS(top, [7], [5]) → УСПЕХ
         top → [5] → [7] → [1]

         Поток A: CAS(top, [7], [3]) → НЕУДАЧА
                  (top теперь [5], не [7])
         current_top = [5]  (обновился автоматически)
         new_node->next = [5]
         CAS(top, [5], [3]) → УСПЕХ
         top → [3] → [5] → [7] → [1]
```

Поток A повторил CAS — это и есть lock-free: система прогрессирует (поток B успешно сделал push), поток A повторяет, но не блокирует никого.
