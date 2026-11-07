/*
this file starts ai workspace and sets up remote add
it reads the socket option from the command line
it creates the gui and starts the socket server
it asks the user to confirm before exiting
*/

#include "aiws/gui/main_window.hpp"
#include "aiws/remote_add_server.hpp"
#include <QApplication>
#include <QMessageBox>
#include <QString>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

// this asks before closing the application
void confirm_exit(QWidget* parent) {
    auto answer = QMessageBox::question(
        parent, "Exit", "Quit the application?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes) QApplication::quit();
}

// this reads a nonnegative number for the build delay option
bool parse_delay(const char* text, int& value) {
    if (!text || *text == '\0') return false;
    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 0 || parsed > INT_MAX) return false;
    value = static_cast<int>(parsed);
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    // read our custom options before creating the qt application
    std::string socket_path = aiws::default_ipc_socket_path();
    int delay_ms = 0;
    for (int i = 1; i < argc; ++i) {
        std::string option = argv[i];
        if (option == "--ipc-socket") {
            if (++i >= argc || argv[i][0] == '\0') {
                std::cerr << "missing --ipc-socket path\n";
                return 1;
            }
            socket_path = argv[i];
        } else if (option == "--build-delay-ms") {
            if (++i >= argc || !parse_delay(argv[i], delay_ms)) {
                std::cerr << "invalid --build-delay-ms value\n";
                return 1;
            }
        } else {
            std::cerr << "unknown option: " << option << '\n';
            return 1;
        }
    }

    // start qt after reading the custom options
    QApplication application(argc, argv);
    aiws::gui::MainWindow window;
    window.set_exit_callback(&confirm_exit);
    window.set_build_delay_ms(delay_ms);

    // start the listening socket before showing the window
    if (!window.start_remote_add(socket_path)) {
        std::cerr << "could not start remote add server at " << socket_path << '\n';
        return 1;
    }
    window.show();
    return application.exec();
}
