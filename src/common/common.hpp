// Common utilities and definitions for the chat project (C++)
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <sys/types.h> // for ssize_t

// Default ports
#define DEFAULT_TCP_PORT 5050
#define DEFAULT_DISCOVERY_PORT 55555

// Limits
#define MAX_CLIENTS 1024
#define MAX_MESSAGE_SIZE 4096

// UDP discovery protocol
#define DISCOVER_REQUEST "CHAT_DISCOVER?"
#define DISCOVER_RESPONSE "CHAT_HERE"

// Simple dynamically growing byte buffer
struct Buffer {
    std::vector<uint8_t> data;
    size_t length{0};

    // Ensures capacity >= min_capacity
    int reserve(size_t min_capacity);
    // Appends len bytes from src
    int append(const void *src, size_t len);
    // Consumes len bytes from the front
    void consume(size_t len);
    // Returns raw pointer to start
    uint8_t *begin() { return data.data(); }
    const uint8_t *begin() const { return data.data(); }
};

// Socket helpers
int set_socket_nonblocking(int fd);
int create_tcp_listener(uint16_t port);
int create_udp_discovery_socket(uint16_t port);

// I/O helpers
ssize_t write_fully_nonblocking(int fd, const uint8_t *data, size_t len);
ssize_t read_into_buffer_nonblocking(int fd, Buffer &buffer);
int flush_buffered_writes(int fd, Buffer &outbuf);

// Message framing (uint32 length prefix, network byte order)
int send_framed_or_buffer(int fd, Buffer &outbuf, const uint8_t *payload, uint32_t len);
int has_complete_frame(const Buffer &inbuf, uint32_t *out_len);
int get_frame_view(const Buffer &inbuf, const uint8_t **payload, uint32_t *len);


