
// m3 code reused for m4


#include "aiws/gui/main_window.hpp"

#include "aiws/gui/context_widget.hpp"
#include "aiws/gui/search_widget.hpp"
#include "aiws/gui/term_inspector_widget.hpp"
#include "aiws/gui/workspace_widget.hpp"

#include <QMetaObject>
#include <QFileDevice>
#include <QTimer>
#include <chrono>
#include <future>
#include <thread>
#include <algorithm>
#include <memory>
#include "aiws/remote_add_server.hpp"
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QString>
#include <QVBoxLayout>

#include <stdexcept>
#include <string>
#include <utility>

namespace aiws::gui {

namespace {

// window size at startup and the corpus text shown while no corpus is current - 900x900, layout, colors
constexpr int kStartWidth = 900;
constexpr int kStartHeight = 900;
const QString kCorpusNotBuilt = QStringLiteral("Corpus: Not built");

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) { // constructor
    setWindowTitle("AI Workspace");

    // the four components plus the status row controls
    workspace_widget_ = new WorkspaceWidget;
    search_widget_ = new SearchWidget;
    term_inspector_widget_ = new TermInspectorWidget;
    context_widget_ = new ContextWidget;
    status_label_ = new QLabel("Ready");
    reset_button_ = new QPushButton("Reset");
    exit_button_ = new QPushButton("Exit");

    // header reset and exit each get a readable non white background
    auto* header = new QFrame;
    header->setStyleSheet("background-color: #6b1f2d; color: white;");
    auto* header_layout = new QVBoxLayout(header);
    header_layout->addWidget(new QLabel("<span style='font-size:22px; font-weight:bold;'>AI Workspace</span>"));
    header_layout->addWidget(new QLabel("Document Search and Context Explorer"));
    reset_button_->setStyleSheet("background-color: #d4a03a; color: #222222; font-weight: bold; "
                                 "border: none; border-radius: 4px; padding: 6px 24px;");
    exit_button_->setStyleSheet("background-color: #3f6fb5; color: white; font-weight: bold; "
                                "border: none; border-radius: 4px; padding: 6px 24px;");
    // long status messages are clipped instead of widening the window
    status_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    // lower pair then the status row with reset and exit on the right
    auto* lower = new QHBoxLayout;
    lower->addWidget(term_inspector_widget_, 1);
    lower->addWidget(context_widget_, 1);
    auto* footer = new QHBoxLayout;
    footer->addWidget(new QLabel("<b>Current status:</b>"));
    footer->addWidget(status_label_, 1);
    footer->addWidget(reset_button_);
    footer->addWidget(exit_button_);

    // layouts only so the window stays resizable
    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->addWidget(header);
    layout->addWidget(workspace_widget_, 3);
    layout->addWidget(search_widget_, 4);
    layout->addLayout(lower, 2);
    layout->addLayout(footer);
    setCentralWidget(central);
    resize(kStartWidth, kStartHeight);

    // events flow from component signals into main window slots
    connect(workspace_widget_, &WorkspaceWidget::add_document_requested, this, &MainWindow::add_document);
    connect(workspace_widget_, &WorkspaceWidget::build_corpus_requested, this, &MainWindow::build_corpus);
    connect(workspace_widget_, &WorkspaceWidget::document_selected, this, &MainWindow::document_selected);
    connect(search_widget_, &SearchWidget::search_requested, this, &MainWindow::search_requested);
    connect(search_widget_, &SearchWidget::query_changed, this, &MainWindow::query_changed);
    connect(search_widget_, &SearchWidget::source_document_requested, this, &MainWindow::source_document_requested);
    connect(term_inspector_widget_, &TermInspectorWidget::term_inspection_requested,
            this, &MainWindow::term_inspection_requested);
    connect(context_widget_, &ContextWidget::build_context_requested, this, &MainWindow::build_context_requested);
    connect(reset_button_, &QPushButton::clicked, this, &MainWindow::reset_application);
    connect(exit_button_, &QPushButton::clicked, this, &MainWindow::exit_application);
    // selected chunk goes straight to the inspector which only remembers it for search:
    connect(search_widget_, &SearchWidget::selected_chunk_changed,
            term_inspector_widget_, &TermInspectorWidget::set_selected_chunk);

    build_timer_ = new QTimer(this);
    build_timer_->setInterval(15);
    connect(build_timer_, &QTimer::timeout, this, &MainWindow::poll_build);
    update_action_states();
}

void MainWindow::set_exit_callback(ExitCallback callback) noexcept {
    exit_callback_ = callback; // exit_callback_ checks the "if (exit_callback_)"
}

// this reads a file and changes nothing if it cannot be read
bool MainWindow::add_document_file(const QString& path) {
    if (path.isEmpty()) return false;
    const QFileInfo info(path);
    const QString full_path = info.absoluteFilePath();
    QFile file(full_path);
    if (!file.open(QIODevice::ReadOnly) || !info.isFile()) return false;
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) return false;
    const std::string id = full_path.toStdString();
    for (const Document& doc : workspace_.documents()) {
        if (doc.id() == id) {
            status_label_->setText("Already in workspace: " + info.fileName());
            return true;
        }
    }
    workspace_.add_document(Document(id, info.fileName().toStdString(),
                       std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()))));
    workspace_widget_->add_document(info.fileName());
    invalidate_corpus();
    update_action_states();
    status_label_->setText("Added document: " + info.fileName() + " (build the corpus to search)");
    return true;
}

// the button and remote add both use the same file loading path
void MainWindow::add_document() {
    const QString path = QFileDialog::getOpenFileName(this, "Add Document", QString(),
                                                       "Text files (*.txt);;All files (*)");
    if (path.isEmpty()) return;
    if (!add_document_file(path)) {
        status_label_->setText("Could not read file: " + QFileInfo(path).fileName());
    }
}

void MainWindow::set_build_delay_ms(int ms_per_document) {
    build_delay_ms_ = std::max(0, ms_per_document);
}

void MainWindow::fail_next_build_for_test() {
    force_failure_next_ = true;
}

// the actual rebuild runs on a standard library worker thread
void MainWindow::build_corpus() {
    if (workspace_.documents().empty() || building_) return;
    // copy all input so changes and reset cannot change the worker data
    Workspace snapshot = workspace_;
    build_generation_ = workspace_generation_;
    const int delay = build_delay_ms_;
    build_cancel_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancel = build_cancel_;
    const bool inject_failure = force_failure_next_;
    force_failure_next_ = false;
    building_ = true;
    corpus_current_ = false;
    // clear the old backend before starting a new build
    processing_core_ = ProcessingCore{};
    clear_processing_outputs();
    term_inspector_widget_->clear_statistics();
    workspace_widget_->set_corpus_status("Corpus: Building...");
    status_label_->setText("Building corpus...");
    update_action_states();
    try {
        build_future_ = std::async(std::launch::async,
            [snapshot = std::move(snapshot), delay, inject_failure, cancel]() mutable -> BuildResult {
                BuildResult result;
                try {
                    if (inject_failure) throw std::runtime_error("simulated build failure");
                    // delay belongs to the worker instead of the gui
                    for (std::size_t i = 0; i < snapshot.documents().size(); ++i) {
                        // check cancellation during each artificial delay
                        for (int elapsed = 0; elapsed < delay && !cancel->load(); elapsed += 10) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10, delay - elapsed)));
                        }
                        if (cancel->load()) return result;
                    }
                    if (!cancel->load()) result.corpus.rebuild(snapshot);
                } catch (const std::exception& error) {
                    result.error = error.what();
                } catch (...) {
                    result.error = "unknown build failure";
                }
                return result;
            });
        build_timer_->start();
    } catch (const std::exception& error) {
        building_ = false;
        invalidate_corpus();
        update_action_states();
        status_label_->setText(QString("Build failed: ") + error.what());
    }
}

// the gui checks completion without ever waiting for the worker
void MainWindow::poll_build() {
    if (!building_ || !build_future_.valid()) return;
    if (build_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    build_timer_->stop();
    BuildResult result;
    try {
        result = build_future_.get();
    } catch (const std::exception& error) {
        result.error = error.what();
    } catch (...) {
        result.error = "unknown build failure";
    }
    // this fault intentionally keeps the application stuck in building state
#ifndef AIWS_FAULT_LOSE_FAILURE
    if (!result.error.empty() && build_generation_ == workspace_generation_) {
        building_ = false;
        invalidate_corpus();
        update_action_states();
        status_label_->setText(QString("Build failed: ") + QString::fromStdString(result.error));
        return;
    }
#else
    if (!result.error.empty() && build_generation_ == workspace_generation_) {
        // deliberately drop the failure notification and stay in building state
        return;
    }
#endif
    building_ = false;
    // a completed build must match the exact workspace version it started with
#ifndef AIWS_FAULT_ACCEPT_STALE
    if (build_generation_ != workspace_generation_) {
        update_action_states();
        return;
    }
#endif
    if (!result.error.empty()) {
        invalidate_corpus();
        update_action_states();
        status_label_->setText(QString("Build failed: ") + QString::fromStdString(result.error));
        return;
    }
    processing_core_ = std::move(result.corpus);
    clear_processing_outputs();
    term_inspector_widget_->clear_statistics();
    corpus_current_ = true;
    workspace_widget_->set_corpus_status(QString("Corpus: %1 chunks").arg(processing_core_.chunk_count()));
    update_action_states();
    status_label_->setText("Corpus built");
}

// the socket server only queues work for the gui and never waits for it
bool MainWindow::start_remote_add(const std::string& socket_path) {
    if (remote_add_server_) return false;
    auto server = std::make_unique<aiws::RemoteAddServer>(
        [this](const std::string& path) {
            const QString text = QString::fromUtf8(path.data(), static_cast<int>(path.size()));
            QMetaObject::invokeMethod(this, [this, text]() {
                add_document_file(text);
            }, Qt::QueuedConnection);
        });
    std::string error;
    if (!server->start(socket_path, error)) {
        status_label_->setText(QString::fromStdString(error));
        return false;
    }
    remote_add_server_ = std::move(server);
    return true;
}

// stop listening and wait for the worker before deleting its owner
MainWindow::~MainWindow() {
    if (build_timer_) build_timer_->stop();
    // cancel long simulated delays so exiting does not wait for them
    if (build_cancel_) build_cancel_->store(true);
    if (build_future_.valid()) build_future_.wait();
    if (remote_add_server_) remote_add_server_->stop();
}

void MainWindow::document_selected(int row) {
    // a negative or stale row means nothing is selected
    const std::vector<Document>& documents = workspace_.documents();
    if (row < 0 || static_cast<std::size_t>(row) >= documents.size()) {
        workspace_widget_->clear_selected_document();
        return;
    }
    // display only and nothing is processed
    const Document& document = documents[static_cast<std::size_t>(row)];
    workspace_widget_->show_selected_document(QString::fromStdString(document.title()),
                                              QString::fromStdString(document.text()));
}

void MainWindow::search_requested(const QString& query, int k) {
    // search is only valid for a current corpus and a non blank query
    if (!corpus_current_ || query.trimmed().isEmpty()) return;

    // new rows replace the old ones and drop the old selection and details
    // and the stale context goes with them
    std::vector<SearchResult> results = processing_core_.search(query.toStdString(), k); // asks backend for search results
    const std::size_t count = results.size();

    search_widget_->set_results(std::move(results)); // displays results
    // set_results wipes term stats + context and also calls SearchResultModel() to switch the data between beginResetModel/end
    term_inspector_widget_->clear_statistics();
    context_widget_->clear_context();
    status_label_->setText(QString("Search returned %1 results").arg(count));
}

void MainWindow::query_changed(const QString&) {
    // editing the query only changes which actions are available
    update_action_states(); // As long as query isn't blank, Search + Build turn on - never searches
}

void MainWindow::build_context_requested(int token_budget) {
    // context uses the current query and k and never the displayed rows (riskier to read from frontend)
    if (!corpus_current_ || !search_widget_->has_query()) return; // checks if corpus is not updated or if query is blank
    const std::vector<ContextItem> items = processing_core_.build_context( // asks backend to build context
        search_widget_->query().toStdString(), search_widget_->result_count(), // query, k, and slider budget
        static_cast<std::size_t>(token_budget));
    context_widget_->set_context(items); // displays context items
    status_label_->setText(QString("Context built with %1 items").arg(items.size()));
}

void MainWindow::source_document_requested(const QString& document_id) {
    // selecting the row makes the workspace widget report it and the display follows
    const std::vector<Document>& documents = workspace_.documents();
    const std::string id = document_id.toStdString();
    for (std::size_t row = 0; row < documents.size(); ++row) {
        if (documents[row].id() == id) { // finding correct document id selected
            workspace_widget_->select_document(static_cast<int>(row)); // displays whole document
            status_label_->setText("Showing source: " + QString::fromStdString(documents[row].title()));
            return; // then goes to document_selected!
        }
    }
}
// try/catch for INSPECT: If you type more than one word (like “term frequency”) and click Inspect, the backend throws error. 
// The app clears the DF/TF numbers and asks for a single word instead of crashing.
void MainWindow::term_inspection_requested(const QString& term, const QString& chunk_id) {
    // inspection needs a current corpus and a non blank term
    if (!corpus_current_ || term.trimmed().isEmpty()) return;

    // chunk frequency is only available when a result chunk is selected
    const std::string text = term.toStdString();
    const bool has_chunk = !chunk_id.isEmpty();
    try { // try_catch
        const std::size_t document_frequency = processing_core_.document_frequency(text); // asks backend for DF
        const std::size_t chunk_frequency =  // if a chunk is selected, ask backend for TF if not display 0
            has_chunk ? processing_core_.term_frequency(text, chunk_id.toStdString()) : 0;
        term_inspector_widget_->set_statistics(document_frequency, has_chunk, chunk_frequency); // show DF and TF
        status_label_->setText("Term inspected");
    } catch (const std::invalid_argument&) { // catches more than one word term input
        // the backend only accepts a term that normalizes to one token
        term_inspector_widget_->clear_statistics();
        status_label_->setText("Enter a single word to inspect");
    }
}

void MainWindow::reset_application() {
    // empty workspace and backend corpus before the components reset
    ++workspace_generation_;
    workspace_.clear(); // clears workspace
    processing_core_.rebuild(workspace_); // "rebuilds" backend from nothing so it actually clears it
    corpus_current_ = false;

    // every component resets through the common interface
    Resettable* const components[] = {workspace_widget_, search_widget_,
                                      term_inspector_widget_, context_widget_};
    for (Resettable* component : components) component->reset();

    update_action_states();
    status_label_->setText("Application reset.");
}

void MainWindow::exit_application() {
    // the callback decides whether the application really quits
    if (exit_callback_) exit_callback_(this); // exit_application checks "if (exit_callback_)" which is NOT null (bc of set_exit_callback in main,
    // which then calls exit_callback_(this) which runs confirm_exit in main.cpp. which is the window popup with
} // a null callback makes exit do nothing

void MainWindow::update_action_states() {
    // the single place that decides which actions are available
    const bool has_documents = !workspace_.documents().empty(); // makes sure workspace has documents
    const bool has_query = search_widget_->has_query(); // makes sure there is a q
    // query typed
    workspace_widget_->set_build_corpus_enabled(has_documents && !building_); // enables
    search_widget_->set_search_enabled(corpus_current_ && has_query);
    term_inspector_widget_->set_inspection_enabled(corpus_current_);
    context_widget_->set_build_context_enabled(corpus_current_ && has_query);
}

void MainWindow::invalidate_corpus() {
    // old corpus and everything derived from it can no longer be used
    ++workspace_generation_;
    corpus_current_ = false;
    processing_core_ = ProcessingCore{};
    clear_processing_outputs();
    term_inspector_widget_->clear_statistics();
    workspace_widget_->set_corpus_status(kCorpusNotBuilt);
}

void MainWindow::clear_processing_outputs() {
    // results selection details and context all depend on processing
    // the search widget reports the lost selection so the inspector forgets its chunk
    search_widget_->clear_results();
    context_widget_->clear_context();
}

}  // namespace aiws::gui
