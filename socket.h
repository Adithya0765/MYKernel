// socket.h - BSD-style Socket API for Alteo OS
#ifndef SOCKET_H
#define SOCKET_H

#include "stdint.h"

// Address families
#define AF_INET     2       // IPv4

// Socket types
#define SOCK_STREAM 1       // TCP
#define SOCK_DGRAM  2       // UDP
#define SOCK_RAW    3       // Raw IP

// Protocol numbers
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

// Socket options
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_KEEPALIVE    9
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define SO_RCVBUF       8
#define SO_SNDBUF       7

// Shutdown flags
#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2

// Socket errors
#define SOCK_ERR_NONE       0
#define SOCK_ERR_INVAL      -1
#define SOCK_ERR_NOBUFS     -2
#define SOCK_ERR_CONNREFUSED -3
#define SOCK_ERR_TIMEOUT    -4
#define SOCK_ERR_NOTCONN    -5
#define SOCK_ERR_ALREADY    -6
#define SOCK_ERR_ADDRINUSE  -7
#define SOCK_ERR_WOULDBLOCK -8

// Maximum sockets
#define MAX_SOCKETS     16

// Socket address (IPv4)
typedef struct {
    uint16_t sin_family;    // Address family (AF_INET)
    uint16_t sin_port;      // Port number (network byte order)
    uint32_t sin_addr;      // IP address (network byte order)
    uint8_t  sin_zero[8];   // Padding
} sockaddr_in_t;

// Generic socket address
typedef struct {
    uint16_t sa_family;
    uint8_t  sa_data[14];
} sockaddr_t;

// Socket structure
typedef struct {
    int      active;        // Socket allocated
    int      type;          // SOCK_STREAM, SOCK_DGRAM, SOCK_RAW
    int      protocol;      // Protocol number
    int      family;        // Address family

    // Binding info
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    int      bound;         // Has been bound
    int      connected;     // Is connected
    int      listening;     // Is listening

    // TCP connection (for SOCK_STREAM)
    int      tcp_conn_id;   // TCP connection ID

    // Receive buffer (for SOCK_DGRAM)
    uint8_t  dgram_buf[2048];
    uint16_t dgram_len;
    uint32_t dgram_src_ip;
    uint16_t dgram_src_port;
    int      dgram_ready;

    // Options
    int      reuse_addr;
    uint32_t recv_timeout;  // Milliseconds
    uint32_t send_timeout;  // Milliseconds

    int      error;         // Last error
} socket_t;

// Initialize socket layer
void socket_init(void);

// Create a socket (returns socket descriptor or -1)
int  socket_create(int family, int type, int protocol);

// Bind socket to local address
int  socket_bind(int sockfd, const sockaddr_in_t* addr);

// Listen for connections (TCP)
int  socket_listen(int sockfd, int backlog);

// Accept a connection (TCP, returns new socket fd)
int  socket_accept(int sockfd, sockaddr_in_t* addr);

// Connect to remote address
int  socket_connect(int sockfd, const sockaddr_in_t* addr);

// Send data
int  socket_send(int sockfd, const void* data, uint32_t len, int flags);

// Receive data
int  socket_recv(int sockfd, void* buf, uint32_t len, int flags);

// Send to specific address (UDP)
int  socket_sendto(int sockfd, const void* data, uint32_t len,
                   int flags, const sockaddr_in_t* dest_addr);

// Receive from specific address (UDP)
int  socket_recvfrom(int sockfd, void* buf, uint32_t len,
                     int flags, sockaddr_in_t* src_addr);

// Close socket
int  socket_close(int sockfd);

// Shutdown socket
int  socket_shutdown(int sockfd, int how);

// Set socket option
int  socket_setsockopt(int sockfd, int level, int optname,
                       const void* optval, uint32_t optlen);

// Get socket option
int  socket_getsockopt(int sockfd, int level, int optname,
                       void* optval, uint32_t* optlen);

// Network helper - create a TCP client connection
int  net_tcp_connect(uint32_t ip, uint16_t port);

// Network helper - create a TCP server
int  net_tcp_listen(uint16_t port);

// Process network events (call from main loop)
void socket_poll(void);

#endif
