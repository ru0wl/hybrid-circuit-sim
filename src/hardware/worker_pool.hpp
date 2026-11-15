#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace hcs::detail {

class WorkerPool final {
public:
    using Callback = void (*)(void*, std::size_t) noexcept;

    explicit WorkerPool(std::uint32_t worker_count);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    [[nodiscard]] std::uint32_t worker_count() const noexcept;
    void parallel_for(std::size_t count, void* context, Callback callback);

private:
    void worker_loop(std::size_t worker_index);
    void consume(void* context, Callback callback, std::size_t worker_index) noexcept;

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable start_cv_;
    std::condition_variable done_cv_;
    std::uint64_t generation_{0};
    std::size_t pending_workers_{0};
    bool stopping_{false};
    std::size_t task_count_{0};
    void* context_{nullptr};
    Callback callback_{nullptr};
};

} // namespace hcs::detail
