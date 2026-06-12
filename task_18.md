## Понятия lock-free и wait-free. Lock-free стек (стек Трайбера) и его реализация.

### Lock-free и Wait-free: определения

Это гарантии прогресса — что происходит если поток приостановлен в середине операции.

`Lock-free` (неблокирующие) методы гарантируют, что хотя бы один поток продолжит работу даже в условиях высокой конкуренции.

`Wait-free` алгоритмы идут дальше, гарантируя завершение операций за предсказуемое время даже при сбоях отдельных потоков.

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
    void push(T value) {
        Node* new_node = new Node(std::move(value));

        Node* current_top = top_.load(std::memory_order_relaxed);
        do {
            new_node->next.store(current_top, std::memory_order_relaxed);
        } while (!top_.compare_exchange_weak(
                     current_top,
                     new_node,
                     std::memory_order_release,
                     std::memory_order_relaxed
                 ));
    }

    std::optional<T> pop() {
        Node* current_top = top_.load(std::memory_order_acquire);

        while (current_top != nullptr) {
            Node* next = current_top->next.load(std::memory_order_relaxed);

            if (top_.compare_exchange_weak(
                    current_top,
                    next,
                    std::memory_order_acquire,
                    std::memory_order_acquire)) {
                T value = std::move(current_top->value);
                delete current_top;
                return value;
            }
        }

        return std::nullopt;
    }

    ~TreiberStack() {
        while (pop().has_value()) {}
    }
};
```
