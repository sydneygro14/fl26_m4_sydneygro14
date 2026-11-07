// test the real main window using deterministic delays and a local socket
#include "aiws/gui/workspace_widget.hpp"
#include "aiws/gui/main_window.hpp"
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QString>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using aiws::gui::MainWindow;

// look for a displayed status without requiring private member access
static bool has_label(MainWindow& window, const QString& value) {
    for (auto* label : window.findChildren<QLabel*>()) {
        if (label->text().contains(value)) return true;
    }
    return false;
}

// wait with event processing only in tests and never inside application code
static bool wait_label(MainWindow& window, const QString& value, int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents();
        if (has_label(window, value)) return true;
        QThread::msleep(5);
    }
    QCoreApplication::processEvents();
    return has_label(window, value);
}

static bool create_file(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

static bool socket_request(const QString& name, const QByteArray& path, bool one_byte_at_time) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray socket_name = name.toUtf8();
    if (static_cast<std::size_t>(socket_name.size()) >= sizeof(address.sun_path)) {
        ::close(fd);
        return false;
    }
    std::memcpy(address.sun_path, socket_name.constData(), static_cast<std::size_t>(socket_name.size()) + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return false;
    }
    QByteArray request;
    request.append('A');
    const uint32_t length = htonl(static_cast<uint32_t>(path.size()));
    request.append(reinterpret_cast<const char*>(&length), 4);
    request.append(path);
    for (int pos = 0; pos < request.size();) {
        const int amount = one_byte_at_time ? 1 : request.size() - pos;
        const ssize_t written = ::send(fd, request.constData() + pos, amount, MSG_NOSIGNAL);
        if (written <= 0) { ::close(fd); return false; }
        pos += static_cast<int>(written);
        if (one_byte_at_time) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    char header[5];
    std::size_t received = 0;
    while (received < sizeof(header)) {
        const ssize_t n = ::recv(fd, header + received, sizeof(header) - received, 0);
        if (n <= 0) { ::close(fd); return false; }
        received += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return static_cast<unsigned char>(header[0]) == 0;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir folder;
    if (!folder.isValid()) return 1;
    const QString first = folder.filePath("first.txt");
    const QString second = folder.filePath("second.txt");
    const QString third = folder.filePath(QString::fromUtf8("two words è\nnewline.txt"));
    if (!create_file(first, "first document text") ||
        !create_file(second, "second document text") ||
        !create_file(third, "third document text")) return 2;

    // r1 the main window remains responsive during the real delayed build
    // r2 a build from an earlier workspace must not become current
    {
        MainWindow window;
        if (!window.add_document_file(first)) return 3;
        window.set_build_delay_ms(500);
        auto* workspace = window.findChild<aiws::gui::WorkspaceWidget*>();
        // drive the same button signal that the gui sends
        if (!workspace) return 4;
        QElapsedTimer clock;
        clock.start();
        QMetaObject::invokeMethod(workspace, "build_corpus_requested", Qt::DirectConnection);
        if (!wait_label(window, "Building", 100)) return 5;
        if (!window.add_document_file(second)) return 6;
        if (clock.elapsed() > 300) return 7;
        QThread::msleep(550);
        QCoreApplication::processEvents();
        if (has_label(window, "Corpus: 1") || has_label(window, "Corpus built")) return 8;
        if (!has_label(window, "Not built")) return 9;
        QMetaObject::invokeMethod(workspace, "build_corpus_requested", Qt::DirectConnection);
        if (!wait_label(window, "Corpus built", 2500)) return 10;
        if (!has_label(window, "chunks")) return 11;
    }

    // r3 a failed worker must report the error and become usable again
    {
        MainWindow window;
        if (!window.add_document_file(first)) return 12;
        auto* workspace = window.findChild<aiws::gui::WorkspaceWidget*>();
        if (!workspace) return 13;
        window.fail_next_build_for_test();
        QMetaObject::invokeMethod(workspace, "build_corpus_requested", Qt::DirectConnection);
        if (!wait_label(window, "Build failed", 1700)) return 14;
        QMetaObject::invokeMethod(workspace, "build_corpus_requested", Qt::DirectConnection);
        if (!wait_label(window, "Corpus built", 1700)) return 15;
    }

    // e1 to e3 the receiver accepts paths including unicode and newlines
    // and receives messages split across several socket writes
    {
        MainWindow window;
        const QString socket_name = folder.filePath("aiws.sock");
        if (!window.start_remote_add(socket_name.toStdString())) return 16;
        if (!socket_request(socket_name, third.toUtf8(), true)) return 17;
        if (!wait_label(window, "Added document", 800)) return 18;
        auto* list = window.findChild<QListWidget*>();
        if (!list || list->count() != 1) return 19;
        // e4 quitting during a slow build must cancel delays and remove the socket
        window.set_build_delay_ms(1000);
        auto* workspace = window.findChild<aiws::gui::WorkspaceWidget*>();
        if (!workspace) return 20;
        QMetaObject::invokeMethod(workspace, "build_corpus_requested", Qt::DirectConnection);
    }
    if (QFile::exists(folder.filePath("aiws.sock"))) return 21;
    std::cout << "student tests passed\n";
    return 0;
}
