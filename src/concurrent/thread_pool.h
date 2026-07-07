/**
 * thread_pool.h — Fixed-size thread pool for offloading work from the event loop.
 * 
 * Why a thread pool instead of "spawn a thread per connection"?
 *   - Thread creation is expensive (~1ms + ~8MB stack per thread on Linux).
 *   - With 500 concurrent games, that's 500 threads = 4GB of stack alone.
 *   - A thread pool pre-creates N threads (e.g., 8) and reuses them forever.
 *   - Tasks are queued and workers pick them up — this is the "producer-consumer" pattern.
 * 
 * Architecture:
 *   epoll loop (1 thread) ——push tasks——> TaskQueue <——pop tasks—— Worker threads (N)
 */

#pragma once

#include "task_queue.h"
#include <thread>
#include <vector>
#include <cstdint>

namespace chess {
namespace concurrent {

class ThreadPool {
public:
    /**
     * @param num_threads Number of worker threads. If 0, defaults to
     *        hardware_concurrency() (number of CPU cores).
     */
    explicit ThreadPool(uint32_t num_threads = 0);
    ~ThreadPool();

    // Disable copy — there's no sensible way to copy running threads
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * Submit a task to the pool. The task will be picked up by the next
     * available worker thread. This is non-blocking — it just pushes
     * to the queue and returns immediately.
     */
    void submit(TaskQueue::Task task);

    /**
     * Graceful shutdown: signal workers to stop, then wait for all
     * of them to finish their current task and exit.
     */
    void shutdown();

    uint32_t get_thread_count() const { return num_threads_; }

private:
    void worker_loop(uint32_t worker_id);

    uint32_t num_threads_;
    std::vector<std::thread> workers_;
    TaskQueue task_queue_;
    bool shutdown_ = false;
};

} // namespace concurrent
} // namespace chess
