/*
 * uri_ipc.c  umvsc2:// protocol handler + loopback 127.0.0.1 single-instance URI handoff
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

#include "uri_ipc.h"
#include "naomi_host.h"

#define URI_MAX 1024

static SOCKET ipc_sock = INVALID_SOCKET;
static int wsa_up;
static char port_file_path[1100];
static char pending_uri[URI_MAX];
static int has_pending;

static void build_port_path_from_cli(int argc, char** argv, char* out, size_t out_sz) {
    const char* cfg = NULL;
    char dir[1024];
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            cfg = argv[i + 1];
            break;
        }
        if (strncmp(argv[i], "--config=", 9) == 0) {
            cfg = argv[i] + 9;
            break;
        }
    }
    if (cfg != NULL && cfg[0] != 0) {
        size_t len = strlen(cfg);
        snprintf(dir, sizeof dir, "%s", cfg);
        /* If points to .cfg file, strip filename */
        if (len >= 4 && _stricmp(dir + len - 4, ".cfg") == 0) {
            char* s1 = strrchr(dir, '/');
            char* s2 = strrchr(dir, '\\');
            char* sep = (s1 > s2) ? s1 : s2;
            if (sep) sep[1] = 0; else dir[0] = 0;
        }
        if (dir[0] != 0 && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != '\\') {
            snprintf(out, out_sz, "%s/mvc2_ipc.port", dir);
        } else {
            snprintf(out, out_sz, "%smvc2_ipc.port", dir);
        }
        return;
    }
    {
        DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir - 1);
        char* sep;
        dir[n] = 0;
        sep = strrchr(dir, '\\');
        if (!sep) sep = strrchr(dir, '/');
        if (sep) sep[1] = 0;
        snprintf(out, out_sz, "%smvc2_ipc.port", dir);
    }
}

static const char* find_join_arg(int argc, char** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--join") == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
        if (strncmp(argv[i], "--join=", 7) == 0) {
            return argv[i] + 7;
        }
    }
    return NULL;
}

int uri_ipc_try_forward_cli(int argc, char** argv) {
    const char* join_uri = find_join_arg(argc, argv);
    char ppath[1100];
    FILE* f;
    unsigned port = 0;
    WSADATA wsa;
    SOCKET s;
    struct sockaddr_in dst, src;
    char pkt[URI_MAX + 8];
    char ack[16];
    int slen, n;
    fd_set fds;
    struct timeval tv;

    if (join_uri == NULL || join_uri[0] == 0) return 0;

    build_port_path_from_cli(argc, argv, ppath, sizeof ppath);
    f = fopen(ppath, "r");
    if (f == NULL) {
        uri_ipc_push_pending(join_uri);
        return 0;
    }
    if (fscanf(f, "%u", &port) != 1 || port == 0 || port > 65535u) {
        fclose(f);
        uri_ipc_push_pending(join_uri);
        return 0;
    }
    fclose(f);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }

    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons((uint16_t)port);

    snprintf(pkt, sizeof pkt, "JOIN %s", join_uri);
    sendto(s, pkt, (int)strlen(pkt), 0, (const struct sockaddr*)&dst, sizeof dst);

    FD_ZERO(&fds);
    FD_SET(s, &fds);
    tv.tv_sec = 0;
    tv.tv_usec = 400000;
    if (select(0, &fds, NULL, NULL, &tv) > 0) {
        slen = sizeof src;
        n = recvfrom(s, ack, sizeof ack - 1, 0, (struct sockaddr*)&src, &slen);
        if (n >= 3 && strncmp(ack, "ACK", 3) == 0) {
            fprintf(stdout, "URI_IPC FORWARDED uri=%s port=%u\n", join_uri, port);
            fflush(stdout);
            closesocket(s);
            WSACleanup();
            return 1;
        }
    }

    closesocket(s);
    WSACleanup();
    uri_ipc_push_pending(join_uri);
    return 0;
}

void uri_ipc_register_protocol(void) {
    char exe[1024];
    char cmd[1200];
    HKEY hk = NULL, hk_cmd = NULL;
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe - 1);
    if (n == 0 || n >= sizeof exe) return;
    exe[n] = 0;

    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Classes\\umvsc2", 0, NULL,
                        0, KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        const char* desc = "URL:UMvSC2 Netplay Join Link";
        const char* empty = "";
        RegSetValueExA(hk, NULL, 0, REG_SZ, (const BYTE*)desc, (DWORD)strlen(desc) + 1);
        RegSetValueExA(hk, "URL Protocol", 0, REG_SZ, (const BYTE*)empty, 1);
        if (RegCreateKeyExA(hk, "shell\\open\\command", 0, NULL,
                            0, KEY_WRITE, NULL, &hk_cmd, NULL) == ERROR_SUCCESS) {
            snprintf(cmd, sizeof cmd, "\"%s\" --join \"%%1\"", exe);
            RegSetValueExA(hk_cmd, NULL, 0, REG_SZ, (const BYTE*)cmd, (DWORD)strlen(cmd) + 1);
            RegCloseKey(hk_cmd);
        }
        RegCloseKey(hk);
    }
}

int uri_ipc_listen_start(void) {
    WSADATA wsa;
    struct sockaddr_in sin;
    u_long nonblock = 1;
    int slen;
    FILE* f;

    uri_ipc_listen_stop();
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    wsa_up = 1;

    ipc_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ipc_sock == INVALID_SOCKET) {
        WSACleanup();
        wsa_up = 0;
        return 0;
    }
    ioctlsocket(ipc_sock, FIONBIO, &nonblock);

    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    if (bind(ipc_sock, (const struct sockaddr*)&sin, sizeof sin) == SOCKET_ERROR) {
        uri_ipc_listen_stop();
        return 0;
    }
    slen = sizeof sin;
    if (getsockname(ipc_sock, (struct sockaddr*)&sin, &slen) != 0) {
        uri_ipc_listen_stop();
        return 0;
    }

    snprintf(port_file_path, sizeof port_file_path, "%s", host_config_path("mvc2_ipc.port"));
    f = fopen(port_file_path, "w");
    if (f) {
        fprintf(f, "%u\n", (unsigned)ntohs(sin.sin_port));
        fclose(f);
    }
    host_log("uri_ipc: listening 127.0.0.1:%u (%s)", (unsigned)ntohs(sin.sin_port), port_file_path);
    return 1;
}

void uri_ipc_listen_stop(void) {
    if (port_file_path[0] != 0) {
        remove(port_file_path);
        port_file_path[0] = 0;
    }
    if (ipc_sock != INVALID_SOCKET) {
        closesocket(ipc_sock);
        ipc_sock = INVALID_SOCKET;
    }
    if (wsa_up) {
        WSACleanup();
        wsa_up = 0;
    }
}

void uri_ipc_push_pending(const char* uri) {
    if (uri != NULL && uri[0] != 0) {
        snprintf(pending_uri, sizeof pending_uri, "%s", uri);
        has_pending = 1;
    }
}

int uri_ipc_poll(char* out_uri, size_t max_len) {
    char buf[URI_MAX + 16];
    struct sockaddr_in src;
    int slen, n;

    if (has_pending) {
        if (out_uri && max_len > 0) snprintf(out_uri, max_len, "%s", pending_uri);
        has_pending = 0;
        return 1;
    }
    if (ipc_sock == INVALID_SOCKET) return 0;

    slen = sizeof src;
    n = recvfrom(ipc_sock, buf, sizeof buf - 1, 0, (struct sockaddr*)&src, &slen);
    if (n <= 0) return 0;
    buf[n] = 0;

    if (strncmp(buf, "JOIN ", 5) == 0) {
        sendto(ipc_sock, "ACK", 3, 0, (const struct sockaddr*)&src, slen);
        if (out_uri && max_len > 0) snprintf(out_uri, max_len, "%s", buf + 5);
        return 1;
    }
    return 0;
}
