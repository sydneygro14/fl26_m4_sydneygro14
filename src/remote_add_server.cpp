/*
this file runs the remote add socket server
it listens for requests on a separate thread
it checks incoming document paths and sends a reply
it lets the gui add accepted documents through a callback
*/

#include "aiws/remote_add_server.hpp"

#include <arpa/inet.h>  // convert message lengths to network byte order
#include <cerrno>       // read socket errors
#include <chrono>      // track timeouts
#include <cstddef>     // find the size of the socket address
#include <cstdint>     // use fixed size message lengths
#include <cstdlib>     // read the user name
#include <cstring>     // copy the socket path
#include <fcntl.h>     // open files and set socket flags
#include <poll.h>      // wait for data without blocking forever
#include <sys/socket.h> // create unix sockets
#include <sys/stat.h>   // check document type
#include <sys/un.h>     // use unix domain socket addresses
#include <unistd.h>     // close sockets and remove the socket file
#include <utility>      // move the callback

namespace aiws {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr std::uint32_t max_path_size = 65535;
constexpr std::uint32_t max_reason_size = 65535;

// this wrapper closes a socket when the function ends
struct Socket {
    explicit Socket(int value = -1) : fd(value) {}
    ~Socket() { if (fd >= 0) ::close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int fd;
};

// this makes the socket return instead of waiting forever
bool set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// this waits for data or space while also checking for shutdown
bool wait_ready(int fd, short event, Deadline deadline, const std::atomic<bool>& stop) {
    while (!stop.load()) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        if (remaining <= 0) return false;
        int wait_ms = static_cast<int>(remaining > 50 ? 50 : remaining);
        pollfd info{fd, event, 0};
        int result = ::poll(&info, 1, wait_ms);
        if (result > 0) return (info.revents & event) != 0;
        if (result < 0 && errno != EINTR) return false;
    }
    return false;
}

// this reads the exact number of bytes in one message part
bool read_all(int fd, void* data, std::size_t size, Deadline deadline,
              const std::atomic<bool>& stop) {
    auto* bytes = static_cast<unsigned char*>(data);
    std::size_t received = 0;
    while (received < size) {
        if (!wait_ready(fd, POLLIN, deadline, stop)) return false;
        ssize_t count = ::recv(fd, bytes + received, size - received, 0);
        if (count > 0) received += static_cast<std::size_t>(count);
        else if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else return false;
    }
    return true;
}

// this writes all bytes even when send writes only some
bool write_all(int fd, const void* data, std::size_t size, Deadline deadline,
               const std::atomic<bool>& stop) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t sent = 0;
    while (sent < size) {
        if (!wait_ready(fd, POLLOUT, deadline, stop)) return false;
        ssize_t count = ::send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (count > 0) sent += static_cast<std::size_t>(count);
        else if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else return false;
    }
    return true;
}

// this sends one status byte and an optional explanation
bool write_reply(int fd, unsigned char status, const std::string& reason,
                 Deadline deadline, const std::atomic<bool>& stop) {
    if (reason.size() > max_reason_size) return false;
    std::uint32_t length = htonl(static_cast<std::uint32_t>(reason.size()));
    return write_all(fd, &status, 1, deadline, stop) &&
           write_all(fd, &length, sizeof(length), deadline, stop) &&
           write_all(fd, reason.data(), reason.size(), deadline, stop);
}

// this checks if the document can be opened and read
bool readable_file(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos) return false;
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat info{};
    bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
    ::close(fd);
    return ok;
}

} // namespace

std::string default_ipc_socket_path() {
    const char* user = std::getenv("USER");
    std::string name = user && *user ? user : std::to_string(::getuid());
    return "/tmp/aiws-" + name + ".sock";
}

RemoteAddServer::RemoteAddServer(AcceptedHandler on_accepted)
    : on_accepted_(std::move(on_accepted)) {}

RemoteAddServer::~RemoteAddServer() { stop(); }

bool RemoteAddServer::start(const std::string& path, std::string& error) {
    if (worker_.joinable() || listen_fd_ >= 0) {
        error = "server already running";
        return false;
    }
    sockaddr_un address{};
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        error = "socket path is empty or too long";
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    Socket listener(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (listener.fd < 0 || !set_nonblocking(listener.fd)) {
        error = "could not create listening socket";
        return false;
    }
    auto address_size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), address_size) < 0) {
        error = "could not bind socket path try another path or remove a stale socket";
        return false;
    }
    if (::listen(listener.fd, 16) < 0) {
        ::unlink(path.c_str());
        error = "could not listen on socket";
        return false;
    }
    socket_path_ = path;
    stopping_.store(false);
    listen_fd_ = listener.fd;
    listener.fd = -1;
    try {
        worker_ = std::thread(&RemoteAddServer::serve, this);
    } catch (...) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(socket_path_.c_str());
        socket_path_.clear();
        error = "could not start the server thread";
        return false;
    }
    return true;
}

void RemoteAddServer::stop() {
    stopping_.store(true);
    if (worker_.joinable()) worker_.join();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (!socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
        socket_path_.clear();
    }
}

void RemoteAddServer::serve() {
    while (!stopping_.load()) {
        pollfd info{listen_fd_, POLLIN, 0};
        int result = ::poll(&info, 1, 50);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) continue;
        if (!(info.revents & POLLIN)) continue;
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) continue;
        Socket client(fd);
        if (!set_nonblocking(client.fd)) continue;
        handle_client(client.fd);
    }
}

void RemoteAddServer::handle_client(int client_fd) {
    Deadline deadline = Clock::now() + std::chrono::seconds(5);
    unsigned char command = 0;
    std::uint32_t network_size = 0;
    if (!read_all(client_fd, &command, 1, deadline, stopping_) ||
        !read_all(client_fd, &network_size, sizeof(network_size), deadline, stopping_)) {
        return;
    }
    std::uint32_t path_size = ntohl(network_size);
    if (command != 'A' || path_size == 0 || path_size > max_path_size) {
        write_reply(client_fd, 1, "invalid command or path length", deadline, stopping_);
        return;
    }
    std::string path(path_size, '\0');
    if (!read_all(client_fd, path.data(), path.size(), deadline, stopping_)) return;
    if (!readable_file(path)) {
        write_reply(client_fd, 1, "file is unreadable or not a regular file", deadline, stopping_);
        return;
    }
    // queue the accepted path without waiting for the gui thread
    // the callback must not directly change any qt widgets
    try {
        on_accepted_(path);
    } catch (...) {
        write_reply(client_fd, 1, "unable to queue document", deadline, stopping_);
        return;
    }
    write_reply(client_fd, 0, "", deadline, stopping_);
}

} // namespace aiws
