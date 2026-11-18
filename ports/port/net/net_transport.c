/*
 * net_transport.c  Non-blocking UDP socket, NAT hole-punching, Go R2 relay & MVC2_NETSIM
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_transport.h"
#include "naomi_host.h"

#define NETSIM_QUEUE_CAP 256
#define GGPO_RX_CAP      256
#define CSUM_RX_CAP      64
#define MAX_PKT_BYTES    2048

typedef struct {
    int used;
    uint32_t due_ms;
    uint32_t dst_ip_be;
    uint16_t dst_port_be;
    int len;
    uint8_t data[MAX_PKT_BYTES + 16];
} NETSIM_PKT;

typedef struct {
    uint32_t src_ip_be;
    uint16_t src_port_be;
    int len;
    uint8_t data[MAX_PKT_BYTES];
} RX_PKT;

typedef struct {
    uint32_t frame;
    uint64_t hash64;
} CSUM_MSG;

static struct {
    int opened;
    SOCKET sock;
    uint16_t local_port;
    uint32_t peer_ip_be;
    uint16_t peer_port_be;
    int has_peer;
    uint32_t relay_ip_be;
    uint16_t relay_port_be;
    int has_relay;
    uint32_t relay_token;
    uint8_t side;
    int force_relay;
    int direct_ok;
    int use_relay;
    uint32_t punch_start_ms;
    uint32_t last_punch_ms;

    /* MVC2_NETSIM */
    int netsim_on;
    int delay_ms;
    int jitter_ms;
    int loss_pct;
    uint32_t prng;
    NETSIM_PKT txq[NETSIM_QUEUE_CAP];

    /* Demuxed RX queues */
    RX_PKT ggpo_rx[GGPO_RX_CAP];
    int rx_head, rx_tail;

    CSUM_MSG csum_rx[CSUM_RX_CAP];
    int cs_head, cs_tail;

    uint32_t stat_sent;
    uint32_t stat_recv;
    uint32_t stat_relayed;
    uint32_t stat_dropped;
} nt;

static uint32_t mono_ms(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint32_t)((now.QuadPart * 1000ULL) / (uint64_t)freq.QuadPart);
}

static uint32_t xs32(void) {
    uint32_t x = nt.prng ? nt.prng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    nt.prng = x;
    return x;
}

static void parse_netsim_env(void) {
    const char* env = getenv("MVC2_NETSIM");
    const char* p;
    nt.netsim_on = 0;
    nt.delay_ms = 0;
    nt.jitter_ms = 0;
    nt.loss_pct = 0;
    if (env == NULL || env[0] == 0) return;
    p = env;
    while (*p != 0) {
        if (strncmp(p, "delay_ms=", 9) == 0) {
            nt.delay_ms = atoi(p + 9);
            nt.netsim_on = 1;
        } else if (strncmp(p, "jitter_ms=", 10) == 0) {
            nt.jitter_ms = atoi(p + 10);
            nt.netsim_on = 1;
        } else if (strncmp(p, "loss_pct=", 9) == 0) {
            nt.loss_pct = atoi(p + 9);
            nt.netsim_on = 1;
        }
        p = strchr(p, ',');
        if (p == NULL) break;
        p++;
    }
    if (nt.loss_pct < 0) nt.loss_pct = 0;
    if (nt.loss_pct > 95) nt.loss_pct = 95;
    if (nt.delay_ms < 0) nt.delay_ms = 0;
    if (nt.jitter_ms < 0) nt.jitter_ms = 0;
}

int net_transport_parse_endpoint(const char* spec, uint32_t* out_ip_be, uint16_t* out_port_be) {
    char host[256];
    const char* colon;
    size_t hlen;
    int port;
    unsigned long addr;
    struct addrinfo hints, *res = NULL;

    if (spec == NULL || spec[0] == 0) return 0;
    colon = strrchr(spec, ':');
    if (colon == NULL) return 0;
    hlen = (size_t)(colon - spec);
    if (hlen == 0 || hlen >= sizeof host) return 0;
    memcpy(host, spec, hlen);
    host[hlen] = 0;
    port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return 0;

    if (strcmp(host, "localhost") == 0) {
        snprintf(host, sizeof host, "127.0.0.1");
    }
    addr = inet_addr(host);
    if (addr != INADDR_NONE) {
        *out_ip_be = (uint32_t)addr;
        *out_port_be = htons((uint16_t)port);
        return 1;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res != NULL) {
        struct sockaddr_in* sin = (struct sockaddr_in*)res->ai_addr;
        *out_ip_be = (uint32_t)sin->sin_addr.s_addr;
        *out_port_be = htons((uint16_t)port);
        freeaddrinfo(res);
        return 1;
    }
    if (res) freeaddrinfo(res);
    return 0;
}

static void raw_send_now(const uint8_t* data, int len, uint32_t dst_ip_be, uint16_t dst_port_be) {
    struct sockaddr_in sin;
    if (!nt.opened || nt.sock == INVALID_SOCKET || data == NULL || len <= 0) return;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = dst_ip_be;
    sin.sin_port = dst_port_be;
    if (sendto(nt.sock, (const char*)data, len, 0, (const struct sockaddr*)&sin, sizeof sin) != SOCKET_ERROR) {
        nt.stat_sent++;
    }
}

static void send_packet_maybe_netsim(const uint8_t* data, int len, uint32_t dst_ip_be, uint16_t dst_port_be, int allow_drop) {
    uint32_t now;
    int i, jitter = 0, slot = -1;
    if (!nt.netsim_on) {
        raw_send_now(data, len, dst_ip_be, dst_port_be);
        return;
    }
    if (allow_drop && nt.loss_pct > 0 && (int)(xs32() % 100u) < nt.loss_pct) {
        nt.stat_dropped++;
        return;
    }
    if (nt.jitter_ms > 0) {
        int span = nt.jitter_ms * 2 + 1;
        jitter = (int)(xs32() % (uint32_t)span) - nt.jitter_ms;
    }
    {
        int total_delay = nt.delay_ms + jitter;
        if (total_delay <= 0) {
            raw_send_now(data, len, dst_ip_be, dst_port_be);
            return;
        }
        now = mono_ms();
        for (i = 0; i < NETSIM_QUEUE_CAP; i++) {
            if (!nt.txq[i].used) {
                slot = i;
                break;
            }
        }
        if (slot < 0 || len > (int)sizeof(nt.txq[0].data)) {
            raw_send_now(data, len, dst_ip_be, dst_port_be);
            return;
        }
        nt.txq[slot].used = 1;
        nt.txq[slot].due_ms = now + (uint32_t)total_delay;
        nt.txq[slot].dst_ip_be = dst_ip_be;
        nt.txq[slot].dst_port_be = dst_port_be;
        nt.txq[slot].len = len;
        memcpy(nt.txq[slot].data, data, (size_t)len);
    }
}

static void send_wire_payload(const uint8_t* payload, int len, uint32_t ip_be, uint16_t port_be, int allow_drop) {
    uint8_t wrapped[MAX_PKT_BYTES + 16];
    if (len <= 0 || len > MAX_PKT_BYTES) return;
    if (nt.use_relay && nt.has_relay) {
        wrapped[0] = 'R';
        wrapped[1] = '2';
        wrapped[2] = 0;
        wrapped[3] = nt.side;
        wrapped[4] = (uint8_t)(nt.relay_token & 0xFFu);
        wrapped[5] = (uint8_t)((nt.relay_token >> 8) & 0xFFu);
        wrapped[6] = (uint8_t)((nt.relay_token >> 16) & 0xFFu);
        wrapped[7] = (uint8_t)((nt.relay_token >> 24) & 0xFFu);
        memcpy(wrapped + 8, payload, (size_t)len);
        nt.stat_relayed++;
        send_packet_maybe_netsim(wrapped, len + 8, nt.relay_ip_be, nt.relay_port_be, allow_drop);
        return;
    }
    send_packet_maybe_netsim(payload, len, ip_be, port_be, allow_drop);
}

static void flush_netsim_txq(void) {
    uint32_t now = mono_ms();
    int i;
    if (!nt.netsim_on) return;
    for (i = 0; i < NETSIM_QUEUE_CAP; i++) {
        if (nt.txq[i].used && (int32_t)(now - nt.txq[i].due_ms) >= 0) {
            raw_send_now(nt.txq[i].data, nt.txq[i].len, nt.txq[i].dst_ip_be, nt.txq[i].dst_port_be);
            nt.txq[i].used = 0;
        }
    }
}

static void send_punch_packets(void) {
    uint8_t pkt[11];
    pkt[0] = 'P'; pkt[1] = 'U'; pkt[2] = 'N'; pkt[3] = 'C'; pkt[4] = 'H'; pkt[5] = '!';
    pkt[6] = (uint8_t)(nt.relay_token & 0xFFu);
    pkt[7] = (uint8_t)((nt.relay_token >> 8) & 0xFFu);
    pkt[8] = (uint8_t)((nt.relay_token >> 16) & 0xFFu);
    pkt[9] = (uint8_t)((nt.relay_token >> 24) & 0xFFu);
    pkt[10] = nt.side;
    if (nt.has_relay) {
        raw_send_now(pkt, sizeof pkt, nt.relay_ip_be, nt.relay_port_be);
    }
    if (nt.has_peer && !nt.force_relay) {
        raw_send_now(pkt, sizeof pkt, nt.peer_ip_be, nt.peer_port_be);
    }
}

static void drain_socket_rx(void) {
    uint8_t buf[MAX_PKT_BYTES + 16];
    struct sockaddr_in from;
    int from_len;
    for (;;) {
        int r, next_tail;
        const uint8_t* pay;
        int pay_len;
        uint32_t src_ip;
        uint16_t src_port;

        from_len = sizeof from;
        r = recvfrom(nt.sock, (char*)buf, sizeof buf, 0, (struct sockaddr*)&from, &from_len);
        if (r == SOCKET_ERROR) {
            break;
        }
        if (r <= 0) continue;
        nt.stat_recv++;

        pay = buf;
        pay_len = r;
        src_ip = (uint32_t)from.sin_addr.s_addr;
        src_port = (uint16_t)from.sin_port;

        /* Unwrap Go R2 relay header when arriving from coordinator relay endpoint */
        if (nt.has_relay && src_ip == nt.relay_ip_be && src_port == nt.relay_port_be &&
            r > 8 && buf[0] == 'R' && buf[1] == '2') {
            uint32_t tok = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                           ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
            if (tok != nt.relay_token) {
                continue;
            }
            pay = buf + 8;
            pay_len = r - 8;
            if (nt.has_peer) {
                src_ip = nt.peer_ip_be;
                src_port = nt.peer_port_be;
            }
        }

        /* Handle NAT hole-punch probes */
        if (pay_len == 11 && memcmp(pay, "PUNCH!", 6) == 0) {
            if (!nt.has_relay || src_ip != nt.relay_ip_be || src_port != nt.relay_port_be) {
                uint8_t back[11];
                nt.direct_ok = 1;
                memcpy(back, pay, 11);
                back[0] = 'P'; back[1] = 'B'; back[2] = 'A'; back[3] = 'C'; back[4] = 'K'; back[5] = '!';
                raw_send_now(back, sizeof back, src_ip, src_port);
            }
            continue;
        }
        if (pay_len == 11 && memcmp(pay, "PBACK!", 6) == 0) {
            if (!nt.has_relay || src_ip != nt.relay_ip_be || src_port != nt.relay_port_be) {
                nt.direct_ok = 1;
            }
            continue;
        }

        /* Handle out-of-band 60f checksum packets */
        if (pay_len == 20 && memcmp(pay, "CSUM64", 6) == 0) {
            int cs_next = (nt.cs_tail + 1) % CSUM_RX_CAP;
            if (cs_next != nt.cs_head) {
                uint32_t frame = (uint32_t)pay[8] | ((uint32_t)pay[9] << 8) |
                                 ((uint32_t)pay[10] << 16) | ((uint32_t)pay[11] << 24);
                uint64_t h = (uint64_t)pay[12] | ((uint64_t)pay[13] << 8) |
                             ((uint64_t)pay[14] << 16) | ((uint64_t)pay[15] << 24) |
                             ((uint64_t)pay[16] << 32) | ((uint64_t)pay[17] << 40) |
                             ((uint64_t)pay[18] << 48) | ((uint64_t)pay[19] << 56);
                nt.csum_rx[nt.cs_tail].frame = frame;
                nt.csum_rx[nt.cs_tail].hash64 = h;
                nt.cs_tail = cs_next;
            }
            continue;
        }

        /* Enqueue GGPO UdpMsg packet */
        if (pay_len > MAX_PKT_BYTES) continue;
        next_tail = (nt.rx_tail + 1) % GGPO_RX_CAP;
        if (next_tail != nt.rx_head) {
            nt.ggpo_rx[nt.rx_tail].src_ip_be = src_ip;
            nt.ggpo_rx[nt.rx_tail].src_port_be = src_port;
            nt.ggpo_rx[nt.rx_tail].len = pay_len;
            memcpy(nt.ggpo_rx[nt.rx_tail].data, pay, (size_t)pay_len);
            nt.rx_tail = next_tail;
        }
    }
}

int net_transport_open(const NET_TRANSPORT_CFG* cfg) {
    WSADATA wsa;
    struct sockaddr_in sin;
    u_long nonblock = 1;
    int optval = 1;

    net_transport_close();
    memset(&nt, 0, sizeof nt);
    nt.sock = INVALID_SOCKET;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 0;
    }

    nt.local_port = cfg ? cfg->local_port : 0;
    nt.relay_token = cfg ? cfg->relay_token : 0;
    nt.side = (cfg && (cfg->side == 1 || cfg->side == 2)) ? cfg->side : 1;
    nt.force_relay = cfg ? cfg->force_relay : 0;
    if (cfg && cfg->peer_addr && net_transport_parse_endpoint(cfg->peer_addr, &nt.peer_ip_be, &nt.peer_port_be)) {
        nt.has_peer = 1;
    }
    if (cfg && cfg->relay_addr && net_transport_parse_endpoint(cfg->relay_addr, &nt.relay_ip_be, &nt.relay_port_be)) {
        nt.has_relay = 1;
    }
    if (!nt.has_peer && nt.has_relay) {
        nt.peer_ip_be = nt.relay_ip_be;
        nt.peer_port_be = nt.relay_port_be;
        nt.has_peer = 1;
    }

    nt.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (nt.sock == INVALID_SOCKET) {
        return 0;
    }
    setsockopt(nt.sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof optval);
    ioctlsocket(nt.sock, FIONBIO, &nonblock);

    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(nt.local_port);
    if (bind(nt.sock, (const struct sockaddr*)&sin, sizeof sin) == SOCKET_ERROR) {
        closesocket(nt.sock);
        nt.sock = INVALID_SOCKET;
        return 0;
    }
    if (nt.local_port == 0) {
        int slen = sizeof sin;
        if (getsockname(nt.sock, (struct sockaddr*)&sin, &slen) == 0) {
            nt.local_port = ntohs(sin.sin_port);
        }
    }

    parse_netsim_env();
    nt.prng = (uint32_t)GetCurrentProcessId() ^ ((uint32_t)nt.side * 2654435761u) ^ 0xA511E9B3u;
    nt.use_relay = (nt.force_relay && nt.has_relay) ? 1 : 0;
    nt.punch_start_ms = mono_ms();
    nt.opened = 1;

    host_log("net_transport: bound udp:%u peer=%s relay=%s force_relay=%d netsim=(delay=%d jitter=%d loss=%d%%)",
             nt.local_port,
             (cfg && cfg->peer_addr) ? cfg->peer_addr : "none",
             (cfg && cfg->relay_addr) ? cfg->relay_addr : "none",
             nt.force_relay, nt.delay_ms, nt.jitter_ms, nt.loss_pct);
    return 1;
}

void net_transport_close(void) {
    if (nt.opened) {
        if (nt.sock != INVALID_SOCKET) {
            closesocket(nt.sock);
            nt.sock = INVALID_SOCKET;
        }
        WSACleanup();
    }
    memset(&nt, 0, sizeof nt);
    nt.sock = INVALID_SOCKET;
}

void net_transport_punch(void) {
    uint32_t now;
    if (!nt.opened) return;
    now = mono_ms();
    if (!nt.force_relay && !nt.direct_ok && (now - nt.punch_start_ms) < 1500u) {
        if (nt.last_punch_ms == 0 || (now - nt.last_punch_ms) >= 50u) {
            send_punch_packets();
            nt.last_punch_ms = now;
        }
    } else if (nt.force_relay && nt.has_relay && nt.stat_recv == 0) {
        if (nt.last_punch_ms == 0 || (now - nt.last_punch_ms) >= 250u) {
            send_punch_packets();
            nt.last_punch_ms = now;
        }
    } else if (nt.has_relay && !nt.direct_ok && !nt.use_relay) {
        nt.use_relay = 1;
        host_log("net_transport: symmetric NAT detected after 1.5s punch, activating Go R2 UDP relay");
    }
}

void net_transport_tick(void) {
    if (!nt.opened) return;
    net_transport_punch();
    flush_netsim_txq();
    drain_socket_rx();
}

int net_transport_ggpo_send(void* user, const char* buf, int len, unsigned int ip_be, unsigned short port_be) {
    (void)user;
    if (!nt.opened || buf == NULL || len <= 0) return -1;
    send_wire_payload((const uint8_t*)buf, len, ip_be, port_be, 1);
    return len;
}

int net_transport_ggpo_recv(void* user, char* buf, int max_len, unsigned int* ip_be, unsigned short* port_be) {
    RX_PKT* pkt;
    int copy_len;
    (void)user;
    if (!nt.opened || buf == NULL || max_len <= 0) return 0;
    net_transport_tick();
    if (nt.rx_head == nt.rx_tail) return 0;
    pkt = &nt.ggpo_rx[nt.rx_head];
    nt.rx_head = (nt.rx_head + 1) % GGPO_RX_CAP;
    copy_len = pkt->len <= max_len ? pkt->len : max_len;
    memcpy(buf, pkt->data, (size_t)copy_len);
    if (ip_be) *ip_be = pkt->src_ip_be;
    if (port_be) *port_be = pkt->src_port_be;
    return copy_len;
}

void net_transport_send_checksum(uint32_t frame, uint64_t hash64) {
    uint8_t pkt[20];
    int i;
    if (!nt.opened || !nt.has_peer) return;
    pkt[0] = 'C'; pkt[1] = 'S'; pkt[2] = 'U'; pkt[3] = 'M'; pkt[4] = '6'; pkt[5] = '4';
    pkt[6] = nt.side;
    pkt[7] = 0;
    for (i = 0; i < 4; i++) pkt[8 + i] = (uint8_t)((frame >> (i * 8)) & 0xFFu);
    for (i = 0; i < 8; i++) pkt[12 + i] = (uint8_t)((hash64 >> (i * 8)) & 0xFFu);
    /* Do not drop 60f determinism verification packets; still apply simulated delay/jitter */
    send_wire_payload(pkt, sizeof pkt, nt.peer_ip_be, nt.peer_port_be, 0);
}

int net_transport_poll_checksum(uint32_t* out_frame, uint64_t* out_hash64) {
    if (!nt.opened) return 0;
    net_transport_tick();
    if (nt.cs_head == nt.cs_tail) return 0;
    if (out_frame) *out_frame = nt.csum_rx[nt.cs_head].frame;
    if (out_hash64) *out_hash64 = nt.csum_rx[nt.cs_head].hash64;
    nt.cs_head = (nt.cs_head + 1) % CSUM_RX_CAP;
    return 1;
}

int net_transport_using_relay(void) {
    return nt.opened && nt.use_relay;
}

int net_transport_direct_ready(void) {
    return nt.opened && nt.direct_ok;
}

void net_transport_stats(uint32_t* out_sent, uint32_t* out_recv, uint32_t* out_relayed, uint32_t* out_dropped) {
    if (out_sent) *out_sent = nt.stat_sent;
    if (out_recv) *out_recv = nt.stat_recv;
    if (out_relayed) *out_relayed = nt.stat_relayed;
    if (out_dropped) *out_dropped = nt.stat_dropped;
}
