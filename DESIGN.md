## M4 implementation decisions

**Thread ownership.** `MainWindow` and all Qt widgets live only on the GUI thread. `MainWindow::build_corpus` copies the `Workspace`, then starts one `std::async(std::launch::async)` worker. The worker owns the snapshot and its own `ProcessingCore` and does all processing and per-document delay. `MainWindow::poll_build` is a `QTimer` member slot: it checks the future without blocking and transfers the completed `ProcessingCore` into the GUI-owned member. At exit an atomic cancellation flag interrupts artificial delays, then the future is waited on; no task is orphaned. New builds are ignored while one is running; the Build button is disabled until it finishes.

**Shared mutable state (constraint 5).** No mutable Workspace, `ProcessingCore`, or widget is shared concurrently between build worker and GUI. The worker operates on its copy; the shared atomic cancellation flag is written by the GUI destructor and read by the worker; `future.get()` transfers the result only once ready. The remote-add server owns `stopping_`, its atomic stop flag, read by its receiver thread and written at shutdown. The server's listener and path are initialized before the receiver starts and cleaned after joining it. The receiver never accesses widget state directly; it queues `MainWindow::add_document_file` on the GUI thread.

**Correctness.** `workspace_generation_` increases on Add Document, remote add, and Reset. A build records `build_generation_` at start. `poll_build` accepts the completed result only when these values still match. Other results are discarded. Search, inspector, and context remain disabled until a valid result is installed. Errors are caught by the worker and displayed by the GUI; the user may retry. `AIWS_FAULT_ACCEPT_STALE` bypasses the generation check; the stale-workspace test detects it. `AIWS_FAULT_LOSE_FAILURE` drops the error delivery and remains building; the failure test detects it.

**Changes since checkpoint.** The delayed rebuild moved into a standard-library worker. A timer checks completion. Remote Add introduced a separate Unix-domain socket thread and the same document-add invalidation path as the GUI. Existing M1/M2 interfaces and search/inspection logic did not change. This design is different from putting processing on the GUI thread, which would freeze document selection and Reset.

**CR-1 protocol.** Sender request: one byte ASCII `A` (0x41), then a four-byte unsigned big-endian pathname byte length, then exactly that many raw UTF-8 pathname bytes (maximum 65,535). Response: one status byte (`0` accepted, `1` rejected), four-byte unsigned big-endian UTF-8 reason length, then reason bytes. Client allows up to two seconds to connect and one five-second deadline for sending plus receiving. The server reads fragmented requests, validates file readability, queues the add, and answers without waiting for the GUI. Stop joins the receiver and removes its socket file.




## Pair programming log #############

# M4 Decision Record

See specification Section 8.

tuesday november 3rd 7:00pm

first 15 min - sydney driver, amruth navigator

Date: Tuesday, November 3rd 2026
Start time Sydney Driver/Amruth Navigator: 7:00pm
End time Sydnet Driver/Amruth Navigator: 7:30pm

Start time Amruth Driver/Sydney Navigator: 7:30pm
End time Amruth Driver/Sydney Navigator: 8:00pm
