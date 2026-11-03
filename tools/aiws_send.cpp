
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














// sends all the bytes in a message
// send might only send some of the bytes
// the loop keeps going until everything is sent

bool send_all(int socket_fd, const char* data, std::size_t size) {

    // start with zero bytes sent


    // keep sending while there are bytes left


        // send the bytes that have not been sent yet


        // check if some bytes were sent


            // add the new bytes to the total


        // try again if the call was interrupted


        // stop if sending failed


    // return true because all bytes were sent

}


// this function packages the document path into a message
// the first byte is the add command
// the next four bytes store the path length
// the remaining bytes store the file path

bool send_add_request(int socket_fd, const std::string& file_path) {

    // check if the path length fits in four bytes


    // use a to represent the add command


    // convert the path length to network byte order


    // send the add command


    // send the four bytes containing the path length


    // send the document path


    // return true because the request was sent

}


// this function receives a specific number of bytes
// recv might return fewer bytes than requested
// the loop continues until everything arrives

bool receive_all(int socket_fd, char* data, std::size_t size) {

    // start with zero bytes received


    // keep receiving while there are bytes left


        // receive the remaining bytes


        // check if some bytes were received


            // add the new bytes to the total


        // try again if the call was interrupted


        // stop if the connection closed or failed


    // return true because all bytes were received

}


// this function reads the response from ai workspace
// the response contains a status and an optional reason

bool receive_response(int socket_fd, int& status, std::string& reason) {

    // create a variable for the response status


    // read the response status


    // check that the status is accepted or rejected


    // create a variable for the reason length


    // read the four bytes containing the reason length


    // convert the length into the computers byte order


    // check that the reason is not too large


    // create a string with enough space for the reason


    // read the reason text if there is any


        // read the reason into the string


    // save the status and reason


    // return true because the response was received

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