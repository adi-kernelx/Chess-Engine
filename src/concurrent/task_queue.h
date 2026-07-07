/**
 * task_queue.h — Thread-safe Multi-Producer Multi-Consumer (MPMC) task queue.
 * 
 * This is the communication channel between the epoll event loop (producer)
 * and the worker threads (consumers). The epoll loop detects that a socket
 * has data, wraps the processing work into a std::function, and pushes it
 * here. Worker threads sit idle until a task appears, then wake up and run it.
 * 
 * We use std::mutex + std::condition_variable (not a lock-free queue) because:
 *   1. It's correct and easy to reason about.
 *   2. The bottleneck is network I/O, not queue contention.
 *   3. Lock-free queues are notoriously hard to get right (ABA problem, memory ordering).
 */

#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace chess {
namespace concurrent {

class TaskQueue {
public:
    using Task = std::function<void()>;

    /**
     * Push a task onto the queue and wake one waiting worker.
     * Called by the producer (epoll event loop).
     */
    void push(Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        // Notify OUTSIDE the lock so the woken thread doesn't immediately
        // block trying to acquire the mutex we're still holding.
        cv_.notify_one();
    }

    /**
     * Pop a task from the queue, blocking until one is available.
     * Called by consumer threads (workers).
     * Returns false if the queue has been shut down and is empty.
     */
    bool pop(Task& task) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Wait until there's a task OR we've been told to stop.
        // The predicate prevents "spurious wakeups" — the OS can
        // sometimes wake a thread for no reason, so we re-check.
        cv_.wait(lock, [this]() {
            return !tasks_.empty() || stopped_;
        });

        // If stopped and nothing left, tell the worker to exit.
        if (stopped_ && tasks_.empty()) {
            return false;
        }

        task = std::move(tasks_.front());
        tasks_.pop();
        return true;
    }

    /**
     * Signal all workers to stop. Any threads blocked in pop() will wake up
     * and return false, causing them to exit their run loop.
     */
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_all();  // Wake ALL waiting threads so they can exit
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

private:
    std::queue<Task> tasks_;
    mutable std::mutex mutex_;    // mutable: allows locking in const methods (size())
    std::condition_variable cv_;
    bool stopped_ = false;
};

} // namespace concurrent
} // namespace chess
