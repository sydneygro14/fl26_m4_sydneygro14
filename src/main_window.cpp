# M3


#include "aiws/gui/main_window.hpp"

#include "aiws/gui/context_widget.hpp"
#include "aiws/gui/search_widget.hpp"
#include "aiws/gui/term_inspector_widget.hpp"
#include "aiws/gui/workspace_widget.hpp"

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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) { # constructor
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

    update_action_states();
}

void MainWindow::set_exit_callback(ExitCallback callback) noexcept {
    exit_callback_ = callback; // exit_callback_ checks the "if (exit_callback_)"
}

void MainWindow::add_document() {
    const QString path = QFileDialog::getOpenFileName(this, "Add Document", QString(),
                                                      "Text files (*.txt);;All files (*)"); // opens file dialog
    if (path.isEmpty()) return;     // if user cancels dialog, nothing happens

    // absolute path is the document id so the same file cannot be added twice
    const QFileInfo info(path);
    const std::string id = info.absoluteFilePath().toStdString();
    for (const Document& existing : workspace_.documents()) {
        if (existing.id() == id) {
            status_label_->setText("Already in workspace: " + info.fileName());ch
            return;
        }
    } // checks if file has error when it opens
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        status_label_->setText("Could not read file: " + info.fileName());
        return;
    }

    // raw bytes go to the backend untouched and the file name is the title
    const QByteArray bytes = file.readAll();
    workspace_.add_document(Document(id, info.fileName().toStdString(),
                                     std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()))));
    workspace_widget_->add_document(info.fileName()); // adds document filename to screen

    // the workspace changed so the old corpus and its output are stale
    invalidate_corpus(); // sets current_corpus() to false + clears workspace (old resutls, stats, etc.)
    update_action_states(); // -> greys out search, inspect, and build buttons so user can't select
    status_label_->setText("Added document: " + info.fileName() + " (build the corpus to search)");
}

// try/catch for BUILD_CORPUS 
// current_corpus remains false
    void MainWindow::build_corpus() {
    // building needs at least one document
    if (workspace_.documents().empty()) return;
    try {
        processing_core_.rebuild(workspace_); // pulls from backend through build
    } catch (const std::exception& error) {
        // a failed rebuild leaves no current corpus
        invalidate_corpus();
        update_action_states();
        status_label_->setText(QString("Build failed: ") + error.what());
        return;
    }
// no error:
    // fresh corpus replaces every old result selection statistic and context / wipes old content if try/catch is successful
    clear_processing_outputs();
    term_inspector_widget_->clear_statistics();
    corpus_current_ = true;
    workspace_widget_->set_corpus_status(QString("Corpus: %1 chunks").arg(processing_core_.chunk_count()));
    update_action_states(); // enables buttons
    status_label_->setText("Corpus built");
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
    workspace_widget_->set_build_corpus_enabled(has_documents); // enables
    search_widget_->set_search_enabled(corpus_current_ && has_query);
    term_inspector_widget_->set_inspection_enabled(corpus_current_);
    context_widget_->set_build_context_enabled(corpus_current_ && has_query);
}

void MainWindow::invalidate_corpus() {
    // old corpus and everything derived from it can no longer be used
    corpus_current_ = false;
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
