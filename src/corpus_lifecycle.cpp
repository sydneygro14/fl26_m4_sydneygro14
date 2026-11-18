#include "aiws/gui/corpus_lifecycle.hpp"

#include <memory>
#include <utility>

namespace aiws::gui {

using Effects = CorpusLifecycle::Effects;

// event handlers belong to state objects instead of gui flag checks
class CorpusState {
public:
    virtual ~CorpusState() = default;
    virtual std::unique_ptr<CorpusState> build(Effects&) const = 0;
    virtual std::unique_ptr<CorpusState> changed(Effects&, bool has_documents) const = 0;
    virtual std::unique_ptr<CorpusState> reset(Effects&) const = 0;
    virtual std::unique_ptr<CorpusState> finished(
        Effects&, BuildWorker::Result&&, std::uint64_t) const = 0;
    virtual void exit(Effects& effects) const { effects.stop_worker(); }
    virtual bool can_build() const = 0;
    virtual bool can_process() const { return false; }
    virtual bool is_building() const { return false; }
};




class EmptyState;
class DirtyState;
class BuildingState;
class ReadyState;

// state objects decide which replacement state handles the next event
std::unique_ptr<CorpusState> empty_state();
std::unique_ptr<CorpusState> dirty_state();
std::unique_ptr<CorpusState> building_state();
std::unique_ptr<CorpusState> ready_state();



// the workspace has no documents and no current corpus
class EmptyState final : public CorpusState {
public:
    std::unique_ptr<CorpusState> build(Effects&) const override { return nullptr; }
    std::unique_ptr<CorpusState> changed(Effects& effects, bool has_documents) const override {
        effects.clear_corpus();
        return has_documents ? dirty_state() : empty_state();
    }
    std::unique_ptr<CorpusState> reset(Effects& effects) const override {
        effects.cancel_build();
        effects.clear_corpus();
        return empty_state();
    }
    std::unique_ptr<CorpusState> finished(Effects&, BuildWorker::Result&&,
                                           std::uint64_t) const override { return nullptr; }
    bool can_build() const override { return false; }
};




// documents exist but no current corpus has been installed
class DirtyState final : public CorpusState {
public:
    std::unique_ptr<CorpusState> build(Effects& effects) const override {
        effects.start_build();
        return building_state();
    }
    std::unique_ptr<CorpusState> changed(Effects& effects, bool has_documents) const override {
        effects.clear_corpus();
        return has_documents ? dirty_state() : empty_state();
    }
    std::unique_ptr<CorpusState> reset(Effects& effects) const override {
        effects.cancel_build();
        effects.clear_corpus();
        return empty_state();
    }
    std::unique_ptr<CorpusState> finished(Effects&, BuildWorker::Result&&,
                                           std::uint64_t) const override { return nullptr; }
    bool can_build() const override { return true; }
};

// the worker is processing a snapshot and no corpus may be searched
class BuildingState final : public CorpusState {
public:
    // build stays disabled until a workspace change cancels this request
    std::unique_ptr<CorpusState> build(Effects&) const override { return nullptr; }
    std::unique_ptr<CorpusState> changed(Effects& effects, bool has_documents) const override {
        effects.cancel_build();
        effects.clear_corpus();
        return has_documents ? dirty_state() : empty_state();
    }
    std::unique_ptr<CorpusState> reset(Effects& effects) const override {
        effects.cancel_build();
        effects.clear_corpus();
        return empty_state();
    }
    std::unique_ptr<CorpusState> finished(Effects& effects, BuildWorker::Result&& result,
                                           std::uint64_t current_generation) const override {
        // stale and cancelled results never change the current gui state
        if (result.cancelled || result.generation != current_generation) return nullptr;
        if (!result.error.empty()) {
            effects.report_failure(result.error);
            return dirty_state();
        }
        effects.install_result(std::move(result));
        return ready_state();
    }
    bool can_build() const override { return false; }
    bool is_building() const override { return true; }
};




// a successful build matches the current workspace and enables processing
class ReadyState final : public CorpusState {
public:
    std::unique_ptr<CorpusState> build(Effects& effects) const override {
        effects.start_build();
        return building_state();
    }
    std::unique_ptr<CorpusState> changed(Effects& effects, bool has_documents) const override {
        effects.clear_corpus();
        return has_documents ? dirty_state() : empty_state();
    }
    std::unique_ptr<CorpusState> reset(Effects& effects) const override {
        effects.cancel_build();
        effects.clear_corpus();
        return empty_state();
    }
    std::unique_ptr<CorpusState> finished(Effects&, BuildWorker::Result&&,
                                           std::uint64_t) const override { return nullptr; }
    bool can_build() const override { return true; }
    bool can_process() const override { return true; }
};


std::unique_ptr<CorpusState> empty_state() { return std::make_unique<EmptyState>(); }
std::unique_ptr<CorpusState> dirty_state() { return std::make_unique<DirtyState>(); }
std::unique_ptr<CorpusState> building_state() { return std::make_unique<BuildingState>(); }
std::unique_ptr<CorpusState> ready_state() { return std::make_unique<ReadyState>(); }

CorpusLifecycle::CorpusLifecycle(Effects effects)
    : state_(empty_state()), effects_(std::move(effects)) {}
CorpusLifecycle::~CorpusLifecycle() = default;



// state handlers return the next state after their work is complete
void CorpusLifecycle::build_requested() {
    if (auto next = state_->build(effects_)) state_ = std::move(next);
}
void CorpusLifecycle::workspace_changed(bool has_documents) {
    if (auto next = state_->changed(effects_, has_documents)) state_ = std::move(next);
}
void CorpusLifecycle::reset() {
    if (auto next = state_->reset(effects_)) state_ = std::move(next);
}
void CorpusLifecycle::build_finished(BuildWorker::Result&& result,
                                     std::uint64_t current_generation) {
    if (auto next = state_->finished(effects_, std::move(result), current_generation)) {
        state_ = std::move(next);
    }
}


void CorpusLifecycle::exit() { state_->exit(effects_); }
bool CorpusLifecycle::can_build() const { return state_->can_build(); }
bool CorpusLifecycle::can_process() const { return state_->can_process(); }
bool CorpusLifecycle::is_building() const { return state_->is_building(); }

}  // namespace aiws::gui


