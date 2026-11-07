#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace aiws {

// this class listens for remote document requests
// it runs socket communication outside the gui thread
class RemoteAddServer {
public:
    using AcceptedHandler = std::function<void(const std::string&)>;

    explicit RemoteAddServer(AcceptedHandler on_accepted);
    ~RemoteAddServer();

    RemoteAddServer(const RemoteAddServer&) = delete;
    RemoteAddServer& operator=(const RemoteAddServer&) = delete;

    // start listening at the given socket path
    bool start(const std::string& socket_path, std::string& error);

    // stop the worker and remove its socket file
    void stop();

private:
    void serve();
    void handle_client(int client_fd);

    AcceptedHandler on_accepted_;
    std::string socket_path_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    int listen_fd_ = -1;
};

// choose the same default socket path as aiws_send
std::string default_ipc_socket_path();

} // namespace aiws
