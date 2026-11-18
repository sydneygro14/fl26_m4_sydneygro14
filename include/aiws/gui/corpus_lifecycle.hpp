#pragma once

#include "aiws/build_worker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace aiws::gui {

class CorpusState;

// gui thread owns the state objects and runs every state transition
class CorpusLifecycle final {
public:
    struct Effects {
        std::function<void()> start_build;
        std::function<void()> cancel_build;
        std::function<void()> stop_worker;
        std::function<void()> clear_corpus;
        std::function<void(BuildWorker::Result&&)> install_result;
        std::function<void(const std::string&)> report_failure;
    };

    explicit CorpusLifecycle(Effects effects);
    ~CorpusLifecycle();

    CorpusLifecycle(const CorpusLifecycle&) = delete;
    CorpusLifecycle& operator=(const CorpusLifecycle&) = delete;

    void build_requested();
    void workspace_changed(bool has_documents);
    void reset();
    void build_finished(BuildWorker::Result&& result, std::uint64_t current_generation);
    void exit();

    bool can_build() const;
    bool can_process() const;
    bool is_building() const;

private:
    std::unique_ptr<CorpusState> state_;
    Effects effects_;
};

}  // namespace aiws::gui
