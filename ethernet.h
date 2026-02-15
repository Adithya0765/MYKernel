// ethernet.h - Ethernet Frame Layer for Alteo OS
#ifndef ETHERNET_H
#define ETHERNET_H

#include "stdint.h"

// Ethernet constants
#define ETH_ALEN        6       // MAC address length
#define ETH_HLEN        14      // Ethernet header length
#define ETH_MTU         1500    // Maximum payload size
#define ETH_FRAME_MAX   1518    // Max frame size (header + payload + CRC)
#define ETH_FRAME_MIN   60      // Minimum frame size

// EtherType values
#define ETH_TYPE_IPV4   0x0800
#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IPV6   0x86DD
#define ETH_TYPE_VLAN   0x8100

// Broadcast MAC address
#define ETH_BROADCAST   ((uint8_t[]){0xFF,0xFF,0xFF,0xFF,0xFF,0xFF})

// Ethernet header
typedef struct __attribute__((packed)) {
    uint8_t  dest[ETH_ALEN];    // Destination MAC
    uint8_t  src[ETH_ALEN];     // Source MAC
    uint16_t ethertype;          // EtherType / Length
} eth_header_t;

// Ethernet frame (header + payload)
typedef struct {
    eth_header_t header;
    uint8_t      payload[ETH_MTU];
    uint16_t     payload_len;
} eth_frame_t;

// ARP header
typedef struct __attribute__((packed)) {
    uint16_t hw_type;       // Hardware type (1 = Ethernet)
    uint16_t proto_type;    // Protocol type (0x0800 = IPv4)
    uint8_t  hw_len;        // Hardware address length (6)
    uint8_t  proto_len;     // Protocol address length (4)
    uint16_t opcode;        // Operation (1=request, 2=reply)
    uint8_t  sender_mac[6]; // Sender MAC
    uint32_t sender_ip;     // Sender IP
    uint8_t  target_mac[6]; // Target MAC
    uint32_t target_ip;     // Target IP
} arp_header_t;

#define ARP_REQUEST     1
#define ARP_REPLY       2
#define ARP_HW_ETHER    1
#define ARP_CACHE_SIZE  32

// ARP cache entry
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint32_t timestamp;
    int      valid;
} arp_entry_t;

// Initialize ethernet layer
void eth_init(void);

// Send an ethernet frame
int  eth_send(const uint8_t dest_mac[6], uint16_t ethertype,
              const uint8_t* payload, uint16_t payload_len);

// Process a received ethernet frame
void eth_receive(const uint8_t* frame, uint16_t length);

// Get our MAC address
void eth_get_mac(uint8_t mac[6]);

// Byte order helpers
uint16_t htons(uint16_t val);
uint16_t ntohs(uint16_t val);
uint32_t htonl(uint32_t val);
uint32_t ntohl(uint32_t val);

// ARP operations
void arp_init(void);
int  arp_resolve(uint32_t ip, uint8_t mac[6]);
void arp_send_request(uint32_t target_ip);
void arp_process(const uint8_t* data, uint16_t len);
void arp_add_entry(uint32_t ip, const uint8_t mac[6]);

#endif
