
// socket.c - BSD-style Socket API for Alteo OS
#include "socket.h"
#include "tcp.h"
#include "ip.h"
#include "ethernet.h"
#include "e1000.h"

static socket_t sockets[MAX_SOCKETS];

// Memory helpers
static void sock_memset(void* dst, uint8_t val, int n) {
    uint8_t* d = (uint8_t*)dst;
    for (int i = 0; i < n; i++) d[i] = val;
}

void socket_init(void) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        sock_memset(&sockets[i], 0, sizeof(socket_t));
        sockets[i].active = 0;
        sockets[i].tcp_conn_id = -1;
    }
}

int socket_create(int family, int type, int protocol) {
    if (family != AF_INET) return SOCK_ERR_INVAL;
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
        return SOCK_ERR_INVAL;

    // Find free socket
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active) {
            sock_memset(&sockets[i], 0, sizeof(socket_t));
            sockets[i].active = 1;
            sockets[i].family = family;
            sockets[i].type = type;
            sockets[i].protocol = protocol ? protocol :
                (type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
            sockets[i].tcp_conn_id = -1;
            return i;
        }
    }
    return SOCK_ERR_NOBUFS;
}

int socket_bind(int sockfd, const sockaddr_in_t* addr) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active) return SOCK_ERR_INVAL;

    // Check for port conflicts
    if (addr->sin_port != 0) {
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (i == sockfd || !sockets[i].active) continue;
            if (sockets[i].local_port == ntohs(addr->sin_port) &&
                sockets[i].type == s->type && !s->reuse_addr)
                return SOCK_ERR_ADDRINUSE;
        }
    }

    s->local_ip = ntohl(addr->sin_addr);
    s->local_port = ntohs(addr->sin_port);
    s->bound = 1;
    return SOCK_ERR_NONE;
}

int socket_listen(int sockfd, int backlog) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active || s->type != SOCK_STREAM) return SOCK_ERR_INVAL;
    if (!s->bound) return SOCK_ERR_INVAL;

    (void)backlog;

    // Create TCP listener
    s->tcp_conn_id = tcp_listen(s->local_port);
    if (s->tcp_conn_id < 0) return SOCK_ERR_NOBUFS;

    s->listening = 1;
    return SOCK_ERR_NONE;
}

int socket_accept(int sockfd, sockaddr_in_t* addr) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active || !s->listening) return SOCK_ERR_INVAL;

    int tcp_id = tcp_accept(s->tcp_conn_id);
    if (tcp_id < 0) return SOCK_ERR_WOULDBLOCK;

    // Create new socket for accepted connection
    int new_fd = socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (new_fd < 0) return new_fd;

    socket_t* ns = &sockets[new_fd];
    ns->tcp_conn_id = tcp_id;
    ns->connected = 1;
    ns->bound = 1;
    ns->local_port = s->local_port;
    ns->local_ip = s->local_ip;

    if (addr) {
        addr->sin_family = AF_INET;
        addr->sin_port = 0; // Could fill from TCP connection
        addr->sin_addr = 0;
    }

    return new_fd;
}

int socket_connect(int sockfd, const sockaddr_in_t* addr) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active) return SOCK_ERR_INVAL;
    if (s->connected) return SOCK_ERR_ALREADY;

    s->remote_ip = ntohl(addr->sin_addr);
    s->remote_port = ntohs(addr->sin_port);

    if (s->type == SOCK_STREAM) {
        s->tcp_conn_id = tcp_connect(s->remote_ip, s->remote_port);
        if (s->tcp_conn_id < 0) return SOCK_ERR_CONNREFUSED;

        // Wait for connection (simplified - non-blocking in practice)
        for (int i = 0; i < 100000; i++) {
            if (tcp_get_state(s->tcp_conn_id) == TCP_STATE_ESTABLISHED) {
                s->connected = 1;
                return SOCK_ERR_NONE;
            }
            __asm__ __volatile__("pause");
        }
        return SOCK_ERR_TIMEOUT;
    } else {
        // UDP - just record the destination
        s->connected = 1;
        return SOCK_ERR_NONE;
    }
}

int socket_send(int sockfd, const void* data, uint32_t len, int flags) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active || !s->connected) return SOCK_ERR_NOTCONN;

    (void)flags;

    if (s->type == SOCK_STREAM) {
        return tcp_send(s->tcp_conn_id, (const uint8_t*)data, (uint16_t)len);
    }
    // UDP send would go here
    return SOCK_ERR_INVAL;
}

int socket_recv(int sockfd, void* buf, uint32_t len, int flags) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active) return SOCK_ERR_INVAL;

    (void)flags;

    if (s->type == SOCK_STREAM) {
        if (s->tcp_conn_id < 0) return SOCK_ERR_NOTCONN;
        return tcp_recv(s->tcp_conn_id, (uint8_t*)buf, (uint16_t)len);
    }
    // UDP recv would go here
    return SOCK_ERR_INVAL;
}

int socket_sendto(int sockfd, const void* data, uint32_t len,
                  int flags, const sockaddr_in_t* dest_addr) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active) return SOCK_ERR_INVAL;

    (void)flags;
    (void)dest_addr;

    if (s->type == SOCK_DGRAM) {
        // UDP implementation placeholder
        uint32_t dest_ip = ntohl(dest_addr->sin_addr);
        (void)dest_ip;
        (void)data;
        (void)len;
        return SOCK_ERR_INVAL; // Not yet implemented
    }

    return socket_send(sockfd, data, len, flags);
}

int socket_recvfrom(int sockfd, void* buf, uint32_t len,
                    int flags, sockaddr_in_t* src_addr) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active) return SOCK_ERR_INVAL;

    (void)flags;

    if (s->type == SOCK_DGRAM) {
        if (!s->dgram_ready) return SOCK_ERR_WOULDBLOCK;

        uint32_t copy_len = s->dgram_len < len ? s->dgram_len : len;
        uint8_t* dst = (uint8_t*)buf;
        for (uint32_t i = 0; i < copy_len; i++)
            dst[i] = s->dgram_buf[i];

        if (src_addr) {
            src_addr->sin_family = AF_INET;
            src_addr->sin_addr = htonl(s->dgram_src_ip);
            src_addr->sin_port = htons(s->dgram_src_port);
        }

        s->dgram_ready = 0;
        return (int)copy_len;
    }

    return socket_recv(sockfd, buf, len, flags);
}

int socket_close(int sockfd) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active) return SOCK_ERR_INVAL;

    if (s->type == SOCK_STREAM && s->tcp_conn_id >= 0) {
        tcp_close(s->tcp_conn_id);
    }

    sock_memset(s, 0, sizeof(socket_t));
    s->tcp_conn_id = -1;
    return SOCK_ERR_NONE;
}

int socket_shutdown(int sockfd, int how) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active) return SOCK_ERR_INVAL;

    (void)how;

    if (s->type == SOCK_STREAM && s->tcp_conn_id >= 0) {
        tcp_close(s->tcp_conn_id);
    }
    return SOCK_ERR_NONE;
}

int socket_setsockopt(int sockfd, int level, int optname,
                      const void* optval, uint32_t optlen) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active) return SOCK_ERR_INVAL;

    (void)optlen;

    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:
                s->reuse_addr = *(const int*)optval;
                return SOCK_ERR_NONE;
            case SO_RCVTIMEO:
                s->recv_timeout = *(const uint32_t*)optval;
                return SOCK_ERR_NONE;
            case SO_SNDTIMEO:
                s->send_timeout = *(const uint32_t*)optval;
                return SOCK_ERR_NONE;
            default:
                return SOCK_ERR_INVAL;
        }
    }
    return SOCK_ERR_INVAL;
}

int socket_getsockopt(int sockfd, int level, int optname,
                      void* optval, uint32_t* optlen) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS) return SOCK_ERR_INVAL;
    socket_t* s = &sockets[sockfd];
    if (!s->active) return SOCK_ERR_INVAL;

    if (level == SOL_SOCKET) {
        switch (optname) {
            case SO_REUSEADDR:
                *(int*)optval = s->reuse_addr;
                if (optlen) *optlen = sizeof(int);
                return SOCK_ERR_NONE;
            default:
                return SOCK_ERR_INVAL;
        }
    }
    return SOCK_ERR_INVAL;
}

int net_tcp_connect(uint32_t ip, uint16_t port) {
    int fd = socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return fd;

    sockaddr_in_t addr;
    addr.sin_family = AF_INET;
    addr.sin_addr = htonl(ip);
    addr.sin_port = htons(port);

    int ret = socket_connect(fd, &addr);
    if (ret != SOCK_ERR_NONE) {
        socket_close(fd);
        return ret;
    }
    return fd;
}

int net_tcp_listen(uint16_t port) {
    int fd = socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return fd;

    sockaddr_in_t addr;
    addr.sin_family = AF_INET;
    addr.sin_addr = 0;
    addr.sin_port = htons(port);

    int ret = socket_bind(fd, &addr);
    if (ret != SOCK_ERR_NONE) { socket_close(fd); return ret; }

    ret = socket_listen(fd, 5);
    if (ret != SOCK_ERR_NONE) { socket_close(fd); return ret; }

    return fd;
}

void socket_poll(void) {
    // Poll NIC for incoming packets
    if (e1000_is_available()) {
        uint8_t pkt_buf[2048];
        int len = e1000_receive(pkt_buf, sizeof(pkt_buf));
        if (len > 0) {
            eth_receive(pkt_buf, (uint16_t)len);
        }
    }

    // Run TCP timer
    tcp_timer();
}
