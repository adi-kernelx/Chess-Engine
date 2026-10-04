#pragma once

#include "storage/database.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace chess::storage {

// Small bounded pool, initialized once before workers start. Exclusive leases
// preserve all existing Database/Transaction semantics. No shared-PGconn calls.
// Pool must outlive every lease; shutdown drains workers before destruction.
class DatabasePool {
public:
    class Lease {
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                pool_ = std::exchange(other.pool_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }
        ~Lease() { release(); }
        Database& database() const {
            if (!pool_) throw std::logic_error("released database lease");
            return *pool_->slots_[index_].db;
        }
    private:
        friend class DatabasePool;
        Lease(DatabasePool* pool, size_t index) : pool_(pool), index_(index) {}
        void release() noexcept {
            if (!pool_) return;
            pool_->release(index_);
            pool_ = nullptr;
        }
        DatabasePool* pool_;
        size_t index_;
    };

    // Only startup calls this; never reconfigure underneath borrowers.
    bool connect_from_env(size_t count, std::string& error) {
        if (!slots_.empty() || count == 0 || count > 4) {
            error = "Pool must be initialized once with 1..4 connections";
            return false;
        }
        std::vector<Slot> pending;
        for (size_t i = 0; i < count; ++i) {
            auto db = std::make_unique<Database>();
            if (!db->connect_from_env(error, 5)) return false;
            pending.push_back({std::move(db), false, true});
        }
        slots_ = std::move(pending);
        return true;
    }

    std::optional<Lease> acquire(std::chrono::milliseconds timeout =
                                 std::chrono::milliseconds(1500)) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto ready = [this] {
            for (const auto& slot : slots_) {
                if (!slot.busy) return true;
            }
            return slots_.empty();
        };
        if (!available_.wait_for(lock, timeout, ready)) return std::nullopt;
        // Prefer healthy slots; otherwise repair one idle slot exclusively,
        // outside the pool mutex. Reconnect does NOT replay a failed query.
        for (bool healthy : {true, false}) {
            for (size_t i = 0; i < slots_.size(); ++i) {
                if (!slots_[i].busy && slots_[i].healthy == healthy) {
                    slots_[i].busy = true;
                    lock.unlock();
                    Lease lease(this, i);
                    if (!healthy) {
                        std::string error;
                        if (!slots_[i].db->connect_from_env(error, 5)) return std::nullopt;
                        std::lock_guard<std::mutex> repaired(mutex_);
                        slots_[i].healthy = true;
                    }
                    return lease;
                }
            }
        }
        return std::nullopt;
    }

private:
    struct Slot { std::unique_ptr<Database> db; bool busy; bool healthy; };
    void release(size_t index) noexcept {
        // Still exclusively owned here. Never hold the pool mutex during SQL.
        bool healthy = false;
        try { healthy = slots_[index].db->reset_for_reuse(); }
        catch (...) { /* Quarantine rather than reuse an uncertain session. */ }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            slots_[index].healthy = healthy;
            slots_[index].busy = false;
        }
        available_.notify_all();
    }
    std::vector<Slot> slots_;
    std::mutex mutex_;
    std::condition_variable available_;
};

} // namespace chess::storage
