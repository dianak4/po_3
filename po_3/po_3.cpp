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

// Task structure
struct Task {
    int priority;
    std::function<void()> func;
    std::chrono::steady_clock::time_point enqueueTime;
    bool operator<(const Task& other) const {
        return priority > other.priority;
    }
};

class ThreadPool {
public:
    ThreadPool(size_t threadCount) : stop(false), immediateStop(false) {
        for (size_t i = 0; i < threadCount; ++i)
            workers.emplace_back([this] { workerLoop(); });
    }

    ~ThreadPool() {
        shutdown(true);
    }

    void addTask(int duration, std::function<void()> taskFunc) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(queueMutex);
        taskQueue.push(Task{ duration, std::move(taskFunc), now });
        cond.notify_one();
    }

    void shutdown(bool waitForTasks) {
        {
            std::lock_guard<std::mutex> lock(controlMutex);
            stop = true;
            immediateStop = !waitForTasks;
        }
        cond.notify_all();
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
    }

    bool isStopped() const { return stop; }
    size_t queueSize() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return taskQueue.size();
    }

    size_t workerCount() const { return workers.size(); }

    std::atomic<int> totalTasks{ 0 };
    std::atomic<long long> execTimeMs{ 0 };
    std::atomic<long long> waitTimeMs{ 0 };
    std::atomic<long long> totalQueueLengths{ 0 };
    std::atomic<int> queueSamples{ 0 };

private:
    void workerLoop() {
        while (true) {
            Task task;

            {
                std::unique_lock<std::mutex> lock(queueMutex);
                cond.wait(lock, [this] {
                    return stop || !taskQueue.empty();
                    });

                if (stop && (immediateStop || taskQueue.empty())) return;
                if (taskQueue.empty()) continue;

                task = std::move(taskQueue.top());
                taskQueue.pop();
            }

            auto start = std::chrono::steady_clock::now();
            waitTimeMs += std::chrono::duration_cast<std::chrono::milliseconds>(start - task.enqueueTime).count();

            task.func();

            auto end = std::chrono::steady_clock::now();
            execTimeMs += std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            totalTasks++;
        }
    }

    std::vector<std::thread> workers;
    std::priority_queue<Task> taskQueue;
    std::mutex queueMutex, controlMutex;
    std::condition_variable cond;

    bool stop;
    bool immediateStop;
};

std::mutex coutMutex;

void simulateTask(int duration, int id) {
    {
        std::lock_guard<std::mutex> lock(coutMutex);
        std::cout << "[Task " << id << "] Start (" << duration << "s)\n";
    }
    std::this_thread::sleep_for(std::chrono::seconds(duration));
    {
        std::lock_guard<std::mutex> lock(coutMutex);
        std::cout << "[Task " << id << "] Done\n";
    }
}

int main() {
    ThreadPool pool(4);
    std::atomic<int> idCounter{ 1 };

    // Multiple task producers
    std::vector<std::thread> producers;
    for (int i = 0; i < 3; ++i) {
        producers.emplace_back([&pool, &idCounter] {
            std::mt19937 gen(std::random_device{}());
            std::uniform_int_distribution<> dist(1, 5);
            for (int j = 0; j < 5; ++j) {
                int dur = dist(gen);
                int id = idCounter++;
                pool.addTask(dur, [=] { simulateTask(dur, id); });
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            });
    }

    std::thread monitor([&pool] {
        while (!pool.isStopped()) {
            pool.totalQueueLengths += pool.queueSize();
            pool.queueSamples++;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        });

    for (auto& t : producers) t.join();

    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::cout << "\n[Main] Shutting down...\n";
    pool.shutdown(true);
    monitor.join();

    int done = pool.totalTasks;
    std::cout << "\n--- Stats ---\n";
    std::cout << "Total tasks: " << done << "\n";
    if (done > 0) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Avg exec time: " << (pool.execTimeMs / 1000.0 / done) << "s\n";
        std::cout << "Avg wait time: " << (pool.waitTimeMs / 1000.0 / done) << "s\n";
    }
    if (pool.queueSamples > 0)
        std::cout << "Avg queue size: " << (pool.totalQueueLengths / pool.queueSamples) << "\n";
    std::cout << "Thread count: " << pool.workerCount() << "\n";

    return 0;
}
