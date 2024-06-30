#include "as_server.h"


int init_server_addr(int port, struct sockaddr_in *addr){
    // Allow sockets across machines.
    addr->sin_family = AF_INET;
    // The port the process will listen on.
    addr->sin_port = htons(port);
    // Clear this field; sin_zero is used for padding for the struct.
    memset(&(addr->sin_zero), 0, 8);

    // Listen on all network interfaces.
    addr->sin_addr.s_addr = INADDR_ANY;

    return 0;
}


int set_up_server_socket(const struct sockaddr_in *server_options, int num_queue) {
    int soc = socket(AF_INET, SOCK_STREAM, 0);
    if (soc < 0) {
        perror("socket");
        exit(1);
    }

    printf("Listen socket created\n");

    // Make sure we can reuse the port immediately after the
    // server terminates. Avoids the "address in use" error
    int on = 1;
    int status = setsockopt(soc, SOL_SOCKET, SO_REUSEADDR,
                            (const char *) &on, sizeof(on));
    if (status < 0) {
        perror("setsockopt");
        exit(1);
    }

    // Associate the process with the address and a port
    if (bind(soc, (struct sockaddr *)server_options, sizeof(*server_options)) < 0) {
        // bind failed; could be because port is in use.
        perror("bind");
        exit(1);
    }

    printf("Socket bound to port %d\n", ntohs(server_options->sin_port));

    // Set up a queue in the kernel to hold pending connections.
    if (listen(soc, num_queue) < 0) {
        // listen failed
        perror("listen");
        exit(1);
    }

    printf("Socket listening for connections\n");

    return soc;
}


ClientSocket accept_connection(int listenfd) {
    ClientSocket client;
    socklen_t addr_size = sizeof(client.addr);
    client.socket = accept(listenfd, (struct sockaddr *)&client.addr,
                               &addr_size);
    if (client.socket < 0) {
        perror("accept_connection: accept");
        exit(-1);
    }

    // print out a message that we got the connection
    printf("Server got a connection from %s, port %d\n",
           inet_ntoa(client.addr.sin_addr), ntohs(client.addr.sin_port));

    return client;
}

// Request response functions
/*
** List the files in the library. The list is returned as a single string
** with each file starting with an integer corresponding to it's index in
** the library, in reverse order. A colon seperates the index and the file's
** name. Each entry is separated by a network newline "\r\n" (2 chars).
** Filenames must not contain newline characters.
**
** For example, if the library contains the files "file1.wav",
** "artist/file2.wav", and "artist/album/file3.wav" in this order,
** the data sent to the client will be the following characters:
** "2:artist/album/file3.wav\r\n1:artist/file2.wav\r\n0:file1.wav\r\n"
**
** Notes:
**   -- the null character is not included in the message sent to the client.
**
** return 0 on success, -1 on error
*/
int list_request_response(const ClientSocket * client, const Library *library) {
    if (client == NULL || library == NULL) {
        ERR_PRINT("Invalid arguments.\n");
        return -1;
    }

    // Calculate the total size needed for the response
    size_t totalSize = 0;
    for (int i = 0; i < library->num_files; i++) {
        // +1 for the colon, +2 for the "\r\n", and +10 for the index (generously assuming no more than 10^9 files)
        totalSize += strlen(library->files[i]) + 1 + 2 + 10;
    }

    // Allocate the response string
    char *response = (char *)malloc(totalSize + 1); // +1 for the null terminator
    if (response == NULL) {
        ERR_PRINT("Failed to allocate memory for list response.\n");
        return -1;
    }

    // Construct the response string
    size_t offset = 0;
    for (int i = library->num_files - 1; i >= 0; i--) {
        int bytes_written = snprintf(response + offset, totalSize + 1 - offset,
                                     "%d:%s\r\n", i, library->files[i]);
        if (bytes_written < 0) {
            ERR_PRINT("Error formatting library entry.\n");
            free(response);
            return -1;
        }
        offset += bytes_written;
    }

    // Send the response to the client
    if (write_precisely(client->socket, response, offset) < 0) {
        ERR_PRINT("Failed to send list response to client.\n");
        free(response);
        return -1;
    }

    // Clean up
    free(response);
    return 0; // Success
}


static int _load_file_size_into_buffer(FILE *file, uint8_t *buffer) {
    if (fseek(file, 0, SEEK_END) < 0) {
        ERR_PRINT("Error seeking to end of file\n");
        return -1;
    }
    uint32_t file_size = ftell(file);
    if (fseek(file, 0, SEEK_SET) < 0) {
        ERR_PRINT("Error seeking to start of file\n");
        return -1;
    }
    buffer[0] = (file_size >> 24) & 0xFF;
    buffer[1] = (file_size >> 16) & 0xFF;
    buffer[2] = (file_size >> 8) & 0xFF;
    buffer[3] = file_size & 0xFF;
    return 0;
}

/*
** Stream a file from the library to the client. The file is streamed in chunks
** of a maximum of STREAM_CHUNK_SIZE bytes. The client will be able to request
** a specific file by its index in the library.
**
** The 32-bit unsigned network byte-order integer file_index will be read
** from the client_socket, but will consider num_pr_bytes (must be <= uint32_t)
** from post_req first, then:
**   The stream will be sent in the following format:
**     - the first 4 bytes (32-bits) will be the file size in network byte-order
**     - the rest of the stream will be the file's data written in chunks of
**       STREAM_CHUNK_SIZE bytes, or less if write returns less than STREAM_CHUNK_SIZE,
**       the file is less than STREAM_CHUNK_SIZE bytes, or the last remaining chunk is
**       less than STREAM_CHUNK_SIZE bytes.
**
** If the file is successfully transported to the client over the client_socket,
** return 0. Otherwise, return -1.
 */
int stream_request_response(const ClientSocket * client, const Library *library,
                            uint8_t *post_req, int num_pr_bytes) {
    if (num_pr_bytes > 4) {
        ERR_PRINT("num_pr_bytes is greater than 4, which is an error.\n");
        return -1;
    }

    // Buffer to hold the file index received from client
    uint8_t file_index_buffer[4];
    if (num_pr_bytes > 0) {
        memcpy(file_index_buffer, post_req, num_pr_bytes);
    }

    // Read the remaining bytes of the file index if necessary
    ssize_t bytes_read;
    while (num_pr_bytes < 4) {
        bytes_read = read(client->socket, file_index_buffer + num_pr_bytes, 4 - num_pr_bytes);
        if (bytes_read <= 0) { // Error or disconnection
            ERR_PRINT("Failed to read file index from client.\n");
            return -1;
        }
        num_pr_bytes += bytes_read;
    }

    // Convert file index from network byte order to host byte order
    uint32_t file_index = ntohl(*((uint32_t *)file_index_buffer));

    // Validate file index
    if (file_index >= library->num_files) {
        ERR_PRINT("File index out of range.\n");
        // Note: The client will hang as no response is sent back
        return -1;
    }

    // Construct file path
    char *file_path = _join_path(library->path, library->files[file_index]);
    if (file_path == NULL) {
        ERR_PRINT("Failed to construct file path.\n");
        return -1;
    }

    // Open the file
    FILE *file = fopen(file_path, "rb");
    free(file_path); // No longer needed
    if (file == NULL) {
        ERR_PRINT("Requested file does not exist.\n");
        return -1;
    }

    // Send file size
    uint8_t file_size_buffer[4];
    if (_load_file_size_into_buffer(file, file_size_buffer) < 0) {
        fclose(file);
        return -1;
    }
    if (write_precisely(client->socket, file_size_buffer, 4) < 0) {
        fclose(file);
        return -1;
    }

    // Stream file content
    uint8_t buffer[STREAM_CHUNK_SIZE];
    int bytes_to_read;
    while ((bytes_to_read = fread(buffer, 1, STREAM_CHUNK_SIZE, file)) > 0) {
        if (write_precisely(client->socket, buffer, bytes_to_read) < 0) {
            fclose(file);
            return -1;
        }
    }

    fclose(file);
    return 0; // Success
}


static Library make_library(const char *path){
    Library library;
    library.path = path;
    library.num_files = 0;
    library.files = NULL;
    library.name = "server";

    printf("Initializing library\n");
    printf("Library path: %s\n", library.path);

    return library;
}


static void _wait_for_children(pid_t **client_conn_pids, int *num_connected_clients, uint8_t immediate) {
    int status;
    for (int i = 0; i < *num_connected_clients; i++) {
        int options = immediate ? WNOHANG : 0;
        if (waitpid((*client_conn_pids)[i], &status, options) > 0) {
            if (WIFEXITED(status)) {
                printf("Client process %d terminated\n", (*client_conn_pids)[i]);
                if (WEXITSTATUS(status) != 0) {
                    fprintf(stderr, "Client process %d exited with status %d\n",
                            (*client_conn_pids)[i], WEXITSTATUS(status));
                }
            } else {
                fprintf(stderr, "Client process %d terminated abnormally\n",
                        (*client_conn_pids)[i]);
            }

            for (int j = i; j < *num_connected_clients - 1; j++) {
                (*client_conn_pids)[j] = (*client_conn_pids)[j + 1];
            }

            (*num_connected_clients)--;
            *client_conn_pids = (pid_t *)realloc(*client_conn_pids,
                                                 (*num_connected_clients)
                                                 * sizeof(pid_t));
        }
    }
}

/*
** Create a server socket and listen for connections
**
** port: the port number to listen on.
** 
** On success, returns the file descriptor of the socket.
** On failure, return -1.
*/
static int initialize_server_socket(int port) {
	struct sockaddr_in server_addr;

    // Initialize server address
    if (init_server_addr(port, &server_addr) < 0) {
        fprintf(stderr, "Failed to initialize server address\n");
        return -1;
    }

    // Set up and get a listening socket
    int listen_fd = set_up_server_socket(&server_addr, MAX_PENDING);
    if (listen_fd < 0) {
        fprintf(stderr, "Failed to set up server socket\n");
        return -1;
    }

    // Socket is now set up and listening
    return listen_fd;
}

/*
** Run the server using the specified port and library directory. The server
** will listen for incoming connections and respond to requests from clients in
** an infinite loop. The loop will terminate if an error occurs or the user types
** q + enter in the server's terminal.
**
** All new connections will be accepted and handled in a child process that will
** exclusively run the handle_client function. The server will continue to listen
** for new connections in the parent process.
**
** If the server is successfully set up and running, this function will never
** return. If any errors occur, the server will terminate with an error message.
*/
int run_server(int port, const char *library_directory){
    Library library = make_library(library_directory);
    if (scan_library(&library) < 0) {
        ERR_PRINT("Error scanning library\n");
        return -1;
    }

    int num_connected_clients = 0;
    pid_t *client_conn_pids = NULL;

	int incoming_connections = initialize_server_socket(port);
	if (incoming_connections == -1) {
		return -1;	
	}
	
    int maxfd = incoming_connections;
    fd_set incoming;
    SET_SERVER_FD_SET(incoming, incoming_connections);
    int num_intervals_without_scan = 0;

    while(1) {
        if (num_intervals_without_scan >= LIBRARY_SCAN_INTERVAL) {
            if (scan_library(&library) < 0) {
                fprintf(stderr, "Error scanning library\n");
                return 1;
            }
            num_intervals_without_scan = 0;
        }

        struct timeval select_timeout = SELECT_TIMEOUT;
        if(select(maxfd + 1, &incoming, NULL, NULL, &select_timeout) < 0){
            perror("run_server");
            exit(1);
        }

        if (FD_ISSET(incoming_connections, &incoming)) {
            ClientSocket client_socket = accept_connection(incoming_connections);

            pid_t pid = fork();
            if(pid == -1){
                perror("run_server");
                exit(-1);
            }
            // child process
            if(pid == 0){
                close(incoming_connections);
                free(client_conn_pids);
                int result = handle_client(&client_socket, &library);
                _free_library(&library);
                close(client_socket.socket);
                return result;
            }
            close(client_socket.socket);
            num_connected_clients++;
            client_conn_pids = (pid_t *)realloc(client_conn_pids,
                                               (num_connected_clients)
                                               * sizeof(pid_t));
            client_conn_pids[num_connected_clients - 1] = pid;
        }
        if (FD_ISSET(STDIN_FILENO, &incoming)) {
            if (getchar() == 'q') break;
        }

        num_intervals_without_scan++;
        SET_SERVER_FD_SET(incoming, incoming_connections);

        // Immediate return wait for client processes
        _wait_for_children(&client_conn_pids, &num_connected_clients, 1);
    }

    printf("Quitting server\n");
    close(incoming_connections);
    _wait_for_children(&client_conn_pids, &num_connected_clients, 0);
    _free_library(&library);
    return 0;
}


static uint8_t _is_file_extension_supported(const char *filename){
    static const char *supported_file_exts[] = SUPPORTED_FILE_EXTS;

    for (int i = 0; i < sizeof(supported_file_exts)/sizeof(char *); i++) {
        char *files_ext = strrchr(filename, '.');
        if (files_ext != NULL && strcmp(files_ext, supported_file_exts[i]) == 0) {
            return 1;
        }
    }

    return 0;
}


static int _depth_scan_library(Library *library, char *current_path){

    char *path_in_lib = _join_path(library->path, current_path);
    if (path_in_lib == NULL) {
        return -1;
    }

    DIR *dir = opendir(path_in_lib);
    if (dir == NULL) {
        perror("scan_library");
        return -1;
    }
    free(path_in_lib);

    struct dirent *entry;
    while((entry = readdir(dir)) != NULL) {
        if ((entry->d_type == DT_REG) &&
            _is_file_extension_supported(entry->d_name)) {
            library->files = (char **)realloc(library->files,
                                              (library->num_files + 1)
                                              * sizeof(char *));
            if (library->files == NULL) {
                perror("_depth_scan_library");
                return -1;
            }

            library->files[library->num_files] = _join_path(current_path, entry->d_name);
            if (library->files[library->num_files] == NULL) {
                perror("scan_library");
                return -1;
            }
            #ifdef DEBUG
            printf("Found file: %s\n", library->files[library->num_files]);
            #endif
            library->num_files++;

        } else if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char *new_path = _join_path(current_path, entry->d_name);
            if (new_path == NULL) {
                return -1;
            }

            #ifdef DEBUG
            printf("Library scan descending into directory: %s\n", new_path);
            #endif

            int ret_code = _depth_scan_library(library, new_path);
            free(new_path);
            if (ret_code < 0) {
                return -1;
            }
        }
    }

    closedir(dir);
    return 0;
}


// This function is implemented recursively and uses realloc to grow the files array
// as it finds more files in the library. It ignores MAX_FILES.
int scan_library(Library *library) {
    // Maximal flexibility, free the old strings and start again
    // A hash table leveraging inode number would be a better way to do this
    #ifdef DEBUG
    printf("^^^^ ----------------------------------- ^^^^\n");
    printf("Freeing library\n");
    #endif
    _free_library(library);

    #ifdef DEBUG
    printf("Scanning library\n");
    #endif
    int result = _depth_scan_library(library, "");
    #ifdef DEBUG
    printf("vvvv ----------------------------------- vvvv\n");
    #endif
    return result;
}


int handle_client(const ClientSocket * client, Library *library) {
    char *request = NULL;
    uint8_t *request_buffer = (uint8_t *)malloc(REQUEST_BUFFER_SIZE);
    if (request_buffer == NULL) {
        perror("handle_client");
        return 1;
    }
    uint8_t *buff_end = request_buffer;

    int bytes_read = 0;
    int bytes_in_buf = 0;
    while((bytes_read = read(client->socket, buff_end, REQUEST_BUFFER_SIZE - bytes_in_buf)) > 0){
        #ifdef DEBUG
        printf("Read %d bytes from client\n", bytes_read);
        #endif

        bytes_in_buf += bytes_read;

        request = find_network_newline((char *)request_buffer, &bytes_in_buf);

        if (request && strcmp(request, REQUEST_LIST) == 0) {
            if (list_request_response(client, library) < 0) {
                ERR_PRINT("Error handling LIST request\n");
                goto client_error;
            }

        } else if (request && strcmp(request, REQUEST_STREAM) == 0) {
            int num_pr_bytes = MIN(sizeof(uint32_t), (unsigned long)bytes_in_buf);
            if (stream_request_response(client, library, request_buffer, num_pr_bytes) < 0) {
                ERR_PRINT("Error handling STREAM request\n");
                goto client_error;
            }
            bytes_in_buf -= num_pr_bytes;
            memmove(request_buffer, request_buffer + num_pr_bytes, bytes_in_buf);

        } else if (request) {
            ERR_PRINT("Unknown request: %s\n", request);
        }

        free(request); request = NULL;
        buff_end = request_buffer + bytes_in_buf;

    }
    if (bytes_read < 0) {
        perror("handle_client");
        goto client_error;
    }

    printf("Client on %s:%d disconnected\n",
           inet_ntoa(client->addr.sin_addr),
           ntohs(client->addr.sin_port));

    free(request_buffer);
    if (request != NULL) {
        free(request);
    }
    return 0;
client_error:
    free(request_buffer);
    if (request != NULL) {
        free(request);
    }
    return -1;
}


static void print_usage(){
    printf("Usage: as_server [-h] [-p port] [-l library_directory]\n");
    printf("  -h  Print this message\n");
    printf("  -p  Port to listen on (default: " XSTR(DEFAULT_PORT) ")\n");
    printf("  -l  Directory containing the library (default: ./library/)\n");
}


int main(int argc, char * const *argv){
    int opt;
    int port = DEFAULT_PORT;
    const char *library_directory = "library";

    // Check out man 3 getopt for how to use this function
    // The short version: it parses command line options
    // Note that optarg is a global variable declared in getopt.h
    while ((opt = getopt(argc, argv, "hp:l:")) != -1) {
        switch (opt) {
            case 'h':
                print_usage();
                return 0;
            case 'p':
                port = atoi(optarg);
                break;
            case 'l':
                library_directory = optarg;
                break;
            default:
                print_usage();
                return 1;
        }
    }

    printf("Starting server on port %d, serving library in %s\n",
           port, library_directory);

    return run_server(port, library_directory);
}
