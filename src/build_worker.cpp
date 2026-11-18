#include "aiws/build_worker.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace aiws {

// create the only build thread for the lifetime of the worker
BuildWorker::BuildWorker(Completion completion)
    : completion_(std::move(completion)),
      thread_(&BuildWorker::run, this) {}

BuildWorker::~BuildWorker() {
    stop();
}

// hand off a copied workspace without waiting for the running build
void BuildWorker::request(Workspace snapshot, std::uint64_t generation,
                          int delay_ms, bool inject_failure) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        ++serial_;
        pending_ = Request{std::move(snapshot), generation, serial_,
                           delay_ms, inject_failure};
    }
    changed_.notify_all();
}

// mark the active request obsolete and discard requests not yet taken
void BuildWorker::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++serial_;
        pending_.reset();
    }
    changed_.notify_all();
}

// only the worker waits on the condition variable
bool BuildWorker::cancelled(std::uint64_t serial) {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_ || serial_ != serial;
}

// simulated time is interruptible when the current request is cancelled
bool BuildWorker::delay_or_cancel(std::uint64_t serial, int delay_ms) {
#ifdef AIWS_FAULT_NO_CANCEL
    (void)serial;
    // this fault ignores obsolete requests while a build is running
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                      [this] { return stopping_; });
    return stopping_;
#else
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                      [this, serial] { return stopping_ || serial_ != serial; });
    return stopping_ || serial_ != serial;
#endif
}

// keep waiting for a request on the same thread after each completion
void BuildWorker::run() {
    for (;;) {
        Request job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            idle_ = true;
            changed_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            idle_ = false;
            if (stopping_) return;
            job = std::move(*pending_);
            pending_.reset();
        }

        Result result;
        result.generation = job.generation;
        result.thread_id = std::this_thread::get_id();
        try {
            if (job.inject_failure) throw std::runtime_error("simulated build failure");

            // each document is a cancellation point before and after its delay
            for (std::size_t i = 0; i < job.snapshot.documents().size(); ++i) {
#ifndef AIWS_FAULT_NO_CANCEL
                if (cancelled(job.serial)) {
                    result.cancelled = true;
                    break; 
                }
#endif
                if (delay_or_cancel(job.serial, job.delay_ms)) {
                    result.cancelled = true;
                    break;
                }
            }
            if (cancelled(job.serial)) result.cancelled = true;
            if (!result.cancelled) result.corpus.rebuild(job.snapshot);
        } catch (const std::exception& error) {
            result.error = error.what();
        } catch (...) {
            result.error = "unknown build failure";
        }

        // the gui receives results only through the queued completion callback
        if (!result.cancelled) completion_(std::move(result));
    }
}

// notify the idle worker and join the same thread on exit
void BuildWorker::stop() {
    [[maybe_unused]] bool was_idle = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) {
            was_idle = idle_;
            stopping_ = true;
            ++serial_;
            pending_.reset();
        }
    }
#ifndef AIWS_FAULT_SILENT_SHUTDOWN
    changed_.notify_all();
#else
    // this fault leaves an idle worker asleep during shutdown
    if (!was_idle) changed_.notify_all();
#endif
    if (thread_.joinable()) thread_.join();
}

}  // namespace aiws
