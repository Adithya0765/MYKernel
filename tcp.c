// tcp.c - TCP Transport Layer for Alteo OS
#include "tcp.h"
#include "ip.h"
#include "ethernet.h"

static tcp_connection_t connections[TCP_MAX_CONNECTIONS];
static uint16_t next_ephemeral_port = 49152;

// Memory helpers
static void tcp_memcpy(void* dst, const void* src, int n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static void tcp_memset(void* dst, uint8_t val, int n) {
    uint8_t* d = (uint8_t*)dst;
    for (int i = 0; i < n; i++) d[i] = val;
}

// Simple pseudo-random ISN
static uint32_t tcp_gen_isn(void) {
    static uint32_t seed = 0x12345678;
    seed = seed * 1103515245 + 12345;
    return seed;
}

void tcp_init(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        connections[i].active = 0;
        connections[i].state = TCP_STATE_CLOSED;
    }
}

static int tcp_alloc_conn(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            tcp_memset(&connections[i], 0, sizeof(tcp_connection_t));
            connections[i].active = 1;
            connections[i].state = TCP_STATE_CLOSED;
            connections[i].rcv_wnd = TCP_WINDOW_SIZE;
            return i;
        }
    }
    return -1;
}

static uint16_t tcp_alloc_port(void) {
    uint16_t port = next_ephemeral_port++;
    if (next_ephemeral_port > 65530) next_ephemeral_port = 49152;
    return port;
}

// Calculate TCP checksum
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t* tcp_data, uint16_t tcp_len) {
    uint32_t sum = 0;

    // Pseudo-header
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += htons(IP_PROTO_TCP);
    sum += htons(tcp_len);

    // TCP segment
    const uint16_t* ptr = (const uint16_t*)tcp_data;
    int remaining = tcp_len;
    while (remaining > 1) {
        sum += *ptr++;
        remaining -= 2;
    }
    if (remaining == 1) {
        sum += *(const uint8_t*)ptr;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
}

// Send a TCP segment
static int tcp_send_segment(tcp_connection_t* conn, uint8_t flags,
                            const uint8_t* data, uint16_t data_len) {
    uint8_t buf[TCP_MAX_SEGMENT + TCP_HEADER_LEN];
    tcp_header_t* hdr = (tcp_header_t*)buf;

    hdr->src_port = htons(conn->local_port);
    hdr->dst_port = htons(conn->remote_port);
    hdr->seq_num = htonl(conn->snd_nxt);
    hdr->ack_num = htonl(conn->rcv_nxt);
    hdr->data_offset = (TCP_HEADER_LEN / 4) << 4;
    hdr->flags = flags;
    hdr->window = htons((uint16_t)conn->rcv_wnd);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data && data_len > 0) {
        tcp_memcpy(buf + TCP_HEADER_LEN, data, data_len);
    }

    uint16_t total_len = TCP_HEADER_LEN + data_len;

    // Calculate checksum
    net_config_t* cfg = ip_get_config();
    hdr->checksum = tcp_checksum(htonl(cfg->ip_addr), htonl(conn->remote_ip),
                                  buf, total_len);

    // Update sequence number
    if (flags & TCP_SYN) conn->snd_nxt++;
    if (flags & TCP_FIN) conn->snd_nxt++;
    conn->snd_nxt += data_len;

    return ip_send(conn->remote_ip, IP_PROTO_TCP, buf, total_len);
}

int tcp_connect(uint32_t remote_ip, uint16_t remote_port) {
    int id = tcp_alloc_conn();
    if (id < 0) return -1;

    tcp_connection_t* conn = &connections[id];
    net_config_t* cfg = ip_get_config();

    conn->local_ip = cfg->ip_addr;
    conn->local_port = tcp_alloc_port();
    conn->remote_ip = remote_ip;
    conn->remote_port = remote_port;

    // Generate ISN
    conn->iss = tcp_gen_isn();
    conn->snd_nxt = conn->iss;
    conn->snd_una = conn->iss;
    conn->snd_wnd = TCP_WINDOW_SIZE;

    // Send SYN
    conn->state = TCP_STATE_SYN_SENT;
    tcp_send_segment(conn, TCP_SYN, 0, 0);

    return id;
}

int tcp_listen(uint16_t port) {
    int id = tcp_alloc_conn();
    if (id < 0) return -1;

    tcp_connection_t* conn = &connections[id];
    net_config_t* cfg = ip_get_config();

    conn->local_ip = cfg->ip_addr;
    conn->local_port = port;
    conn->state = TCP_STATE_LISTEN;

    return id;
}

int tcp_accept(int listen_id) {
    if (listen_id < 0 || listen_id >= TCP_MAX_CONNECTIONS) return -1;
    tcp_connection_t* lconn = &connections[listen_id];
    if (!lconn->active || lconn->state != TCP_STATE_LISTEN) return -1;

    // Check for established connections from this listener
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (i == listen_id) continue;
        if (connections[i].active && connections[i].state == TCP_STATE_ESTABLISHED &&
            connections[i].local_port == lconn->local_port) {
            return i;
        }
    }
    return -1; // No pending connections
}

int tcp_send(int conn_id, const uint8_t* data, uint16_t length) {
    if (conn_id < 0 || conn_id >= TCP_MAX_CONNECTIONS) return -1;
    tcp_connection_t* conn = &connections[conn_id];
    if (!conn->active || conn->state != TCP_STATE_ESTABLISHED) return -1;

    uint16_t sent = 0;
    while (sent < length) {
        uint16_t chunk = length - sent;
        if (chunk > TCP_MAX_SEGMENT) chunk = TCP_MAX_SEGMENT;

        int ret = tcp_send_segment(conn, TCP_ACK | TCP_PSH, data + sent, chunk);
        if (ret < 0) return sent > 0 ? (int)sent : ret;

        sent += chunk;
    }
    return sent;
}

int tcp_recv(int conn_id, uint8_t* buffer, uint16_t max_len) {
    if (conn_id < 0 || conn_id >= TCP_MAX_CONNECTIONS) return -1;
    tcp_connection_t* conn = &connections[conn_id];
    if (!conn->active) return -1;

    if (conn->recv_len == 0) return 0; // No data available

    uint16_t to_read = conn->recv_len - conn->recv_read_pos;
    if (to_read > max_len) to_read = max_len;

    tcp_memcpy(buffer, conn->recv_buf + conn->recv_read_pos, to_read);
    conn->recv_read_pos += to_read;

    // Reset buffer if fully read
    if (conn->recv_read_pos >= conn->recv_len) {
        conn->recv_len = 0;
        conn->recv_read_pos = 0;
    }

    return to_read;
}

void tcp_close(int conn_id) {
    if (conn_id < 0 || conn_id >= TCP_MAX_CONNECTIONS) return;
    tcp_connection_t* conn = &connections[conn_id];
    if (!conn->active) return;

    if (conn->state == TCP_STATE_ESTABLISHED) {
        conn->state = TCP_STATE_FIN_WAIT_1;
        tcp_send_segment(conn, TCP_FIN | TCP_ACK, 0, 0);
    } else if (conn->state == TCP_STATE_CLOSE_WAIT) {
        conn->state = TCP_STATE_LAST_ACK;
        tcp_send_segment(conn, TCP_FIN | TCP_ACK, 0, 0);
    } else {
        conn->state = TCP_STATE_CLOSED;
        conn->active = 0;
    }
}

// Find connection matching incoming segment
static int tcp_find_conn(uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
    // First try exact match
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) continue;
        if (connections[i].remote_ip == src_ip &&
            connections[i].remote_port == src_port &&
            connections[i].local_port == dst_port) {
            return i;
        }
    }
    // Then try listening socket
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) continue;
        if (connections[i].state == TCP_STATE_LISTEN &&
            connections[i].local_port == dst_port) {
            return i;
        }
    }
    return -1;
}

void tcp_receive(uint32_t src_ip, uint32_t dst_ip, const uint8_t* data, uint16_t length) {
    if (length < TCP_HEADER_LEN) return;

    const tcp_header_t* hdr = (const tcp_header_t*)data;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seq = ntohl(hdr->seq_num);
    uint32_t ack = ntohl(hdr->ack_num);
    uint8_t flags = hdr->flags;
    uint8_t data_off = (hdr->data_offset >> 4) * 4;
    uint16_t payload_len = length - data_off;
    const uint8_t* payload = data + data_off;

    (void)dst_ip;

    int conn_id = tcp_find_conn(src_ip, src_port, dst_port);
    if (conn_id < 0) {
        // No matching connection, send RST if not RST already
        if (!(flags & TCP_RST)) {
            // Would send RST here in full implementation
        }
        return;
    }

    tcp_connection_t* conn = &connections[conn_id];

    switch (conn->state) {
        case TCP_STATE_LISTEN:
            if (flags & TCP_SYN) {
                // Create new connection for incoming SYN
                int new_id = tcp_alloc_conn();
                if (new_id < 0) return;

                tcp_connection_t* nc = &connections[new_id];
                nc->local_ip = conn->local_ip;
                nc->local_port = conn->local_port;
                nc->remote_ip = src_ip;
                nc->remote_port = src_port;
                nc->rcv_nxt = seq + 1;
                nc->iss = tcp_gen_isn();
                nc->snd_nxt = nc->iss;
                nc->snd_una = nc->iss;
                nc->state = TCP_STATE_SYN_RECEIVED;

                tcp_send_segment(nc, TCP_SYN | TCP_ACK, 0, 0);
            }
            break;

        case TCP_STATE_SYN_SENT:
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                conn->rcv_nxt = seq + 1;
                conn->snd_una = ack;
                conn->state = TCP_STATE_ESTABLISHED;
                tcp_send_segment(conn, TCP_ACK, 0, 0);
            }
            break;

        case TCP_STATE_SYN_RECEIVED:
            if (flags & TCP_ACK) {
                conn->snd_una = ack;
                conn->state = TCP_STATE_ESTABLISHED;
            }
            break;

        case TCP_STATE_ESTABLISHED:
            if (flags & TCP_FIN) {
                conn->rcv_nxt = seq + payload_len + 1;
                conn->state = TCP_STATE_CLOSE_WAIT;
                tcp_send_segment(conn, TCP_ACK, 0, 0);
            } else {
                // Process incoming data
                if (payload_len > 0 && seq == conn->rcv_nxt) {
                    uint16_t space = TCP_RECV_BUF_SIZE - conn->recv_len;
                    uint16_t to_copy = payload_len < space ? payload_len : space;
                    tcp_memcpy(conn->recv_buf + conn->recv_len, payload, to_copy);
                    conn->recv_len += to_copy;
                    conn->rcv_nxt += to_copy;

                    // Send ACK
                    tcp_send_segment(conn, TCP_ACK, 0, 0);
                }
                if (flags & TCP_ACK) {
                    conn->snd_una = ack;
                }
            }
            break;

        case TCP_STATE_FIN_WAIT_1:
            if (flags & TCP_ACK) {
                conn->snd_una = ack;
                if (flags & TCP_FIN) {
                    conn->rcv_nxt = seq + 1;
                    conn->state = TCP_STATE_TIME_WAIT;
                    tcp_send_segment(conn, TCP_ACK, 0, 0);
                } else {
                    conn->state = TCP_STATE_FIN_WAIT_2;
                }
            }
            break;

        case TCP_STATE_FIN_WAIT_2:
            if (flags & TCP_FIN) {
                conn->rcv_nxt = seq + 1;
                conn->state = TCP_STATE_TIME_WAIT;
                tcp_send_segment(conn, TCP_ACK, 0, 0);
            }
            break;

        case TCP_STATE_CLOSE_WAIT:
            // Waiting for application to close
            break;

        case TCP_STATE_LAST_ACK:
            if (flags & TCP_ACK) {
                conn->state = TCP_STATE_CLOSED;
                conn->active = 0;
            }
            break;

        case TCP_STATE_TIME_WAIT:
            // Would set timer to clean up
            conn->state = TCP_STATE_CLOSED;
            conn->active = 0;
            break;

        default:
            break;
    }
}

void tcp_timer(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) continue;

        // Clean up TIME_WAIT connections
        if (connections[i].state == TCP_STATE_TIME_WAIT) {
            connections[i].state = TCP_STATE_CLOSED;
            connections[i].active = 0;
        }
    }
}

int tcp_get_state(int conn_id) {
    if (conn_id < 0 || conn_id >= TCP_MAX_CONNECTIONS) return TCP_STATE_CLOSED;
    return connections[conn_id].state;
}

int tcp_data_available(int conn_id) {
    if (conn_id < 0 || conn_id >= TCP_MAX_CONNECTIONS) return 0;
    tcp_connection_t* conn = &connections[conn_id];
    if (!conn->active) return 0;
    return conn->recv_len - conn->recv_read_pos;
}

const char* tcp_state_name(int state) {
    switch (state) {
        case TCP_STATE_CLOSED:       return "CLOSED";
        case TCP_STATE_LISTEN:       return "LISTEN";
        case TCP_STATE_SYN_SENT:     return "SYN_SENT";
        case TCP_STATE_SYN_RECEIVED: return "SYN_RCVD";
        case TCP_STATE_ESTABLISHED:  return "ESTABLISHED";
        case TCP_STATE_FIN_WAIT_1:   return "FIN_WAIT_1";
        case TCP_STATE_FIN_WAIT_2:   return "FIN_WAIT_2";
        case TCP_STATE_CLOSE_WAIT:   return "CLOSE_WAIT";
        case TCP_STATE_CLOSING:      return "CLOSING";
        case TCP_STATE_LAST_ACK:     return "LAST_ACK";
        case TCP_STATE_TIME_WAIT:    return "TIME_WAIT";
        default:                     return "UNKNOWN";
    }
}
