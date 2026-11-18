# M5 design decisions

**M4 fixes.** None identified in the supplied M4 source. Existing document loading, socket framing, search, term inspection, and context behavior are retained. The M4 build mechanism is replaced with the M5 worker and state lifecycle.

**Threads and shared data.** `MainWindow` owns Qt widgets, the live `Workspace`, `processing_core_`, `workspace_generation_`, and every `CorpusState` on the GUI thread. `BuildWorker` creates one `std::thread` in its constructor and reuses it through exit. The worker processes its own `Workspace` copy and a separate `ProcessingCore`. `BuildWorker::request` replaces an unclaimed request; `run` sleeps only for simulated build time and otherwise waits with `std::condition_variable`. `BuildWorker::stop` wakes and joins it. The only mutable data shared by the GUI and build threads is `pending_`, `serial_`, `stopping_`, and `idle_` inside `BuildWorker`; `mutex_` protects every access. `changed_` coordinates handoff and cancellation. The completion callback posts a result to the GUI with `QMetaObject::invokeMethod` and `Qt::QueuedConnection`; the worker never accesses widgets. The separate CR-1 receiver uses its existing atomic `stopping_` flag and queues document additions without touching GUI state.

**Corpus states.** `CorpusLifecycle` holds one polymorphic `CorpusState`. State event methods select the replacement state and run the corresponding GUI-thread effect. No flag or enum selects transitions.

| State | Meaning | Events and next state |
|---|---|---|
| `EmptyState` | No documents or corpus | Add becomes Dirty; Build ignored; Reset stays Empty |
| `DirtyState` | Documents present but no valid corpus | Build becomes Building; Add remains Dirty; Reset becomes Empty |
| `BuildingState` | Current request is running or pending | Add cancels and becomes Dirty; Reset cancels and becomes Empty; valid success becomes Ready; failure becomes Dirty; obsolete completion ignored; another Build ignored |
| `ReadyState` | Current corpus installed | Build becomes Building; Add becomes Dirty; Reset becomes Empty |

Exit requests cancellation from any state. Only the GUI creates, calls, and destroys these state objects.

**Event trace.** On the GUI thread, `build_corpus` calls `CorpusLifecycle::build_requested`; `DirtyState::build` calls `begin_build_work`. `BuildWorker::request` hands a snapshot to the worker. On the worker thread, `BuildWorker::run` takes it. A remote Add arrives through `RemoteAddServer::handle_client`, which queues `add_document_file` on the GUI. `BuildingState::changed` calls `BuildWorker::cancel`, invalidates the corpus, and becomes Dirty. The worker observes its changed serial and stops. A later Build submits the new snapshot to the same thread. Its completion is queued to the GUI; `BuildingState::finished` checks the generation before installing it and entering Ready.

**Three open decisions.** (1) Build is disabled while Building; an attempted request is ignored by `BuildingState::build`. A Workspace change cancels that build and enables a new request. (2) A remote add mid-build cancels the old job, clears search outputs, shows the added file and not-built corpus, and requires another Build; `add_document_file` and `BuildingState::changed` implement this. (3) The four mutex-protected worker fields above are the minimum shared mutable handoff, cancellation, shutdown, and idle-status data; everything else is GUI-owned or moved to the worker.

**Fault detection.** `AIWS_FAULT_NO_CANCEL` in `BuildWorker::delay_or_cancel` ignores obsolete requests, causing a replacement build to wait; the long-build replacement test in `student_tests.cpp` catches this. `AIWS_FAULT_SILENT_SHUTDOWN` in `BuildWorker::stop` omits the idle notification; the bounded child-process shutdown test catches the hang. Normal builds must pass `student_tests` while each faulted configuration must fail.
