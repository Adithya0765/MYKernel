// ip.h - IPv4 Network Layer for Alteo OS
#ifndef IP_H
#define IP_H

#include "stdint.h"

// IP protocol numbers
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

// IP header flags
#define IP_FLAG_DF      0x4000  // Don't Fragment
#define IP_FLAG_MF      0x2000  // More Fragments
#define IP_FRAG_MASK    0x1FFF  // Fragment offset mask

// IP version
#define IP_VERSION_4    4
#define IP_HEADER_LEN   20      // Minimum header length

// ICMP types
#define ICMP_ECHO_REPLY     0
#define ICMP_ECHO_REQUEST   8
#define ICMP_DEST_UNREACH   3
#define ICMP_TIME_EXCEEDED  11

// Maximum route table entries
#define MAX_ROUTES      8

// IPv4 header
typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;   // Version (4 bits) + IHL (4 bits)
    uint8_t  tos;           // Type of Service
    uint16_t total_length;  // Total Length
    uint16_t id;            // Identification
    uint16_t flags_frag;    // Flags (3 bits) + Fragment Offset (13 bits)
    uint8_t  ttl;           // Time to Live
    uint8_t  protocol;      // Protocol
    uint16_t checksum;      // Header Checksum
    uint32_t src_ip;        // Source Address
    uint32_t dst_ip;        // Destination Address
} ip_header_t;

// ICMP header
typedef struct __attribute__((packed)) {
    uint8_t  type;          // Message type
    uint8_t  code;          // Message code
    uint16_t checksum;      // Checksum
    uint16_t id;            // Identifier (echo)
    uint16_t sequence;      // Sequence number (echo)
} icmp_header_t;

// Route table entry
typedef struct {
    uint32_t network;       // Network address
    uint32_t netmask;       // Subnet mask
    uint32_t gateway;       // Gateway address
    int      metric;        // Route metric
    int      active;        // Entry in use
} route_entry_t;

// Network configuration
typedef struct {
    uint32_t ip_addr;       // Our IP address
    uint32_t netmask;       // Subnet mask
    uint32_t gateway;       // Default gateway
    uint32_t dns_server;    // DNS server
    uint32_t broadcast;     // Broadcast address
} net_config_t;

// IP address helper macros
#define IP_ADDR(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
#define IP_A(ip) (((ip)>>24)&0xFF)
#define IP_B(ip) (((ip)>>16)&0xFF)
#define IP_C(ip) (((ip)>>8)&0xFF)
#define IP_D(ip) ((ip)&0xFF)

// Initialize IP layer
void ip_init(void);

// Configure network
void ip_configure(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns);

// Get current network configuration
net_config_t* ip_get_config(void);

// Send an IP packet
int ip_send(uint32_t dest_ip, uint8_t protocol, const uint8_t* data, uint16_t length);

// Process a received IP packet
void ip_receive(const uint8_t* data, uint16_t length);

// Calculate IP checksum
uint16_t ip_checksum(const void* data, uint16_t length);

// ICMP operations
void icmp_send_echo_request(uint32_t dest_ip, uint16_t id, uint16_t seq);
void icmp_process(uint32_t src_ip, const uint8_t* data, uint16_t length);

// Ping utility
int  ping(uint32_t dest_ip, int count);

// Route management
void route_add(uint32_t network, uint32_t netmask, uint32_t gateway, int metric);
void route_remove(uint32_t network);
uint32_t route_lookup(uint32_t dest_ip);

// IP address utilities
void ip_to_str(uint32_t ip, char* buf);
uint32_t str_to_ip(const char* str);

#endif
