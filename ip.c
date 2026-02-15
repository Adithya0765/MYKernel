// ip.c - IPv4 Network Layer for Alteo OS
#include "ip.h"
#include "ethernet.h"
#include "tcp.h"

// Network configuration
static net_config_t net_cfg = {0};
static route_entry_t route_table[MAX_ROUTES];
static uint16_t ip_id_counter = 0;

// Memory helper
static void ip_memcpy(void* dst, const void* src, int n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static void ip_memset(void* dst, uint8_t val, int n) {
    uint8_t* d = (uint8_t*)dst;
    for (int i = 0; i < n; i++) d[i] = val;
}

void ip_init(void) {
    // Default configuration (10.0.2.15 for QEMU)
    net_cfg.ip_addr = IP_ADDR(10, 0, 2, 15);
    net_cfg.netmask = IP_ADDR(255, 255, 255, 0);
    net_cfg.gateway = IP_ADDR(10, 0, 2, 2);
    net_cfg.dns_server = IP_ADDR(10, 0, 2, 3);
    net_cfg.broadcast = IP_ADDR(10, 0, 2, 255);

    // Clear route table
    for (int i = 0; i < MAX_ROUTES; i++)
        route_table[i].active = 0;

    // Add default route
    route_add(IP_ADDR(10, 0, 2, 0), IP_ADDR(255, 255, 255, 0), 0, 0);
    route_add(0, 0, IP_ADDR(10, 0, 2, 2), 100); // Default gateway
}

void ip_configure(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns) {
    net_cfg.ip_addr = ip;
    net_cfg.netmask = netmask;
    net_cfg.gateway = gateway;
    net_cfg.dns_server = dns;
    net_cfg.broadcast = (ip & netmask) | (~netmask);
}

net_config_t* ip_get_config(void) {
    return &net_cfg;
}

uint16_t ip_checksum(const void* data, uint16_t length) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;

    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }
    if (length == 1) {
        sum += *(const uint8_t*)ptr;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
}

int ip_send(uint32_t dest_ip, uint8_t protocol, const uint8_t* data, uint16_t length) {
    if (length + IP_HEADER_LEN > ETH_MTU) return -1;

    uint8_t packet[ETH_MTU];
    ip_header_t* hdr = (ip_header_t*)packet;

    // Build IP header
    hdr->version_ihl = (IP_VERSION_4 << 4) | (IP_HEADER_LEN / 4);
    hdr->tos = 0;
    hdr->total_length = htons(IP_HEADER_LEN + length);
    hdr->id = htons(ip_id_counter++);
    hdr->flags_frag = htons(IP_FLAG_DF);
    hdr->ttl = 64;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src_ip = htonl(net_cfg.ip_addr);
    hdr->dst_ip = htonl(dest_ip);

    // Calculate checksum
    hdr->checksum = ip_checksum(hdr, IP_HEADER_LEN);

    // Copy payload
    ip_memcpy(packet + IP_HEADER_LEN, data, length);

    // Determine next hop
    uint32_t next_hop = dest_ip;
    if ((dest_ip & net_cfg.netmask) != (net_cfg.ip_addr & net_cfg.netmask)) {
        next_hop = net_cfg.gateway; // Use gateway for different subnet
    }

    // Resolve MAC via ARP
    uint8_t dest_mac[6];
    if (dest_ip == net_cfg.broadcast || dest_ip == 0xFFFFFFFF) {
        // Broadcast
        ip_memset(dest_mac, 0xFF, 6);
    } else {
        if (arp_resolve(next_hop, dest_mac) != 0) {
            return -2; // ARP resolution pending
        }
    }

    return eth_send(dest_mac, ETH_TYPE_IPV4, packet, IP_HEADER_LEN + length);
}

void ip_receive(const uint8_t* data, uint16_t length) {
    if (length < IP_HEADER_LEN) return;

    const ip_header_t* hdr = (const ip_header_t*)data;

    // Verify version
    uint8_t version = (hdr->version_ihl >> 4) & 0xF;
    if (version != 4) return;

    // Get header length
    uint8_t ihl = (hdr->version_ihl & 0xF) * 4;
    if (ihl < IP_HEADER_LEN || ihl > length) return;

    // Verify checksum
    if (ip_checksum(hdr, ihl) != 0) return;

    uint32_t dest = ntohl(hdr->dst_ip);
    // Check if packet is for us
    if (dest != net_cfg.ip_addr && dest != net_cfg.broadcast && dest != 0xFFFFFFFF)
        return;

    uint32_t src = ntohl(hdr->src_ip);
    uint16_t payload_len = ntohs(hdr->total_length) - ihl;
    const uint8_t* payload = data + ihl;

    switch (hdr->protocol) {
        case IP_PROTO_ICMP:
            icmp_process(src, payload, payload_len);
            break;
        case IP_PROTO_TCP:
            tcp_receive(src, dest, payload, payload_len);
            break;
        case IP_PROTO_UDP:
            // UDP handling (future)
            break;
        default:
            break;
    }
}

// ---- ICMP ----

static uint16_t ping_id = 1;
static uint16_t ping_seq = 0;
static int ping_received = 0;

void icmp_send_echo_request(uint32_t dest_ip, uint16_t id, uint16_t seq) {
    uint8_t buf[64];
    icmp_header_t* icmp = (icmp_header_t*)buf;

    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(id);
    icmp->sequence = htons(seq);

    // Payload (timestamp or pattern)
    for (int i = sizeof(icmp_header_t); i < 64; i++)
        buf[i] = (uint8_t)(i & 0xFF);

    icmp->checksum = ip_checksum(buf, 64);

    ip_send(dest_ip, IP_PROTO_ICMP, buf, 64);
}

void icmp_process(uint32_t src_ip, const uint8_t* data, uint16_t length) {
    if (length < sizeof(icmp_header_t)) return;

    const icmp_header_t* icmp = (const icmp_header_t*)data;

    switch (icmp->type) {
        case ICMP_ECHO_REQUEST: {
            // Reply to ping
            uint8_t reply[ETH_MTU];
            icmp_header_t* rep = (icmp_header_t*)reply;
            rep->type = ICMP_ECHO_REPLY;
            rep->code = 0;
            rep->checksum = 0;
            rep->id = icmp->id;
            rep->sequence = icmp->sequence;

            // Copy original data
            int copy_len = length - sizeof(icmp_header_t);
            if (copy_len > 0) {
                if (copy_len > (int)(ETH_MTU - sizeof(icmp_header_t)))
                    copy_len = ETH_MTU - sizeof(icmp_header_t);
                ip_memcpy(reply + sizeof(icmp_header_t),
                          data + sizeof(icmp_header_t), copy_len);
            }

            rep->checksum = ip_checksum(reply, sizeof(icmp_header_t) + copy_len);
            ip_send(src_ip, IP_PROTO_ICMP, reply, sizeof(icmp_header_t) + copy_len);
            break;
        }
        case ICMP_ECHO_REPLY:
            if (ntohs(icmp->id) == ping_id) {
                ping_received = 1;
            }
            break;
        default:
            break;
    }
}

int ping(uint32_t dest_ip, int count) {
    int replies = 0;
    for (int i = 0; i < count; i++) {
        ping_received = 0;
        ping_seq++;
        icmp_send_echo_request(dest_ip, ping_id, ping_seq);

        // Wait for reply (simple polling)
        for (int t = 0; t < 100000 && !ping_received; t++) {
            // Would poll NIC here in real implementation
            __asm__ __volatile__("pause");
        }

        if (ping_received) replies++;
    }
    return replies;
}

// ---- Routing ----

void route_add(uint32_t network, uint32_t netmask, uint32_t gateway, int metric) {
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (!route_table[i].active) {
            route_table[i].network = network;
            route_table[i].netmask = netmask;
            route_table[i].gateway = gateway;
            route_table[i].metric = metric;
            route_table[i].active = 1;
            return;
        }
    }
}

void route_remove(uint32_t network) {
    for (int i = 0; i < MAX_ROUTES; i++) {
        if (route_table[i].active && route_table[i].network == network) {
            route_table[i].active = 0;
            return;
        }
    }
}

uint32_t route_lookup(uint32_t dest_ip) {
    uint32_t best_gw = net_cfg.gateway;
    int best_metric = 999999;
    uint32_t longest_mask = 0;

    for (int i = 0; i < MAX_ROUTES; i++) {
        if (!route_table[i].active) continue;
        if ((dest_ip & route_table[i].netmask) == route_table[i].network) {
            if (route_table[i].netmask > longest_mask ||
                (route_table[i].netmask == longest_mask && route_table[i].metric < best_metric)) {
                longest_mask = route_table[i].netmask;
                best_metric = route_table[i].metric;
                best_gw = route_table[i].gateway ? route_table[i].gateway : dest_ip;
            }
        }
    }
    return best_gw;
}

// ---- Utility ----

void ip_to_str(uint32_t ip, char* buf) {
    int pos = 0;
    for (int i = 3; i >= 0; i--) {
        int octet = (ip >> (i * 8)) & 0xFF;
        if (octet >= 100) { buf[pos++] = '0' + octet / 100; }
        if (octet >= 10)  { buf[pos++] = '0' + (octet / 10) % 10; }
        buf[pos++] = '0' + octet % 10;
        if (i > 0) buf[pos++] = '.';
    }
    buf[pos] = 0;
}

uint32_t str_to_ip(const char* str) {
    uint32_t ip = 0;
    uint32_t octet = 0;
    int shift = 24;

    while (*str) {
        if (*str == '.') {
            ip |= (octet & 0xFF) << shift;
            shift -= 8;
            octet = 0;
        } else if (*str >= '0' && *str <= '9') {
            octet = octet * 10 + (*str - '0');
        }
        str++;
    }
    ip |= (octet & 0xFF) << shift;
    return ip;
}
