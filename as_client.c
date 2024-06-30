#include "as_client.h"


static int connect_to_server(int port, const char *hostname) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("connect_to_server");
        return -1;
    }

    struct sockaddr_in addr;

    // Allow sockets across machines.
    addr.sin_family = AF_INET;
    // The port the server will be listening on.
    // htons() converts the port number to network byte order.
    // This is the same as the byte order of the big-endian architecture.
    addr.sin_port = htons(port);
    // Clear this field; sin_zero is used for padding for the struct.
    memset(&(addr.sin_zero), 0, 8);

    // Lookup host IP address.
    struct hostent *hp = gethostbyname(hostname);
    if (hp == NULL) {
        ERR_PRINT("Unknown host: %s\n", hostname);
        return -1;
    }

    addr.sin_addr = *((struct in_addr *) hp->h_addr);

    // Request connection to server.
    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("connect");
        return -1;
    }

    return sockfd;
}


/*
** Helper for: list_request
** This function reads from the socket until it finds a network newline.
** This is processed as a list response for a single library file,
** of the form:
**                   <index>:<filename>\r\n
**
** returns index on success, -1 on error
** filename is a heap allocated string pointing to the parsed filename
*/
static int get_next_filename(int sockfd, char **filename) {
    static int bytes_in_buffer = 0;
    static char buf[RESPONSE_BUFFER_SIZE];

    while((*filename = find_network_newline(buf, &bytes_in_buffer)) == NULL) {
        int num = read(sockfd, buf + bytes_in_buffer,
                       RESPONSE_BUFFER_SIZE - bytes_in_buffer);
        if (num < 0) {
            perror("list_request");
            return -1;
        }
        bytes_in_buffer += num;
        if (bytes_in_buffer == RESPONSE_BUFFER_SIZE) {
            ERR_PRINT("Response buffer filled without finding file\n");
            ERR_PRINT("Bleeding data, this shouldn't happen, but not giving up\n");
            memmove(buf, buf + BUFFER_BLEED_OFF, RESPONSE_BUFFER_SIZE - BUFFER_BLEED_OFF);
        }
    }

    char *parse_ptr = strtok(*filename, ":");
    int index = strtol(parse_ptr, NULL, 10);
    parse_ptr = strtok(NULL, ":");
    // moves the filename to the start of the string (overwriting the index)
    memmove(*filename, parse_ptr, strlen(parse_ptr) + 1);

    return index;
}

/*
** Sends a list request to the server and prints the list of files in the
** library. Also parses the list of files and stores it in the list parameter.
**
** The list of files is stored as a dynamic array of strings. Each string is
** a path to a file in the file library. The indexes of the array correspond
** to the file indexes that can be used to request each file from the server.
**
** You may free and malloc or realloc the library->files array as preferred.
**
** returns the length of the new library on success, -1 on error
*/
int list_request(int sockfd, Library *library) {
    char request[] = REQUEST_LIST END_OF_MESSAGE_TOKEN;
    if (write_precisely(sockfd, request, strlen(request)) == -1) {
        return -1; // Error handling for failed write
    }

    int index;
    char *filename = NULL;
    while ((index = get_next_filename(sockfd, &filename)) != -1) {
        if (index >= library->num_files) {
            // Resize the files array to fit new index
            char **temp = realloc(library->files, (index + 1) * sizeof(char *));
            if (!temp) {
                perror("list_request: realloc failed");
                return -1;
            }
            library->files = temp;
            for (int i = library->num_files; i <= index; i++) {
                library->files[i] = NULL; // Initialize new slots to NULL
            }
            library->num_files = index + 1;
        }
        library->files[index] = filename; // filename is already heap-allocated

        if (index == 0) {
            break; // No more files
        }
    }

    if (library->num_files == 0) {
        // No files received
        return -1;
    }

    // Print filenames in ascending order
    for (int i = 0; i < library->num_files; i++) {
        if (library->files[i] != NULL) { // Check if the filename exists
            printf("%d: %s\n", i, library->files[i]);
        }
    }

    return library->num_files; // Return the count of files as success indicator
}

/*
** Get the permission of the library directory. If the library 
** directory does not exist, this function shall create it.
**
** library_dir: the path of the directory storing the audio files
** perpt:       an output parameter for storing the permission of the 
**              library directory.
**
** returns 0 on success, -1 on error
*/
static int get_library_dir_permission(const char *library_dir, mode_t * perpt) {
    struct stat st;

    // Check if the directory exists
    if (stat(library_dir, &st) == -1) {
        // If the directory does not exist, try to create it
        if (mkdir(library_dir, 0700) == -1) {
            perror("mkdir");
            return -1;
        }
        // After creation, check its status again to get the permissions
        if (stat(library_dir, &st) == -1) {
            perror("stat");
            return -1;
        }
    }

    // Check if the path is indeed a directory
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s is not a directory\n", library_dir);
        return -1;
    }

    // Extract the directory's permissions from st_mode
    *perpt = st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO); // Mask to extract permission bits

    return 0; // Success
}

/*
** Creates any directories needed within the library dir so that the file can be
** written to the correct destination. All directories will inherit the permissions
** of the library_dir.
**
** This function is recursive, and will create all directories needed to reach the
** file in destination.
**
** Destination shall be a path without a leading /
**
** library_dir can be an absolute or relative path, and can optionally end with a '/'
**
*/
static void create_missing_directories(const char *destination, const char *library_dir) {
    // get the permissions of the library dir
    mode_t permissions;
    if (get_library_dir_permission(library_dir, &permissions) == -1) {
        exit(1);
    }
    char *str_de_tokville = strdup(destination);
    if (str_de_tokville == NULL) {
        perror("create_missing_directories");
        return;
    }

    char *before_filename = strrchr(str_de_tokville, '/');
    if (!before_filename){
        goto free_tokville;
    }

    char *path = malloc(strlen(library_dir) + strlen(destination) + 2);
    if (path == NULL) {
        goto free_tokville;
    } *path = '\0';

    char *dir = strtok(str_de_tokville, "/");
    if (dir == NULL){
        goto free_path;
    }
    strcpy(path, library_dir);
    if (path[strlen(path) - 1] != '/') {
        strcat(path, "/");
    }
    strcat(path, dir);

    while (dir != NULL && dir != before_filename + 1) {
        #ifdef DEBUG
        printf("Creating directory %s\n", path);
        #endif
        if (mkdir(path, permissions) == -1) {
            if (errno != EEXIST) {
                perror("create_missing_directories");
                goto free_path;
            }
        }
        dir = strtok(NULL, "/");
        if (dir != NULL) {
            strcat(path, "/");
            strcat(path, dir);
        }
    }
free_path:
    free(path);
free_tokville:
    free(str_de_tokville);
}


/*
** Helper for: get_file_request
*/
static int file_index_to_fd(uint32_t file_index, const Library * library){
    create_missing_directories(library->files[file_index], library->path);

    char *filepath = _join_path(library->path, library->files[file_index]);
    if (filepath == NULL) {
        return -1;
    }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    #ifdef DEBUG
    printf("Opened file %s\n", filepath);
    #endif
    free(filepath);
    if (fd < 0 ) {
        perror("file_index_to_fd");
        return -1;
    }

    return fd;
}


int get_file_request(int sockfd, uint32_t file_index, const Library * library){
    #ifdef DEBUG
    printf("Getting file %s\n", library->files[file_index]);
    #endif

    int file_dest_fd = file_index_to_fd(file_index, library);
    if (file_dest_fd == -1) {
        return -1;
    }

    int result = send_and_process_stream_request(sockfd, file_index, -1, file_dest_fd);
    if (result == -1) {
        return -1;
    }

    return 0;
}

/*
** Starts the audio player process and returns the file descriptor of
** the write end of a pipe connected to the audio player's stdin.
**
** The audio player process is started with the AUDIO_PLAYER command and the
** AUDIO_PLAYER_ARGS arguments. The file descriptor to write the audio stream to
** is returned in the audio_out_fd parameter.
**
** returns PID of AUDIO_PLAYER (returns in parent process on success), -1 on error
** child process does not return.
*/
int start_audio_player_process(int *audio_out_fd) {
    int pipefd[2];
    
    // Create a pipe
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return -1;
    }
    
    // Fork the current process to create a parent and a child process
    pid_t pid = fork();
    if (pid == -1) {
        // Error handling for fork failure
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    } else if (pid == 0) {
        // Close the write end of the pipe, as it's not needed in the child process
        close(pipefd[1]);
        
        // Redirect the read end of the pipe to standard input (stdin)
        if (dup2(pipefd[0], STDIN_FILENO) == -1) {
            perror("dup2");
            exit(EXIT_FAILURE);
        }
        // After redirection, the original read end is no longer needed
        close(pipefd[0]);
        
        // Sleep to allow the audio player to boot up
        sleep(AUDIO_PLAYER_BOOT_DELAY);
        
        // Replace the child process with the audio player process
        char *args[] = AUDIO_PLAYER_ARGS;
        if (execvp(AUDIO_PLAYER, args) == -1) {
            perror("execvp");
            exit(EXIT_FAILURE);
        }
        // execvp does not return unless there is an error
    } else {
        // Close the read end of the pipe, as it's not needed in the parent process
        close(pipefd[0]);
        
        // Return the write end of the pipe through the audio_out_fd parameter
        *audio_out_fd = pipefd[1];
        
        // Return the PID of the child process (audio player)
        return pid;
    }
    // This line should never be reached
    return -1;
}

static void _wait_on_audio_player(int audio_player_pid) {
    int status;
    if (waitpid(audio_player_pid, &status, 0) == -1) {
        perror("_wait_on_audio_player");
        return;
    }
    if (WIFEXITED(status)) {
        fprintf(stderr, "Audio player exited with status %d\n", WEXITSTATUS(status));
    } else {
        printf("Audio player exited abnormally\n");
    }
}


int stream_request(int sockfd, uint32_t file_index) {
    int audio_out_fd;
    int audio_player_pid = start_audio_player_process(&audio_out_fd);

    int result = send_and_process_stream_request(sockfd, file_index, audio_out_fd, -1);
    if (result == -1) {
        ERR_PRINT("stream_request: send_and_process_stream_request failed\n");
        return -1;
    }

    _wait_on_audio_player(audio_player_pid);

    return 0;
}


int stream_and_get_request(int sockfd, uint32_t file_index, const Library * library) {
    int audio_out_fd;
    int audio_player_pid = start_audio_player_process(&audio_out_fd);

    #ifdef DEBUG
    printf("Getting file %s\n", library->files[file_index]);
    #endif

    int file_dest_fd = file_index_to_fd(file_index, library);
    if (file_dest_fd == -1) {
        ERR_PRINT("stream_and_get_request: file_index_to_fd failed\n");
        return -1;
    }

    int result = send_and_process_stream_request(sockfd, file_index,
                                                 audio_out_fd, file_dest_fd);
    if (result == -1) {
        ERR_PRINT("stream_and_get_request: send_and_process_stream_request failed\n");
        return -1;
    }
    printf("waiting on audio player\n");
    _wait_on_audio_player(audio_player_pid);
    printf("audio player done\n");
    return 0;
}


/*
** Sends a stream request to the server and sends the audio
** Input: sockfd - the socket file descriptor, file_index - the index of the file to stream
** returns 0 on success, -1 on error
*/ 
int sendStreamRequest(int sockfd, uint32_t file_index) {
    char stream_command[] = "STREAM\r\n";
    uint32_t network_order_index = htonl(file_index);

    // Write the STREAM command
    if (write(sockfd, stream_command, sizeof(stream_command) - 1) < 0) {
        perror("Failed to send STREAM command");
        return -1;
    }

    // Write the file index
    if (write(sockfd, &network_order_index, sizeof(network_order_index)) < 0) {
        perror("Failed to send file index");
        return -1;
    }

    return 0; // Success
}

/*
** Sends a stream request for the particular file_index to the server and sends the audio
** stream to the audio_out_fd and file_dest_fd file descriptors
** -- provided that they are not < 0.
**
** The select system call should be used to simultaneously wait for data to be available
** to read from the server connection/socket, as well as for when audio_out_fd and file_dest_fd
** (if applicable) are ready to be written to. Differing numbers of bytes may be written to
** at each time (do no use write_precisely for this purpose -- you will nor receive full marks)
** audio_out_fd and file_dest_fd, and this must be handled.
**
** One of audio_out_fd or file_dest_fd can be -1, but not both. File descriptors >= 0
** should be closed before the function returns.
**
** This function will leverage a dynamic circular buffer with two output streams
** and one input stream. The input stream is the server connection/socket, and the output
** streams are audio_out_fd and file_dest_fd. The buffer should be dynamically sized using
** realloc. See the assignment handout for more information, and notice how realloc is used
** to manage the library.files in this client and the server.
**
** Phrased differently, this uses a FIFO with two independent out streams and one in stream,
** but because it is as long as needed, we call it circular, and just point to three different
** parts of it.
**
** returns 0 on success, -1 on error
*/
int send_and_process_stream_request(int sockfd, uint32_t file_index, int audio_out_fd, int file_dest_fd) {
    if (audio_out_fd == -1 && file_dest_fd == -1) {
        ERR_PRINT("Both audio_out_fd and file_dest_fd are -1\n");
        return -1;
    }
 
    // Send stream request to server
    if (sendStreamRequest(sockfd, file_index) < 0) {
        ERR_PRINT("Failed to send stream request\n");
        if (audio_out_fd != -1) close(audio_out_fd);
        if (file_dest_fd != -1) close(file_dest_fd);
        return -1;
    }
 
    // Read file size from server
    uint32_t file_size;
    if (read_precisely(sockfd, &file_size, sizeof(file_size)) != sizeof(file_size)) {
        ERR_PRINT("Failed to read file size from server\n");
        if (audio_out_fd != -1) close(audio_out_fd);
        if (file_dest_fd != -1) close(file_dest_fd);
        return -1;
    }
    file_size = ntohl(file_size); // Ensure network byte order is converted to host byte order
 
    fd_set read_fds, write_fds;
    struct timeval tv;
    char fixed_buffer[NETWORK_PRE_DYNAMIC_BUFF_SIZE];
    char *buffer = NULL;
    char *temp;

    int buffer_size = 0, total_bytes_read = 0, total_bytes_audio = 0, total_bytes_file = 0, audio_offset = 0, file_offset = 0;
    int nread, nwritten, awritten;
    int min_bytes_written;

    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_SET(sockfd, &read_fds);
    if (audio_out_fd != -1) FD_SET(audio_out_fd, &write_fds);
    if (file_dest_fd != -1) FD_SET(file_dest_fd, &write_fds);
 
    while (total_bytes_read < file_size || (audio_out_fd != -1 && total_bytes_audio < file_size) || (file_dest_fd != -1 && total_bytes_file < file_size)) {
        
        fd_set r_fds = read_fds;
        fd_set w_fds = write_fds;
        
        tv.tv_sec = SELECT_TIMEOUT_SEC;
        tv.tv_usec = SELECT_TIMEOUT_USEC;
 
        int max_fd = sockfd > file_dest_fd ? sockfd : file_dest_fd;
        max_fd = max_fd > audio_out_fd ? max_fd : audio_out_fd;
        if (select(max_fd + 1, &r_fds, &w_fds, NULL, &tv) == -1) {
            perror("select failed");
            break;
        }
 
        // Reading from server
        if (FD_ISSET(sockfd, &r_fds) && total_bytes_read < file_size) {
            nread = read(sockfd, fixed_buffer, sizeof(fixed_buffer));
            if (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("read");
                if (buffer != NULL) free(buffer);
                if (audio_out_fd != -1) close(audio_out_fd);
                if (file_dest_fd != -1) close(file_dest_fd);
                return -1;
            }

            temp = realloc(buffer, buffer_size + nread);
            if (temp == NULL) {
                perror("realloc");
                if (buffer != NULL) free(buffer);
                if (audio_out_fd != -1) close(audio_out_fd);
                if (file_dest_fd != -1) close(file_dest_fd);
                return -1;
            } else {
                buffer = temp;
            }

            memcpy(buffer + buffer_size, fixed_buffer, nread);

            buffer_size += nread;
            total_bytes_read += nread;
        }
  
        awritten = 0;
        nwritten = 0;

        // Writing to audio output
        if (audio_out_fd != -1){
            if (FD_ISSET(audio_out_fd, &w_fds)) {
                awritten = write(audio_out_fd, buffer + audio_offset, buffer_size - audio_offset);
                if (awritten < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write");
                    if (buffer != NULL) free(buffer);
                    if (audio_out_fd != -1) close(audio_out_fd);
                    if (file_dest_fd != -1) close(file_dest_fd);
                    return -1;
                }
                audio_offset += awritten;
            }
        } 
 
        // Writing to file descriptor
        if (file_dest_fd != -1) {
            if (FD_ISSET(file_dest_fd, &w_fds)){
                nwritten = write(file_dest_fd, buffer + file_offset, buffer_size - file_offset);
                if (nwritten < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("write");
                    if (buffer != NULL) free(buffer);
                    if (audio_out_fd != -1) close(audio_out_fd);
                    if (file_dest_fd != -1) close(file_dest_fd);
                    return -1;
                }
                file_offset += nwritten;
            }
        } 

        min_bytes_written = 0;

        if (audio_out_fd != -1 && file_dest_fd != -1) {
            min_bytes_written = awritten < nwritten ? awritten : nwritten;
            total_bytes_audio += awritten;
            total_bytes_file += nwritten;
            
            audio_offset -= min_bytes_written;
            file_offset -= min_bytes_written;
        } else if (audio_out_fd != -1) {
            min_bytes_written = awritten;

            total_bytes_audio += awritten;
            audio_offset -= min_bytes_written;
        } else {
            min_bytes_written = nwritten;

            total_bytes_file += nwritten;
            file_offset -= min_bytes_written;
        }

        // Adjust buffer and its counters if any bytes have been written
        if (min_bytes_written > 0) {
            // Calculate the new size of the dynamic buffer
            buffer_size -= min_bytes_written;
            memmove(buffer, buffer + min_bytes_written, buffer_size); // Shift the unwritten part of the buffer to the beginning  
        }
        printf("Total bytes read: %d out of %d\n", total_bytes_read, file_size);
        printf("Total bytes written to audio: %d out of %d read\n", total_bytes_audio, total_bytes_read);
        printf("Total bytes written to file: %d out of %d read\n", total_bytes_file, total_bytes_read);
    }
 
    if (buffer != NULL) free(buffer);
    if (audio_out_fd != -1) close(audio_out_fd);
    if (file_dest_fd != -1) close(file_dest_fd);
 
    return 0; // Success
}


static void _print_shell_help(){
    printf("Commands:\n");
    printf("  list: List the files in the library\n");
    printf("  get <file_index>: Get a file from the library\n");
    printf("  stream <file_index>: Stream a file from the library (without saving it)\n");
    printf("  stream+ <file_index>: Stream a file from the library\n");
    printf("                        and save it to the local library\n");
    printf("  help: Display this help message\n");
    printf("  quit: Quit the client\n");
}


/*
** Shell to handle the client options
** ----------------------------------
** This function is a mini shell to handle the client options. It prompts the
** user for a command and then calls the appropriate function to handle the
** command. The user can enter the following commands:
** - "list" to list the files in the library
** - "get <file_index>" to get a file from the library
** - "stream <file_index>" to stream a file from the library (without saving it)
** - "stream+ <file_index>" to stream a file from the library and save it to the local library
** - "help" to display the help message
** - "quit" to quit the client
*/
static int client_shell(int sockfd, const char *library_directory) {
    char buffer[REQUEST_BUFFER_SIZE];
    char *command;
    int file_index;

    Library library = {"client", library_directory, NULL, 0};

    while (1) {
        if (library.files == 0) {
            printf("Server library is empty or not retrieved yet\n");
        }

        printf("Enter a command: ");
        if (fgets(buffer, REQUEST_BUFFER_SIZE, stdin) == NULL) {
            perror("client_shell");
            goto error;
        }

        command = strtok(buffer, " \n");
        if (command == NULL) {
            continue;
        }

        // List Request -- list the files in the library
        if (strcmp(command, CMD_LIST) == 0) {
            if (list_request(sockfd, &library) == -1) {
                goto error;
            }

        // Get Request -- get a file from the library
        } else if (strcmp(command, CMD_GET) == 0) {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL) {
                printf("Usage: get <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files) {
                printf("Invalid file index\n");
                continue;
            }

            if (get_file_request(sockfd, file_index, &library) == -1) {
                goto error;
            }

        // Stream Request -- stream a file from the library (without saving it)
        } else if (strcmp(command, CMD_STREAM) == 0) {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL) {
                printf("Usage: stream <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            printf("file_index: %d\n", file_index);
            if (file_index < 0 || file_index >= library.num_files) {
                printf("Invalid file index\n");
                continue;
            }

            if (stream_request(sockfd, file_index) == -1) {
                goto error;
            }
            printf("Streamed file %s\n", library.files[file_index]);

        // Stream and Get Request -- stream a file from the library and save it to the local library
        } else if (strcmp(command, CMD_STREAM_AND_GET) == 0) {
            char *file_index_str = strtok(NULL, " \n");
            if (file_index_str == NULL) {
                printf("Usage: stream+ <file_index>\n");
                continue;
            }
            file_index = strtol(file_index_str, NULL, 10);
            if (file_index < 0 || file_index >= library.num_files) {
                printf("Invalid file index\n");
                continue;
            }

            if (stream_and_get_request(sockfd, file_index, &library) == -1) {
                goto error;
            }

        } else if (strcmp(command, CMD_HELP) == 0) {
            _print_shell_help();

        } else if (strcmp(command, CMD_QUIT) == 0) {
            printf("Quitting shell\n");
            break;

        } else {
            printf("Invalid command\n");
        }
    }

    _free_library(&library);
    return 0;
error:
    _free_library(&library);
    return -1;
}


static void print_usage() {
    printf("Usage: as_client [-h] [-a NETWORK_ADDRESS] [-p PORT] [-l LIBRARY_DIRECTORY]\n");
    printf("  -h: Print this help message\n");
    printf("  -a NETWORK_ADDRESS: Connect to server at NETWORK_ADDRESS (default 'localhost')\n");
    printf("  -p  Port to listen on (default: " XSTR(DEFAULT_PORT) ")\n");
    printf("  -l LIBRARY_DIRECTORY: Use LIBRARY_DIRECTORY as the library directory (default 'as-library')\n");
}


int main(int argc, char * const *argv) {
    int opt;
    int port = DEFAULT_PORT;
    const char *hostname = "localhost";
    const char *library_directory = "saved";

    while ((opt = getopt(argc, argv, "ha:p:l:")) != -1) {
        switch (opt) {
            case 'h':
                print_usage();
                return 0;
            case 'a':
                hostname = optarg;
                break;
            case 'p':
                port = strtol(optarg, NULL, 10);
                if (port < 0 || port > 65535) {
                    ERR_PRINT("Invalid port number %d\n", port);
                    return 1;
                }
                break;
            case 'l':
                library_directory = optarg;
                break;
            default:
                print_usage();
                return 1;
        }
    }

    printf("Connecting to server at %s:%d, using library in %s\n",
           hostname, port, library_directory);

    int sockfd = connect_to_server(port, hostname);
    if (sockfd == -1) {
        return -1;
    }

    int result = client_shell(sockfd, library_directory);
    if (result == -1) {
        close(sockfd);
        return -1;
    }

    close(sockfd);
    return 0;
}
