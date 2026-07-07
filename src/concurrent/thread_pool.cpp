/**
 * thread_pool.cpp — ThreadPool implementation.
 * 
 * Each worker thread runs a simple infinite loop:
 *   1. Pop a task from the queue (blocks if queue is empty).
 *   2. Execute the task.
 *   3. Go back to step 1.
 * 
 * When shutdown() is called, the queue is signalled to stop.
 * Workers wake up, see that the queue is done, and exit cleanly.
 */

#include "thread_pool.h"
#include "../core/logger.h"

namespace chess {
namespace concurrent {

ThreadPool::ThreadPool(uint32_t num_threads) {
    // If 0, use the number of CPU cores. std::thread::hardware_concurrency()
    // returns 0 if it can't determine the count, so we fall back to 4.
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
    }
    num_threads_ = num_threads;

    core::Logger::info("pool", "ThreadPool", "Starting thread pool with " + std::to_string(num_threads_) + " workers");

    // Pre-create all worker threads. Each one immediately starts
    // running worker_loop(), which blocks on task_queue_.pop().
    workers_.reserve(num_threads_);
    for (uint32_t i = 0; i < num_threads_; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this, i);
    }
}

ThreadPool::~ThreadPool() {
    if (!shutdown_) {
        shutdown();
    }
}

void ThreadPool::submit(TaskQueue::Task task) {
    task_queue_.push(std::move(task));
}

void ThreadPool::shutdown() {
    shutdown_ = true;
    
    // Tell the queue to stop — this wakes all blocked workers
    task_queue_.stop();

    // Wait for every worker to finish its current task and exit.
    // std::thread::join() blocks until that thread terminates.
    // We MUST join every thread before destroying the ThreadPool,
    // otherwise the program will call std::terminate() and crash.
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    core::Logger::info("pool", "ThreadPool", "All workers shut down");
}

void ThreadPool::worker_loop(uint32_t worker_id) {
    std::string thread_name = "worker-" + std::to_string(worker_id);
    core::Logger::debug(thread_name, "ThreadPool", "Worker started");

    TaskQueue::Task task;
    
    // This loop runs forever until task_queue_.pop() returns false,
    // which only happens after stop() is called AND the queue is empty.
    while (task_queue_.pop(task)) {
        try {
            task();
        } catch (const std::exception& e) {
            core::Logger::error(thread_name, "ThreadPool", "Task threw exception: " + std::string(e.what()));
        } catch (...) {
            core::Logger::error(thread_name, "ThreadPool", "Task threw unknown exception");
        }
    }

    core::Logger::debug(thread_name, "ThreadPool", "Worker exiting");
}

} // namespace concurrent
} // namespace chess
