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

