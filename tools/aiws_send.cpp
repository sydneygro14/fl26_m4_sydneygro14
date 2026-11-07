
/*
starting point for the aiws_send program
leta another program send a document path to the ai workspace
*/


#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <sys/socket.h>

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/un.h>



// struct that stores the information needed to send a file
struct SendOptions {
    std::string socket_path; // path to the socket
    std::string file_path;   //path to the document
};


// finds the default socket path
std::string default_socket_path() {

    const char* user = std::getenv("USER"); // gets the user name

    std::string name; // store username in string

    if (user != nullptr && user[0] != '\0') { // checks if name exists/not empty
        name = user; // use usernmene

    } else {
        name = std::to_string(getuid()); // use the numeric user id if no user name exists

    }

    // return the full default socket path
    return "/tmp/aiws-" + name + ".sock";
}

// prints how to use the program
void print_usage() {

    std::cerr << "usage: aiws_send [--socket PATH] add FILE\n";
}


// this function reads and checks the command line arguments
bool parse_options(int argc, char* argv[], SendOptions& options) {

    // start at the first argument after the program name
    int index = 1;

    // use the default socket path unless another path is given
    options.socket_path = default_socket_path();

    // check if the user entered the optional socket argument
    if (index < argc && std::string(argv[index]) == "--socket") {

        // check if the socket argument is missing its path
        if (index + 1 >= argc) {
            return false;
        }
        // save the socket path entered by the user
        options.socket_path = argv[index + 1];
        index += 2;
    }

    // check that there are exactly two arguments left
    // also check that the command is add
    if (argc - index != 2 || std::string(argv[index]) != "add") {

        // return false if the command format is wrong
        return false;
    }


//// Error checking: //////////////
    // stores any file path errors
    std::error_code error;

    // convert the file path into an absolute path
    std::filesystem::path file =
        std::filesystem::absolute(argv[index + 1], error);

    // check if converting the file path caused an error
    if (error) {
        std::cerr << "could not read file path: "
                  << error.message() << "\n"; // prints error message
        return false;
    }
    // check if the socket path is empty
    if (options.socket_path.empty()) {
        return false;
    }


    // save the cleaned absolute file path so it can be used
    options.file_path = file.lexically_normal().string();
    return true;
}





////////// PAIR PROGRAMMING ///////////////

// this stores the time when communication must end
using Deadline = std::chrono::steady_clock::time_point;

// this waits for the socket without going past the deadline
bool wait_for_socket(int socket_fd, short events, Deadline deadline) {

    // retry if a signal interrupts the wait
    while (true) {

        // calculate how much time is left
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();

        // stop once the deadline has passed
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }

        // wait at least one millisecond if time is still left
        int milliseconds = remaining > 0 ? static_cast<int>(remaining) : 1;

        // choose whether we are waiting to send or receive
        pollfd info{socket_fd, events, 0};

        // wait for the socket to be ready
        int result = poll(&info, 1, milliseconds);

        // stop if the socket is ready or has an error
        if (result > 0) {
            return (info.revents & events) != 0;
        }

        // stop if the wait used up the deadline
        if (result == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            continue;
        }

        // keep waiting if a signal interrupted the call
        if (errno == EINTR) {
            continue;
        }

        // stop on any other error
        return false;
    }
}

// sends all the bytes in a message
// send might only send some of the bytes
// the loop keeps going until everything is sent

bool send_all(int socket_fd, const char* data, std::size_t size, Deadline deadline) {

    // start with zero bytes sent
    std::size_t total_sent = 0;

    // keep sending while there are bytes left
    while (total_sent < size) {

        // wait until sending is possible within our deadline
        if (!wait_for_socket(socket_fd, POLLOUT, deadline)) {
            return false;
        }

        // send the bytes that have not been sent yet
        ssize_t sent = send(
            socket_fd,           // the socket we are sending through
            data + total_sent,   // start at the first unsent byte
            size - total_sent,   // number of bytes still left to send
            MSG_NOSIGNAL | MSG_DONTWAIT // stops a broken connection from ending the program
        );

        // check if some bytes were sent
        if (sent > 0) {

            // add the new bytes to the total
            total_sent += static_cast<std::size_t>(sent);

        } else if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {

            // try again if the call was interrupted
            continue;

        } else {

            // stop if sending failed
            return false;
        }
    }

    // return true because all bytes were sent
    return true;
}


// this function packages the document path into a message
// the first byte is the add command
// the next four bytes store the path length
// the remaining bytes store the file path

bool send_add_request(int socket_fd, const std::string& file_path, Deadline deadline) {

    // check if the path length fits in four bytes
    if (file_path.size() > std::numeric_limits<uint32_t>::max()) {
        return false; // stop if the path is too long
    }

    // use a to represent the add command
    char command = 'A';

    // convert the path length to network byte order
    uint32_t path_length =
        htonl(static_cast<uint32_t>(file_path.size()));

    // send the add command
    if (!send_all(socket_fd, &command, 1, deadline)) {
        return false; // stop if the command could not be sent
    }

    // send the four bytes containing the path length
    if (!send_all(
            socket_fd,                                    // the socket connection
            reinterpret_cast<const char*>(&path_length), // treat the number as bytes
            sizeof(path_length), deadline)) {             // send all four bytes
        return false; // stop if the length could not be sent
    }

    // send the document path
    if (!send_all(
            socket_fd,           // the socket connection
            file_path.data(),    // location of the path text in memory
            file_path.size(), deadline)) { // number of bytes in the file path
        return false; // stop if the path could not be sent
    }

    // return true because the request was sent
    return true;
}


// this function receives a specific number of bytes
// recv might return fewer bytes than requested
// the loop continues until everything arrives

bool receive_all(int socket_fd, char* data, std::size_t size, Deadline deadline) {

    // start with zero bytes received
    std::size_t total_received = 0;

    // keep receiving while there are bytes left
    while (total_received < size) {

        // wait until data is available within our deadline
        if (!wait_for_socket(socket_fd, POLLIN, deadline)) {
            return false;
        }

        // receive the remaining bytes
        ssize_t received = recv(
            socket_fd,                  // the socket we are reading from
            data + total_received,      // where the next bytes should go
            size - total_received,      // number of bytes still needed
            MSG_DONTWAIT                // use normal receiving behavior
        );

        // check if some bytes were received
        if (received > 0) {

            // add the new bytes to the total
            total_received += static_cast<std::size_t>(received);

        } else if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {

            // try again if the call was interrupted
            continue;

        } else {

            // stop if the connection closed or failed
            return false;
        }
    }

    // return true because all bytes were received
    return true;
}


// this function reads the response from ai workspace
// the response contains a status and an optional reason

bool receive_response(int socket_fd, int& status, std::string& reason, Deadline deadline) {

    // create a variable for the response status
    unsigned char response_status = 0;

    // read the response status
    if (!receive_all(
            socket_fd,                                    // the socket connection
            reinterpret_cast<char*>(&response_status),   // where the status is stored
            1, deadline)) {                               // read one byte
        return false; // stop if the status could not be read
    }

    // check that the status is accepted or rejected
    if (response_status != 0 && response_status != 1) {
        return false; // stop if the status is invalid
    }

    // create a variable for the reason length
    uint32_t reason_length = 0;

    // read the four bytes containing the reason length
    if (!receive_all(
            socket_fd,                                  // the socket connection
            reinterpret_cast<char*>(&reason_length),   // where the length is stored
            sizeof(reason_length), deadline)) {         // read four bytes
        return false; // stop if the length could not be read
    }

    // convert the length into the computers byte order
    reason_length = ntohl(reason_length);

    // check that the reason is not too large
    if (reason_length > 65536) {
        return false; // stop if the response is too large
    }

    // create a string with enough space for the reason
    std::string message(reason_length, '\0');

    // read the reason text if there is any
    if (reason_length > 0) {

        // read the reason into the string
        if (!receive_all(
                socket_fd,          // the socket connection
                message.data(),     // where the reason text is stored
                message.size(), deadline)) {  // number of bytes to read
            return false; // stop if the reason could not be read
        }
    }

    // save the status and reason
    status = response_status;
    reason = message;

    // return true because the response was received
    return true;
}




// this function connects to the running ai workspace
// it returns a socket number or negative one if it fails

int connect_to_server(const std::string& socket_path) {

    // create a unix socket for communication
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    // check if creating the socket failed
    if (socket_fd < 0) {
        return -1;
    }

    // create an address to store the socket path
    sockaddr_un address{};
    address.sun_family = AF_UNIX;

    // make sure the socket path is not too long
    if (socket_path.size() >= sizeof(address.sun_path)) {
        close(socket_fd);
        return -1;
    }

    // copy the socket path into the address
    std::memcpy(address.sun_path, socket_path.c_str(),
                socket_path.size() + 1);

    // get the current socket settings
    int flags = fcntl(socket_fd, F_GETFL, 0);

    // stop if the settings could not be read
    if (flags < 0 || fcntl(socket_fd, F_SETFL,
                           flags | O_NONBLOCK) < 0) {
        close(socket_fd);
        return -1;
    }

    // try connecting to ai workspace
    int result = connect(socket_fd,
                         reinterpret_cast<sockaddr*>(&address),
                         sizeof(address));

    // check if the connection is still starting
    if (result < 0 && errno == EINPROGRESS) {

        // set the two second connection deadline
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(2);

        // wait until the connection is ready or time runs out
        while (true) {

            // calculate how much time is left
            auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();

            // stop if the deadline has passed
            if (remaining <= 0) {
                close(socket_fd);
                return -1;
            }

            // check when the socket is ready
            pollfd info{socket_fd, POLLOUT, 0};
            int ready = poll(&info, 1, static_cast<int>(remaining));

            // stop waiting when the socket responds
            if (ready > 0) {
                break;
            }

            // retry if the wait was interrupted
            if (ready < 0 && errno == EINTR) {
                continue;
            }

            // stop if the connection times out or fails
            close(socket_fd);
            return -1;
        }

        // check whether the connection succeeded
        int error = 0;
        socklen_t error_size = sizeof(error);

        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR,
                       &error, &error_size) < 0 || error != 0) {
            close(socket_fd);
            return -1;
        }

    } else if (result < 0) {

        // stop if connecting failed immediately
        close(socket_fd);
        return -1;
    }

    // change the socket back to blocking mode
    if (fcntl(socket_fd, F_SETFL, flags) < 0) {
        close(socket_fd);
        return -1;
    }

    // set timeouts for sending and receiving
    timeval timeout{};
    timeout.tv_sec = 5;

    // stop if either socket timeout cannot be set
    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) < 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0) {
        close(socket_fd);
        return -1;
    }

    // return the connected socket
    return socket_fd;
}


// sends a document request and checks the reply
// uses the send and receive functions already written

int send_add(const SendOptions& options) {

    // make sure the path is within the agreed message limit
    if (options.file_path.size() > 65535) {
        std::cerr << "file path is too long\n";
        return 1;
    }

    // connect to the socket using the chosen path
    int socket_fd = connect_to_server(options.socket_path);

    // return two if the application could not be reached
    if (socket_fd < 0) {
        std::cerr << "could not connect to ai workspace\n";
        return 2;
    }

    // use one deadline for both sending and receiving
    Deadline deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    // send the document path using the existing function
    bool sent = send_add_request(socket_fd, options.file_path, deadline);

    // stop if the request could not be sent
    if (!sent) {
        std::cerr << "could not send request\n";
        close(socket_fd);
        return 2;
    }

    // create variables to store the response
    int status = 0;
    std::string reason;

    // read the response using the existing function
    bool received = receive_response(socket_fd, status, reason, deadline);

    // close the socket because communication is finished
    close(socket_fd);

    // return two if the response could not be read
    if (!received) {
        std::cerr << "could not receive response\n";
        return 2;
    }

    // print the reason if the server rejected the request
    if (status == 1) {
        std::cerr << (reason.empty() ? "document rejected" : reason)
                  << "\n";
    }

    // return zero for accepted or one for rejected
    return status;
}



////////////////////////////////////////////
// PROGRAM START

// checks the command then sends the document request

int main(int argc, char* argv[]) {

    // create a variable to store the command options
    SendOptions options;

    // check the arguments entered in the terminal
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 1;
    }

    // connect to ai workspace and return the result
    return send_add(options);
}
