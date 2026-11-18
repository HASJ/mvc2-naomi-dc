/*
 * net_transport.h  Non-blocking Winsock2/BSD UDP transport, NAT hole-punching, R2 relay & MVC2_NETSIM
 */

#ifndef NET_TRANSPORT_H
#define NET_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

typedef struct NET_TRANSPORT_CFG {
    uint16_t    local_port;
    const char* peer_addr;     /* "ip:port" or NULL */
    const char* relay_addr;    /* coordinator/relay "ip:port" or NULL */
    uint32_t    relay_token;   /* 32-bit session token for R2 relay & punch */
    uint8_t     side;          /* 1 or 2 */
    int         force_relay;   /* 1 = force R2 relay immediately */
} NET_TRANSPORT_CFG;

int  net_transport_open(const NET_TRANSPORT_CFG* cfg);
void net_transport_close(void);
void net_transport_tick(void);

/* Initiates / advances 1.5s P2P hole-punching toward coordinator & peer */
void net_transport_punch(void);

/* GGPOTransportCallbacks bindings */
int  net_transport_ggpo_send(void* user, const char* buf, int len, unsigned int ip_be, unsigned short port_be);
int  net_transport_ggpo_recv(void* user, char* buf, int max_len, unsigned int* ip_be, unsigned short* port_be);

/* Out-of-band 60f peer determinism checksum messages */
void net_transport_send_checksum(uint32_t frame, uint64_t hash64);
int  net_transport_poll_checksum(uint32_t* out_frame, uint64_t* out_hash64);

/* Status telemetry */
int  net_transport_using_relay(void);
int  net_transport_direct_ready(void);
void net_transport_stats(uint32_t* out_sent, uint32_t* out_recv, uint32_t* out_relayed, uint32_t* out_dropped);

/* Parse "host:port" or dotted "ip:port" -> host byte order IP + port or network byte order helpers */
int  net_transport_parse_endpoint(const char* spec, uint32_t* out_ip_be, uint16_t* out_port_be);

/* Serverless direct P2P listener & outbound IP detection */
int      net_transport_detect_local_ip(char* out_ip, size_t cap);
uint16_t net_transport_listen_p2p(uint16_t preferred_port, uint32_t token);
int      net_transport_poll_p2p_peer(char* out_peer_addr, size_t cap);
uint16_t net_transport_local_port(void);

#endif
