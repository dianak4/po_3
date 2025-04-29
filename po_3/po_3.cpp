#include <iostream>
#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <random>
#include <atomic>
#include <chrono>
#include <iomanip>

using namespace std;
using namespace std::chrono;

struct Task {
    int priority;
    function<void()> func;
    steady_clock::time_point enqueueTime; // для підрахунку часу очікування задачі в черзі
    bool operator<(const Task& other) const { // так як пріоритетна черга працює як max-heap,
        return priority > other.priority;  // а нам потрібно аби менший час виконання мав вищий пріоритет, інвертуємо
    }
};

class ThreadPool {
public:
    ThreadPool(size_t threadCount) : stop(false), immediateStop(false) { // час зупинки, скидання
        idleTimesMs.resize(threadCount, 0);
        for (size_t i = 0; i < threadCount; ++i)
            workers.emplace_back([this, i] { workerLoop(i); }); // лямбда. Додати потік у  workers, виконувати функцію workerLoop() 
    }

    ~ThreadPool() {
        shutdown(true);
    }

    void addTask(int duration, function<void()> taskFunc) {
        auto now = steady_clock::now(); // підрахунок очікування

        lock_guard<mutex> lock(controlMutex); // захист від одночасного доступу до stop та immediateStop
        if (stop) return; // Завдання більше не приймаються

        {
            lock_guard<mutex> qlock(queueMutex); // захист від одночасного доступу до taskQueue
            taskQueue.push(Task{ duration, move(taskFunc), now });
        }
        cond.notify_one(); // сповіщення, що є нове завдання
    }

    void shutdown(bool waitForTasks) {
        {
            lock_guard<mutex> lock(controlMutex);
            stop = true; // прапорець зупинки
            immediateStop = !waitForTasks; // якщо не чекаємо на завершення, то зупиняємо всі потоки
        }

        cond.notify_all();
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
    }

    void pause() {
        paused = true;
    }

    void resume() {
        paused = false;
        cond.notify_all();
    }

    bool isPaused() const { return paused; }
    bool isStopped() const { return stop; }

    size_t queueSize() {
        lock_guard<mutex> lock(queueMutex);
        return taskQueue.size();
    }

    size_t workerCount() const { return workers.size(); }

    atomic<int> totalTasks{ 0 };
    atomic<long long> execTimeMs{ 0 };
    atomic<long long> waitTimeMs{ 0 };
    atomic<long long> totalQueueLengths{ 0 };
    atomic<int> queueSamples{ 0 };
    vector<long long> idleTimesMs;

private:
    void workerLoop(int index) {
        while (true) {
            Task task;

            auto idleStart = steady_clock::now();

            {
                unique_lock<mutex> lock(queueMutex);
                cond.wait(lock, [this] {
                    return stop || (!paused && !taskQueue.empty());
                    });

                auto idleEnd = steady_clock::now();
                idleTimesMs[index] += duration_cast<milliseconds>(idleEnd - idleStart).count();

                if (stop && (immediateStop || taskQueue.empty())) return;
                if (paused || taskQueue.empty()) continue;

                task = move(taskQueue.top());
                taskQueue.pop();
            }

            auto start = steady_clock::now();
            waitTimeMs += duration_cast<milliseconds>(start - task.enqueueTime).count();

            task.func();

            auto end = steady_clock::now();
            execTimeMs += duration_cast<milliseconds>(end - start).count();
            totalTasks++;
        }
    }

    vector<thread> workers;
    priority_queue<Task> taskQueue;
    mutex queueMutex, controlMutex;
    condition_variable cond;

    bool stop;
    bool immediateStop;
    atomic<bool> paused;
};

mutex coutMutex;

void simulateTask(int duration, int id) {
    {
        lock_guard<mutex> lock(coutMutex);
        cout << "[Task " << id << "] Start (" << duration << "s)\n";
    }

    this_thread::sleep_for(seconds(duration));

    {
        lock_guard<mutex> lock(coutMutex);
        cout << "[Task " << id << "] Done\n";
    }
}

int main() {
    ThreadPool pool(4);
    atomic<int> idCounter{ 1 };

    vector<thread> producers; //створюють і додають задачі до пулу
    for (int i = 0; i < 3; ++i) {
        producers.emplace_back([&pool, &idCounter] {
            srand(time(nullptr) + idCounter); // унікальна ініціалізація для кожного потоку
            for (int j = 0; j < 5; ++j) {
                int dur = 5 + rand() % 6;
                int id = idCounter++;
                pool.addTask(dur, [=] { simulateTask(dur, id); });
                this_thread::sleep_for(milliseconds(300));
            }
            });
    }

    thread monitor([&pool] { // відстежує розмір черги задач у пулі. накопичує статистику для оцінки середнього завантаження черги
        while (!pool.isStopped()) {
            pool.totalQueueLengths += pool.queueSize();
            pool.queueSamples++;
            this_thread::sleep_for(milliseconds(200));
        }
        });

    for (auto& t : producers) t.join(); //чекати завершення всіх продюсерів

    this_thread::sleep_for(seconds(5));
    cout << "\n[Main] Pausing pool...\n";
    pool.pause();

    this_thread::sleep_for(seconds(5));
    cout << "\n[Main] Resuming pool...\n";
    pool.resume();

    this_thread::sleep_for(seconds(10));
    pool.shutdown(true);

    cout << "\n[Main] Shutting down...\n";
    monitor.join();

    int done = pool.totalTasks;

    cout << "\n--- Stats ---\n";
    cout << "Total tasks: " << done << "\n";

    if (done > 0) {
        cout << fixed << setprecision(2);
        cout << "Avg exec time: " << (pool.execTimeMs / 1000.0 / done) << "s\n";
        cout << "Avg wait time: " << (pool.waitTimeMs / 1000.0 / done) << "s\n";
    }

    if (pool.queueSamples > 0)
        cout << "Avg queue size: " << (pool.totalQueueLengths / pool.queueSamples) << "\n";

    cout << "Thread count: " << pool.workerCount() << "\n";

    long long totalIdleMs = 0;
    for (size_t i = 0; i < pool.workerCount(); ++i) {
        totalIdleMs += pool.idleTimesMs[i];
    }
    cout << "\nTotal idle time (all threads combined): " << fixed << setprecision(2)
        << (totalIdleMs / 1000.0) << "s\n";

    return 0;
}