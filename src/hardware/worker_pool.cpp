#include "worker_pool.hpp"

#include <algorithm>

namespace hcs::detail {

WorkerPool::WorkerPool(std::uint32_t worker_count) {
    if (worker_count < 2) return;
    workers_.reserve(worker_count - 1);
    try {
        for (std::uint32_t i = 1; i < worker_count; ++i)
            workers_.emplace_back([this, i] { worker_loop(i); });
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            ++generation_;
        }
        start_cv_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
        throw;
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        ++generation_;
    }
    start_cv_.notify_all();
    for (auto& worker : workers_) if (worker.joinable()) worker.join();
}

std::uint32_t WorkerPool::worker_count() const noexcept {
    return workers_.empty() ? 1u : static_cast<std::uint32_t>(workers_.size() + 1);
}

void WorkerPool::consume(void* context, Callback callback, std::size_t worker_index) noexcept {
    const auto worker_total = workers_.size() + 1;
    const auto begin = task_count_ * worker_index / worker_total;
    const auto end = task_count_ * (worker_index + 1) / worker_total;
    for (std::size_t index = begin; index < end; ++index) callback(context, index);
}

void WorkerPool::parallel_for(std::size_t count, void* context, Callback callback) {
    if (count == 0 || workers_.empty()) {
        for (std::size_t i = 0; i < count; ++i) callback(context, i);
        return;
    }

    {
        std::lock_guard lock(mutex_);
        task_count_ = count;
        context_ = context;
        callback_ = callback;
        pending_workers_ = workers_.size();
        ++generation_;
    }
    start_cv_.notify_all();
    consume(context, callback, 0);

    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this] { return pending_workers_ == 0; });
    callback_ = nullptr;
    context_ = nullptr;
}

void WorkerPool::worker_loop(std::size_t worker_index) {
    std::uint64_t observed_generation = 0;
    for (;;) {
        void* context = nullptr;
        Callback callback = nullptr;
        {
            std::unique_lock lock(mutex_);
            start_cv_.wait(lock, [this, &observed_generation] {
                return stopping_ || generation_ != observed_generation;
            });
            if (stopping_) return;
            observed_generation = generation_;
            context = context_;
            callback = callback_;
        }

        consume(context, callback, worker_index);

        {
            std::lock_guard lock(mutex_);
            if (--pending_workers_ == 0) done_cv_.notify_one();
        }
    }
}

} // namespace hcs::detail
