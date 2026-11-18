#pragma once

#include "aiws/processing_core.hpp"
#include "aiws/workspace.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace aiws {

// one thread handles every build request until the worker is stopped
class BuildWorker final {
public:
    struct Result {
        std::uint64_t generation = 0;
        ProcessingCore corpus;
        std::string error;
        bool cancelled = false;
        std::thread::id thread_id;
    };

    using Completion = std::function<void(Result&&)>;

    explicit BuildWorker(Completion completion);
    ~BuildWorker();

    BuildWorker(const BuildWorker&) = delete;
    BuildWorker& operator=(const BuildWorker&) = delete;

    // the newest request replaces any request that has not started
    void request(Workspace snapshot, std::uint64_t generation,
                 int delay_ms, bool inject_failure);

    // cancel running work and remove any pending request
    void cancel();

    // stop the thread and join it before any shared data is destroyed
    void stop();

private:
    struct Request {
        Workspace snapshot;
        std::uint64_t generation = 0;
        std::uint64_t serial = 0;
        int delay_ms = 0;
        bool inject_failure = false;
    };

    void run();
    bool cancelled(std::uint64_t serial);
    bool delay_or_cancel(std::uint64_t serial, int delay_ms);

    Completion completion_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Request> pending_;
    std::uint64_t serial_ = 0;
    bool stopping_ = false;
    bool idle_ = false;
    std::thread thread_;
};

}  // namespace aiws
