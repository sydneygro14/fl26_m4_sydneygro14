
/*
this file is the starting point for the aiws_send program
it will let another program send a document path to the ai workspace
right now it handles command line arguments and file paths
the socket connection and message handling will be added during pairing
*/


#include <cstdlib>       
#include <filesystem>    
#include <iostream>      
#include <string>       
#include <system_error>  
#include <unistd.h>      

#include <arpa/inet.h>   // converts numbers to network byte order
#include <cerrno>        // check socket errors
#include <cstdint>       // lets us use fixed size numbers
#include <limits>        // check number limits
#include <sys/socket.h>  // send and receive data



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

// sends all the bytes in a message
// send might only send some of the bytes
// the loop keeps going until everything is sent

bool send_all(int socket_fd, const char* data, std::size_t size) {

    // start with zero bytes sent
    std::size_t total_sent = 0;

    // keep sending while there are bytes left
    while (total_sent < size) {

        // send the bytes that have not been sent yet
        ssize_t sent = send(
            socket_fd,           // the socket we are sending through
            data + total_sent,   // start at the first unsent byte
            size - total_sent,   // number of bytes still left to send
            MSG_NOSIGNAL         // stops a broken connection from ending the program
        );

        // check if some bytes were sent
        if (sent > 0) {

            // add the new bytes to the total
            total_sent += static_cast<std::size_t>(sent);

        } else if (sent < 0 && errno == EINTR) {

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

bool send_add_request(int socket_fd, const std::string& file_path) {

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
    if (!send_all(socket_fd, &command, 1)) {
        return false; // stop if the command could not be sent
    }

    // send the four bytes containing the path length
    if (!send_all(
            socket_fd,                                    // the socket connection
            reinterpret_cast<const char*>(&path_length), // treat the number as bytes
            sizeof(path_length))) {                       // send all four bytes
        return false; // stop if the length could not be sent
    }

    // send the document path
    if (!send_all(
            socket_fd,           // the socket connection
            file_path.data(),    // location of the path text in memory
            file_path.size())) { // number of bytes in the file path
        return false; // stop if the path could not be sent
    }

    // return true because the request was sent
    return true;
}


// this function receives a specific number of bytes
// recv might return fewer bytes than requested
// the loop continues until everything arrives

bool receive_all(int socket_fd, char* data, std::size_t size) {

    // start with zero bytes received
    std::size_t total_received = 0;

    // keep receiving while there are bytes left
    while (total_received < size) {

        // receive the remaining bytes
        ssize_t received = recv(
            socket_fd,                  // the socket we are reading from
            data + total_received,      // where the next bytes should go
            size - total_received,      // number of bytes still needed
            0                           // use normal receiving behavior
        );

        // check if some bytes were received
        if (received > 0) {

            // add the new bytes to the total
            total_received += static_cast<std::size_t>(received);

        } else if (received < 0 && errno == EINTR) {

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

bool receive_response(int socket_fd, int& status, std::string& reason) {

    // create a variable for the response status
    unsigned char response_status = 0;

    // read the response status
    if (!receive_all(
            socket_fd,                                    // the socket connection
            reinterpret_cast<char*>(&response_status),   // where the status is stored
            1)) {                                         // read one byte
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
            sizeof(reason_length))) {                   // read four bytes
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
                message.size())) {  // number of bytes to read
            return false; // stop if the reason could not be read
        }
    }

    // save the status and reason
    status = response_status;
    reason = message;

    // return true because the response was received
    return true;
}





////////////////////////////////////////////
// PROGRAM START

int main(int argc, char* argv[]) {

    SendOptions options; // cmnd line options
    if (!parse_options(argc, argv, options)) { // check if arguments are valid
        print_usage(); // show correct usage if invalid
        return 1; // returns error code
    }

    // socket communication will be added during pair programming
    std::cerr << "socket communication is not implemented yet\n";

    // return 2 because no server communication happened yet
    return 2;
}


Send loop:
1. Connects to the AI Workspace socket.
2. Starts sending the packaged message.
3. Checks how many bytes were successfully sent.
4. Keeps sending until the entire message is transferred or an error occurs.
Receive loop:
1. Waits for the response from AI Workspace.
2. Reads the incoming bytes.
3. If only part of the response arrives, keeps reading.
4. Stops when the full response arrives or an error occurs.
5. Determines whether the request was accepted or rejected.