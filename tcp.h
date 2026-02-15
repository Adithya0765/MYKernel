// tcp.h - TCP Transport Layer for Alteo OS
#ifndef TCP_H
#define TCP_H

#include "stdint.h"

// TCP flags
#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10
#define TCP_URG     0x20

// TCP states (RFC 793)
#define TCP_STATE_CLOSED        0
#define TCP_STATE_LISTEN        1
#define TCP_STATE_SYN_SENT      2
#define TCP_STATE_SYN_RECEIVED  3
#define TCP_STATE_ESTABLISHED   4
#define TCP_STATE_FIN_WAIT_1    5
#define TCP_STATE_FIN_WAIT_2    6
#define TCP_STATE_CLOSE_WAIT    7
#define TCP_STATE_CLOSING       8
#define TCP_STATE_LAST_ACK      9
#define TCP_STATE_TIME_WAIT     10

// TCP constants
#define TCP_HEADER_LEN      20      // Minimum header size
#define TCP_WINDOW_SIZE     8192    // Default window size
#define TCP_MAX_SEGMENT     1460    // MSS for Ethernet
#define TCP_MAX_CONNECTIONS 16      // Max simultaneous connections
#define TCP_SEND_BUF_SIZE  4096    // Send buffer per connection
#define TCP_RECV_BUF_SIZE  4096    // Receive buffer per connection
#define TCP_RETRANSMIT_MS  1000    // Retransmit timeout
#define TCP_MAX_RETRIES    5       // Max retransmit attempts

// TCP header
typedef struct __attribute__((packed)) {
    uint16_t src_port;      // Source port
    uint16_t dst_port;      // Destination port
    uint32_t seq_num;       // Sequence number
    uint32_t ack_num;       // Acknowledgment number
    uint8_t  data_offset;   // Data offset (4 bits) + reserved (4 bits)
    uint8_t  flags;         // Control flags
    uint16_t window;        // Window size
    uint16_t checksum;      // Checksum
    uint16_t urgent_ptr;    // Urgent pointer
} tcp_header_t;

// TCP pseudo-header for checksum
typedef struct __attribute__((packed)) {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_length;
} tcp_pseudo_header_t;

// TCP connection (TCB - Transmission Control Block)
typedef struct {
    int      state;             // Connection state
    uint32_t local_ip;          // Local IP
    uint16_t local_port;        // Local port
    uint32_t remote_ip;         // Remote IP
    uint16_t remote_port;       // Remote port

    // Sequence numbers
    uint32_t snd_una;           // Send unacknowledged
    uint32_t snd_nxt;           // Send next
    uint32_t snd_wnd;           // Send window
    uint32_t rcv_nxt;           // Receive next
    uint32_t rcv_wnd;           // Receive window
    uint32_t iss;               // Initial send sequence

    // Buffers
    uint8_t  send_buf[TCP_SEND_BUF_SIZE];
    uint16_t send_len;
    uint8_t  recv_buf[TCP_RECV_BUF_SIZE];
    uint16_t recv_len;
    uint16_t recv_read_pos;     // Read position in recv buffer

    // Retransmission
    uint32_t retransmit_time;   // When to retransmit
    int      retransmit_count;  // Retry counter

    int      active;            // Slot in use
} tcp_connection_t;

// Initialize TCP layer
void tcp_init(void);

// Create a new TCP connection (returns connection ID or -1)
int  tcp_connect(uint32_t remote_ip, uint16_t remote_port);

// Listen on a port (returns connection ID or -1)
int  tcp_listen(uint16_t port);

// Accept incoming connection on listening socket
int  tcp_accept(int listen_id);

// Send data on a connection
int  tcp_send(int conn_id, const uint8_t* data, uint16_t length);

// Receive data from a connection (returns bytes received)
int  tcp_recv(int conn_id, uint8_t* buffer, uint16_t max_len);

// Close a connection gracefully
void tcp_close(int conn_id);

// Process incoming TCP segment
void tcp_receive(uint32_t src_ip, uint32_t dst_ip, const uint8_t* data, uint16_t length);

// TCP timer tick (call periodically for retransmissions)
void tcp_timer(void);

// Get connection state
int  tcp_get_state(int conn_id);

// Check if data is available to read
int  tcp_data_available(int conn_id);

// Get connection info for debugging
const char* tcp_state_name(int state);

#endif
