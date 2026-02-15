// ethernet.c - Ethernet Frame Layer for Alteo OS
#include "ethernet.h"
#include "e1000.h"
#include "ip.h"

static uint8_t our_mac[ETH_ALEN] = {0};
static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int eth_ready = 0;

// Byte swap helpers
uint16_t htons(uint16_t val) {
    return (val >> 8) | (val << 8);
}

uint16_t ntohs(uint16_t val) {
    return (val >> 8) | (val << 8);
}

uint32_t htonl(uint32_t val) {
    return ((val >> 24) & 0xFF) |
           ((val >> 8) & 0xFF00) |
           ((val << 8) & 0xFF0000) |
           ((val << 24) & 0xFF000000);
}

uint32_t ntohl(uint32_t val) {
    return htonl(val);
}

// Memory helpers
static void eth_memcpy(void* dst, const void* src, int n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

static void eth_memset(void* dst, uint8_t val, int n) {
    uint8_t* d = (uint8_t*)dst;
    for (int i = 0; i < n; i++) d[i] = val;
}

static int eth_memcmp(const void* a, const void* b, int n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    for (int i = 0; i < n; i++) {
        if (p[i] != q[i]) return p[i] - q[i];
    }
    return 0;
}

void eth_init(void) {
    // Initialize ARP cache
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = 0;

    // Get MAC from NIC driver
    if (e1000_is_available()) {
        e1000_get_mac(our_mac);
        eth_ready = 1;
    } else {
        eth_ready = 0;
    }
}

void eth_get_mac(uint8_t mac[6]) {
    eth_memcpy(mac, our_mac, 6);
}

int eth_send(const uint8_t dest_mac[6], uint16_t ethertype,
             const uint8_t* payload, uint16_t payload_len) {
    if (!eth_ready || !payload || payload_len > ETH_MTU)
        return -1;

    uint8_t frame[ETH_FRAME_MAX];
    eth_header_t* hdr = (eth_header_t*)frame;

    // Build header
    eth_memcpy(hdr->dest, dest_mac, ETH_ALEN);
    eth_memcpy(hdr->src, our_mac, ETH_ALEN);
    hdr->ethertype = htons(ethertype);

    // Copy payload
    eth_memcpy(frame + ETH_HLEN, payload, payload_len);

    // Pad if necessary
    uint16_t frame_len = ETH_HLEN + payload_len;
    if (frame_len < ETH_FRAME_MIN) {
        eth_memset(frame + frame_len, 0, ETH_FRAME_MIN - frame_len);
        frame_len = ETH_FRAME_MIN;
    }

    return e1000_send(frame, frame_len);
}

void eth_receive(const uint8_t* frame, uint16_t length) {
    if (!frame || length < ETH_HLEN) return;

    const eth_header_t* hdr = (const eth_header_t*)frame;
    uint16_t ethertype = ntohs(hdr->ethertype);
    const uint8_t* payload = frame + ETH_HLEN;
    uint16_t payload_len = length - ETH_HLEN;

    switch (ethertype) {
        case ETH_TYPE_ARP:
            arp_process(payload, payload_len);
            break;
        case ETH_TYPE_IPV4:
            ip_receive(payload, payload_len);
            break;
        default:
            break;
    }
}

// ---- ARP ----

void arp_init(void) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        arp_cache[i].valid = 0;
}

void arp_add_entry(uint32_t ip, const uint8_t mac[6]) {
    // Check for existing entry
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            eth_memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    // Find empty slot
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            eth_memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return;
        }
    }
    // Cache full, overwrite first entry
    arp_cache[0].ip = ip;
    eth_memcpy(arp_cache[0].mac, mac, 6);
    arp_cache[0].valid = 1;
}

int arp_resolve(uint32_t ip, uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            eth_memcpy(mac, arp_cache[i].mac, 6);
            return 0;
        }
    }
    // Not found, send ARP request
    arp_send_request(ip);
    return -1;
}

void arp_send_request(uint32_t target_ip) {
    if (!eth_ready) return;

    net_config_t* cfg = ip_get_config();
    arp_header_t arp;
    arp.hw_type = htons(ARP_HW_ETHER);
    arp.proto_type = htons(ETH_TYPE_IPV4);
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.opcode = htons(ARP_REQUEST);
    eth_memcpy(arp.sender_mac, our_mac, 6);
    arp.sender_ip = htonl(cfg->ip_addr);
    eth_memset(arp.target_mac, 0, 6);
    arp.target_ip = htonl(target_ip);

    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    eth_send(broadcast, ETH_TYPE_ARP, (uint8_t*)&arp, sizeof(arp));
}

void arp_process(const uint8_t* data, uint16_t len) {
    if (len < sizeof(arp_header_t)) return;

    const arp_header_t* arp = (const arp_header_t*)data;
    uint16_t opcode = ntohs(arp->opcode);

    // Add sender to our cache regardless
    arp_add_entry(ntohl(arp->sender_ip), arp->sender_mac);

    if (opcode == ARP_REQUEST) {
        net_config_t* cfg = ip_get_config();
        if (ntohl(arp->target_ip) == cfg->ip_addr) {
            // It's for us, send reply
            arp_header_t reply;
            reply.hw_type = htons(ARP_HW_ETHER);
            reply.proto_type = htons(ETH_TYPE_IPV4);
            reply.hw_len = 6;
            reply.proto_len = 4;
            reply.opcode = htons(ARP_REPLY);
            eth_memcpy(reply.sender_mac, our_mac, 6);
            reply.sender_ip = htonl(cfg->ip_addr);
            eth_memcpy(reply.target_mac, arp->sender_mac, 6);
            reply.target_ip = arp->sender_ip;

            eth_send(arp->sender_mac, ETH_TYPE_ARP, (uint8_t*)&reply, sizeof(reply));
        }
    }
}
