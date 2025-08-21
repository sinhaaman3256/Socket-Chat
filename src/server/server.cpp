#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../common/common.hpp"

struct Client {
    int fd{ -1 };
    Buffer inbuf{};
    Buffer outbuf{};
    bool closed{ false };
    Client *next{ nullptr };
};

static volatile sig_atomic_t g_should_terminate = 0;

static void handle_sigint(int /*sig*/) {
    g_should_terminate = 1;
}

static void epoll_update_events(int epfd, int fd, uint32_t events, bool enable_out) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events | EPOLLET | EPOLLRDHUP;
    if (enable_out) ev.events |= EPOLLOUT;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        // Try add if mod fails (e.g., not present yet)
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
}

static void remove_client(int epfd, Client **list, Client *c) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, nullptr);
    close(c->fd);
    // Buffers free automatically
    // unlink
    Client **pp = list;
    while (*pp) {
        if (*pp == c) { *pp = c->next; break; }
        pp = &(*pp)->next;
    }
    delete c;
}

static void broadcast_to_others(int epfd, Client *clients, int sender_fd, const uint8_t *data, uint32_t len) {
    for (Client *c = clients; c != nullptr; c = c->next) {
        if (c->fd == sender_fd) continue;
        if (send_framed_or_buffer(c->fd, c->outbuf, data, len) < 0) {
            c->closed = true;
            continue;
        }
        if (c->outbuf.length > 0) {
            epoll_update_events(epfd, c->fd, EPOLLIN, true);
        }
    }
}

int main(int argc, char **argv) {
    uint16_t tcp_port = DEFAULT_TCP_PORT;
    uint16_t disc_port = DEFAULT_DISCOVERY_PORT;

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            tcp_port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--discover-port") == 0) && i + 1 < argc) {
            disc_port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: %s [-p PORT] [-d DISCOVERY_PORT]\n", argv[0]);
            return 0;
        }
    }

    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    int listen_fd = create_tcp_listener(tcp_port);
    if (listen_fd < 0) {
        std::perror("listen socket");
        return 1;
    }

    int udp_fd = create_udp_discovery_socket(disc_port);
    if (udp_fd < 0) {
        std::perror("udp discovery socket");
        close(listen_fd);
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        std::perror("epoll_create1");
        close(listen_fd);
        close(udp_fd);
        return 1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        std::perror("epoll add listen");
        return 1;
    }
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = udp_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, udp_fd, &ev) < 0) {
        std::perror("epoll add udp");
        return 1;
    }

    Client *clients = nullptr;

    std::printf("Server listening on TCP %u, discovery UDP %u\n", tcp_port, disc_port);

    const int MAX_EVENTS = 128;
    epoll_event events[128];

    while (!g_should_terminate) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t e = events[i].events;

            if (fd == listen_fd) {
                // accept all
                for (;;) {
                    sockaddr_in addr{};
                    socklen_t alen = sizeof(addr);
                    int cfd = accept(listen_fd, reinterpret_cast<sockaddr *>(&addr), &alen);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        std::perror("accept");
                        break;
                    }
                    set_socket_nonblocking(cfd);

                    Client *c = new (std::nothrow) Client();
                    if (!c) { close(cfd); continue; }
                    c->fd = cfd;
                    c->closed = false;
                    c->next = clients;
                    clients = c;

                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);

                    char ipstr[64];
                    inet_ntop(AF_INET, &addr.sin_addr, ipstr, sizeof(ipstr));
                    std::printf("Client connected: %s:%u (fd=%d)\n", ipstr, ntohs(addr.sin_port), cfd);
                }
            } else if (fd == udp_fd) {
                for (;;) {
                    uint8_t buf[512];
                    sockaddr_in src{};
                    socklen_t slen = sizeof(src);
                    ssize_t r = recvfrom(udp_fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&src), &slen);
                    if (r < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        std::perror("recvfrom");
                        break;
                    }
                    buf[r] = 0;
                    if (r >= static_cast<ssize_t>(std::strlen(DISCOVER_REQUEST)) && std::memcmp(buf, DISCOVER_REQUEST, std::strlen(DISCOVER_REQUEST)) == 0) {
                        char reply[128];
                        int m = std::snprintf(reply, sizeof(reply), "%s %u", DISCOVER_RESPONSE, static_cast<unsigned>(tcp_port));
                        sendto(udp_fd, reply, static_cast<size_t>(m), 0, reinterpret_cast<sockaddr *>(&src), slen);
                    }
                }
            } else {
                // find client by fd
                Client *c = clients;
                while (c && c->fd != fd) c = c->next;
                if (!c) {
                    // Might be stale
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    continue;
                }

                if (e & (EPOLLHUP | EPOLLRDHUP)) {
                    c->closed = true;
                }

                if (e & EPOLLIN) {
                    ssize_t r = read_into_buffer_nonblocking(fd, c->inbuf);
                    if (r < 0) {
                        c->closed = true;
                    } else if (r == 0) {
                        c->closed = true; // peer closed
                    } else {
                        for (;;) {
                            uint32_t mlen = 0;
                            int hr = has_complete_frame(c->inbuf, &mlen);
                            if (hr == -2) { c->closed = true; break; }
                            if (hr != 1) break;
                            const uint8_t *payload = nullptr;
                            get_frame_view(c->inbuf, &payload, &mlen);
                            broadcast_to_others(epfd, clients, c->fd, payload, mlen);
                            c->inbuf.consume(4 + mlen);
                        }
                    }
                }

                if ((e & EPOLLOUT) && c->outbuf.length > 0) {
                    int flushed = flush_buffered_writes(fd, c->outbuf);
                    if (flushed < 0) c->closed = true;
                    if (c->outbuf.length == 0) epoll_update_events(epfd, fd, EPOLLIN, false);
                }

                if (c->closed) {
                    std::printf("Client disconnected (fd=%d)\n", c->fd);
                    remove_client(epfd, &clients, c);
                }
            }
        }
    }

    // Cleanup
    for (Client *c = clients; c;) {
        Client *next = c->next;
        remove_client(epfd, &clients, c);
        c = next;
    }
    close(udp_fd);
    close(listen_fd);
    close(epfd);
    std::printf("Server terminated.\n");
    return 0;
}


