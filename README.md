# Introducing the Music Streaming System: Server and Client Application

## Overview

This project entails developing a robust system of interconnected applications: a server and multiple clients communicating via sockets to stream and manage a music library. The server hosts a collection of audio files, allowing clients to stream or download these files efficiently. This system supports various media formats, including wav, mp3, ogg, and flac, and demonstrates the implementation of dynamic buffering, socket communication, and process management.

## Key Features

### Server Functionality

**Music Library Management:** The server scans and maintains a list of audio files in a specified directory, ensuring files are accessible for client requests.

**Client Handling:** The server listens for client connections, spawning child processes to handle individual client requests without disrupting other operations.

**Streaming and Listing:** Clients can request a list of available files or stream specific files. The server sends files in chunks, supporting real-time streaming.

### Client Functionality

**Request Handling:** Clients can request the list of available files, stream specific files, or download them. Commands include `list`, `get <index>`, `stream <index>`, and `stream+ <index>`.

**Dynamic Buffering:** Clients use a dynamic buffer to handle incoming data, ensuring efficient memory usage and uninterrupted streaming or downloading.

**Audio Playback Integration:** The client manages an audio player process (mpv) to play streamed files directly.

## Implementation Details

### Setup

1. **Clone Repository:** Start by cloning the provided repository containing the starter code.
2. **Compile Code:** Use the `make` command to compile the source files, generating the necessary executables for the server and client.
3. **Run Server:** Launch the server using `./as_server`, which will listen for client connections.
4. **Run Client:** Connect to the server using `./as_client` and use the provided commands to interact with the server.

### Server Architecture

**Passive Application Architecture:** The server sets up a socket for incoming connections and uses `select` to manage multiple clients. It handles client requests in child processes, streaming files in chunks and maintaining the music library.

### Client Architecture

**Active Application Architecture:** The client features a command shell to interact with the server. Commands allow listing files, streaming, downloading, and playing audio files. The client handles data reception and playback using dynamic buffers and the `select` system call.

### Dynamic Buffering

**Client-Side Buffering:** The client uses a fixed-size buffer to read from the socket and a dynamic buffer to manage data yet to be written to the audio player or file system. The buffer expands as needed, ensuring efficient memory usage and smooth data streaming.

## Task Breakdown

### Server Tasks

- **initialize_server_socket:** Sets up the server socket for listening to connections.
- **list_request_response:** Responds to client requests for listing available files.
- **stream_request_response:** Streams requested files to clients in chunks.

### Client Tasks

- **get_library_dir_permission:** Ensures the client's library directory exists with the correct permissions.
- **list_request:** Sends a list request to the server and processes the response.
- **start_audio_player_process:** Manages the audio player process for streaming files.
- **send_and_process_stream_request:** Handles streaming requests, managing data reception and playback.
