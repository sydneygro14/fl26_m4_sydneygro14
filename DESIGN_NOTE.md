# M4 Design Note

Due Wed Oct 28, 11:59 pm. See specification Section 7.


1. Components / threads:
- MainWindow (GUI thread only) - owns the workspace, the current corpus is std::unique_ptr<ProcessingCore> corpus_ (null when there is no current corpus), a std::uint64_t generation_ counter, the corpus state, and one CorpusBuilder. Every slot, including the new build_finished(quint64generation) slot, runs on the GUI thread and only MainWindow touches widgets.
- CorpusBuilder(new class, no QT widgets) - start() and finish() run on the GUI thread. start() copies the Workspace and triggers on std::thread that runs run(). run() is the only function on the worker thread and it builds a brand new ProcessingCore from the copy, stores result in std::promise, and calls QMetaObject::invokeMethod(main_window, "build_finished", Qt::QueuedConnection, generation). finish() will join the thread and call future.get(). the destuctor sets the cancel flag and joins
- DelaydCHunking(new ChunkingStrategy wrapper). Its chunk() runs on the worker thread inside rebuild() once per document. It sleep the --build-delay-ms value in 10ms steps, checks the cancel flag between each step, then calls the real Chunker. This will slow the real build path without ever touching the GUI thread.
Shared data - the worker will get its own copy of the Workspace, the generation number, and the delay by value, so it never reads GUI data. Only two things are shared. the std::atomic<bool> cancel flag is written by GUI thread and read by worker which is safe bc it is atomic. And the promise/future shared state is written once by the worker and read by the GUI thread only after build_finished arrives, which is safe because std::future synchronizes it.

-------------------------------------------------------------------

2. Corpus states/events

NotBuilt: no corpuse. Build corpus is enabled if the workspace has documents
Building: a thread is running and its result is wanted. Search, term inspector, context, and build are all disabled
Discarding: a thread is still runing but the workspace changed or reset was clicked, so its result will be thrown away. build stays disabled
Current: corpus_ matches the workspace. Chunk count is shown and search, term inspector, and context are enabled

Events:
NotBuilt -> Building: Build Corpus clicked

Building -> Current: build_finished with a matching generation and a successful result

Building -> NotBuilt: build_finished with a failure, the error is shown in status label

Building -> Discarding: a doc is added or reset is clicked, generation_ goes up by one and cancel flag gets set

Discarding -> NotBuilt: build_finished arrives, the result gets dropped

Current -> NotBuilt: a doc is added or reset is clicked, corpus_ is reset and the outputs are cleared

---------------------------------------------------------

3. Threatening Sequences
- Add a document during a build (R2): handled by the generation check, walked through below.
- Reset during a build, or Reset and then re-add the same files before the build finishes (R2): comparing Workspace contents would wrongly accept this result. A generation number is never reused, so the old build never matches.
- Exit during a build (R4): walked through below.
- Build throws (R2, R3): run() catches every exception and calls set_exception, so nothing escapes the thread. finish() rethrows it on the GUI thread, where it is caught and reported. The state goes to NotBuilt and the app stays usable.
- Search or inspect while building (R2): those actions are disabled, and the slots also return early unless the state is Current.
- Double clicking Build, or many builds in a row (R4): Build is disabled while any thread is alive, so there is at most one thread. Each one is joined in build_finished before the next can start, and the old corpus is freed when it is replaced.
- Unreadable file during a build: add_document_file() returns false and changes nothing, including generation_.

Walkthrough A, add a document during a build:
1. The user clicks Build. generation_ becomes 5, the state becomes Building, and the worker starts with a copy of the Workspace and generation 5.
2. The user adds doc_ai.txt. The Workspace changes, generation_ becomes 6, the cancel flag is set, and the state becomes Discarding.
3. The worker stops at its next delay check and posts build_finished(5).
4. build_finished calls finish(), which joins the thread. 5 != 6, so the result is destroyed and the state becomes NotBuilt. The status says to build again, and Build is enabled.

Walkthrough B, Exit during a build:
1. A build with a 2000 ms per document delay is running.
2. The user confirms Exit and MainWindow is destroyed. The CorpusBuilder destructor sets the cancel flag.
3. The worker sees the flag within 10 ms and returns. The destructor joins it.
4. The queued build_finished event targets an object that no longer exists, so Qt drops it. No detached thread, no crash, no hang.


------------------------------------------------------------------
4. Ownership path of one corpus

The worker creates it with std::make_unique<ProcessingCore>. It moves into the promise with set_value. On the GUI thread, finish() moves it out with future.get() into a local unique_ptr in build_finished. If the generation matches, it moves into corpus_ and the previous corpus is destroyed. corpus_ is destroyed when the next accepted build replaces it, when a document is added, on Reset, or when MainWindow is destroyed. If the generation does not match, the local unique_ptr destroys it at the end of build_finished. There is no raw new or delete at any step.

5. Rejected alternative

I rejected running rebuild() on the worker directly on the shared processing_core_ member from M3. Search and Term Inspector would read its chunks while the worker writes them, which is a data race that would need a mutex, and mutexes are out of scope. Building into a new object and swapping it in on the GUI thread means no data is ever written and read at the same time, and the GUI only does a pointer move.
