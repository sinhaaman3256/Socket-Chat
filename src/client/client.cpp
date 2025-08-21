#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../common/common.hpp"

static int enable_broadcast(int fd) {
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
}

static bool discover_server(uint16_t disc_port, std::string &out_ip, uint16_t &out_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::perror("socket UDP"); return false; }
    enable_broadcast(fd);

    // Set receive timeout
    timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in baddr{}; baddr.sin_family = AF_INET; baddr.sin_port = htons(disc_port);
    baddr.sin_addr.s_addr = inet_addr("255.255.255.255");

    const char *msg = DISCOVER_REQUEST;
    if (sendto(fd, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr*>(&baddr), sizeof(baddr)) < 0) {
        std::perror("sendto");
        close(fd);
        return false;
    }

    uint8_t buf[256];
    sockaddr_in src{}; socklen_t slen = sizeof(src);
    ssize_t r = recvfrom(fd, buf, sizeof(buf)-1, 0, reinterpret_cast<sockaddr*>(&src), &slen);
    if (r < 0) {
        std::perror("recvfrom");
        close(fd);
        return false;
    }
    buf[r] = 0;
    // Expect: "CHAT_HERE <port>"
    if (r >= static_cast<ssize_t>(std::strlen(DISCOVER_RESPONSE)) && std::memcmp(buf, DISCOVER_RESPONSE, std::strlen(DISCOVER_RESPONSE)) == 0) {
        unsigned p = 0;
        std::sscanf(reinterpret_cast<char*>(buf) + std::strlen(DISCOVER_RESPONSE), "%u", &p);
        char ipstr[64];
        inet_ntop(AF_INET, &src.sin_addr, ipstr, sizeof(ipstr));
        out_ip = ipstr;
        out_port = static_cast<uint16_t>(p);
        close(fd);
        return true;
    }
    close(fd);
    return false;
}

static void print_usage(const char *prog) {
    std::printf("Usage: %s [--host IP] [--port PORT] [--discover-port UDP_PORT]\n", prog);
    std::printf("If --host is omitted, UDP discovery is used.\n");
}

int main(int argc, char **argv) {
    std::string host;
    uint16_t tcp_port = 0; // 0 means unknown yet
    uint16_t disc_port = DEFAULT_DISCOVERY_PORT;

    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--host") == 0 || std::strcmp(argv[i], "-h") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if ((std::strcmp(argv[i], "--port") == 0 || std::strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            tcp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if ((std::strcmp(argv[i], "--discover-port") == 0 || std::strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            disc_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (host.empty()) {
        std::string ip;
        uint16_t port = 0;
        if (!discover_server(disc_port, ip, port)) {
            std::fprintf(stderr, "Discovery failed. Provide --host and --port.\n");
            return 1;
        }
        host = ip;
        tcp_port = port;
        std::printf("Discovered server %s:%u\n", host.c_str(), tcp_port);
    } else if (tcp_port == 0) {
        tcp_port = DEFAULT_TCP_PORT;
    }

    // Connect TCP
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { std::perror("socket"); return 1; }
    set_socket_nonblocking(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tcp_port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "Invalid host IP: %s\n", host.c_str());
        close(fd);
        return 1;
    }

    int cr = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (cr < 0 && errno != EINPROGRESS) {
        std::perror("connect");
        close(fd);
        return 1;
    }

    // Non-blocking stdin as well
    set_socket_nonblocking(STDIN_FILENO);

    Buffer inbuf;
    Buffer outbuf;
    std::vector<uint8_t> stdin_buf; // accumulate line input
    stdin_buf.reserve(4096);

    std::printf("Connected. Type messages and press Enter to send. Ctrl+C to quit.\n");

    for (;;) {
        pollfd fds[2];
        fds[0].fd = fd; fds[0].events = POLLIN | (outbuf.length > 0 ? POLLOUT : 0); fds[0].revents = 0;
        fds[1].fd = STDIN_FILENO; fds[1].events = POLLIN; fds[1].revents = 0;

        int pn = ::poll(fds, 2, 500);
        if (pn < 0) {
            if (errno == EINTR) continue;
            std::perror("poll");
            break;
        }

        // Socket readable/writable
        if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            std::fprintf(stderr, "Connection closed.\n");
            break;
        }
        if (fds[0].revents & POLLIN) {
            ssize_t r = read_into_buffer_nonblocking(fd, inbuf);
            if (r <= 0) { std::fprintf(stderr, "Disconnected.\n"); break; }
            for (;;) {
                uint32_t mlen = 0;
                int hr = has_complete_frame(inbuf, &mlen);
                if (hr == -2) { std::fprintf(stderr, "Protocol error.\n"); goto done; }
                if (hr != 1) break;
                const uint8_t *payload = nullptr; get_frame_view(inbuf, &payload, &mlen);
                std::fwrite(payload, 1, mlen, stdout);
                std::fputc('\n', stdout);
                std::fflush(stdout);
                inbuf.consume(4 + mlen);
            }
        }
        if ((fds[0].revents & POLLOUT) && outbuf.length > 0) {
            int flushed = flush_buffered_writes(fd, outbuf);
            if (flushed < 0) { std::fprintf(stderr, "Write error.\n"); break; }
        }

        // Stdin readable
        if (fds[1].revents & POLLIN) {
            uint8_t tmp[1024];
            ssize_t r = ::read(STDIN_FILENO, tmp, sizeof(tmp));
            if (r > 0) {
                stdin_buf.insert(stdin_buf.end(), tmp, tmp + r);
                // Split on newlines
                size_t start = 0;
                for (size_t i = 0; i < stdin_buf.size(); ++i) {
                    if (stdin_buf[i] == '\n') {
                        size_t len = i - start;
                        if (len > 0 && stdin_buf[i-1] == '\r') len -= 1; // trim CR
                        if (len > 0) {
                            if (send_framed_or_buffer(fd, outbuf, stdin_buf.data() + start, static_cast<uint32_t>(len)) < 0) {
                                std::fprintf(stderr, "Send failed.\n");
                                goto done;
                            }
                        }
                        start = i + 1;
                    }
                }
                // remove consumed
                if (start > 0) {
                    stdin_buf.erase(stdin_buf.begin(), stdin_buf.begin() + static_cast<long>(start));
                }
            } else if (r == 0) {
                // stdin closed
                goto done;
            }
        }
    }

done:
    close(fd);
    return 0;
}


