#include "common.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int Buffer::reserve(size_t min_capacity) {
    if (data.capacity() >= min_capacity) return 0;
    size_t new_cap = data.capacity() ? data.capacity() : static_cast<size_t>(1024);
    const size_t max_size = std::numeric_limits<size_t>::max();
    while (new_cap < min_capacity) {
        if (new_cap > max_size / 2) { new_cap = min_capacity; break; }
        new_cap *= 2;
    }
    try {
        data.reserve(new_cap);
        if (data.size() < length) data.resize(length);
        return 0;
    } catch (...) {
        return -1;
    }
}

int Buffer::append(const void *src, size_t len) {
    if (len == 0) return 0;
    if (reserve(length + len) != 0) return -1;
    const uint8_t *p = static_cast<const uint8_t *>(src);
    if (data.size() < length) data.resize(length);
    size_t old_size = data.size();
    data.resize(length + len);
    std::memcpy(data.data() + length, p, len);
    (void)old_size;
    length += len;
    return 0;
}

void Buffer::consume(size_t len) {
    if (len == 0) return;
    if (len >= length) {
        length = 0;
        return;
    }
    std::memmove(data.data(), data.data() + len, length - len);
    length -= len;
}

int set_socket_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    return 0;
}

int create_tcp_listener(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, SOMAXCONN) < 0) {
        ::close(fd);
        return -1;
    }
    if (set_socket_nonblocking(fd) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int create_udp_discovery_socket(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    if (set_socket_nonblocking(fd) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

ssize_t write_fully_nonblocking(int fd, const uint8_t *data, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::write(fd, data + total, len - total);
        if (n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        return -1;
    }
    return static_cast<ssize_t>(total);
}

ssize_t read_into_buffer_nonblocking(int fd, Buffer &buffer) {
    uint8_t temp[8192];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = ::read(fd, temp, sizeof(temp));
        if (n > 0) {
            if (buffer.append(temp, static_cast<size_t>(n)) != 0) return -1;
            total += n;
            continue;
        }
        if (n == 0) return total; // peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK) return total;
        return -1;
    }
}

int flush_buffered_writes(int fd, Buffer &outbuf) {
    if (outbuf.length == 0) return 0;
    ssize_t n = write_fully_nonblocking(fd, outbuf.begin(), outbuf.length);
    if (n < 0) return -1;
    outbuf.consume(static_cast<size_t>(n));
    return (outbuf.length == 0) ? 1 : 0; // 1 means fully flushed
}

int send_framed_or_buffer(int fd, Buffer &outbuf, const uint8_t *payload, uint32_t len) {
    if (len > MAX_MESSAGE_SIZE) return -1;
    uint32_t nlen = htonl(len);
    uint8_t hdr[4];
    std::memcpy(hdr, &nlen, 4);

    if (outbuf.length == 0) {
        ssize_t n = write_fully_nonblocking(fd, hdr, 4);
        if (n < 0) return -1;
        if (static_cast<size_t>(n) < 4) {
            if (outbuf.append(hdr + n, 4 - static_cast<size_t>(n)) != 0) return -1;
            if (outbuf.append(payload, len) != 0) return -1;
            return 0;
        }
        ssize_t m = write_fully_nonblocking(fd, payload, len);
        if (m < 0) return -1;
        if (static_cast<size_t>(m) < len) {
            if (outbuf.append(payload + m, len - static_cast<size_t>(m)) != 0) return -1;
        }
        return 0;
    } else {
        if (outbuf.append(hdr, 4) != 0) return -1;
        if (outbuf.append(payload, len) != 0) return -1;
        return 0;
    }
}

int has_complete_frame(const Buffer &inbuf, uint32_t *out_len) {
    if (inbuf.length < 4) return 0;
    uint32_t nlen;
    std::memcpy(&nlen, inbuf.begin(), 4);
    uint32_t len = ntohl(nlen);
    if (len > MAX_MESSAGE_SIZE) return -2;
    if (inbuf.length >= 4 + len) {
        if (out_len) *out_len = len;
        return 1;
    }
    return 0;
}

int get_frame_view(const Buffer &inbuf, const uint8_t **payload, uint32_t *len) {
    uint32_t l = 0;
    int r = has_complete_frame(inbuf, &l);
    if (r != 1) return r;
    if (payload) *payload = inbuf.begin() + 4;
    if (len) *len = l;
    return 1;
}


