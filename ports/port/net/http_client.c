/*
 * http_client.c  Non-blocking background WinHTTP REST & WebSocket client (zero main-thread stalls)
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_client.h"

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

#define MAX_HTTP_JOBS 32
#define MAX_HTTP_EVS  64

typedef HINTERNET (WINAPI *pfn_ws_upgrade_t)(HINTERNET, DWORD_PTR);
typedef DWORD (WINAPI *pfn_ws_send_t)(HINTERNET, WINHTTP_WEB_SOCKET_BUFFER_TYPE, void*, DWORD);
typedef DWORD (WINAPI *pfn_ws_recv_t)(HINTERNET, void*, DWORD, DWORD*, WINHTTP_WEB_SOCKET_BUFFER_TYPE*);
typedef DWORD (WINAPI *pfn_ws_close_t)(HINTERNET, USHORT, void*, DWORD);

typedef struct HTTP_JOB {
    int used;
    char method[16];
    char url[1024];
    char token[256];
    char content_type[128];
    char save_path[512];
    void* body;
    size_t body_len;
    int user_tag;
} HTTP_JOB;

static struct {
    int inited;
    int stop;
    char base_url[512];

    CRITICAL_SECTION lock;
    HANDLE job_sem;
    HANDLE worker_thread;
    HTTP_JOB jobs[MAX_HTTP_JOBS];
    int j_head, j_tail;

    HTTP_EVENT evs[MAX_HTTP_EVS];
    int e_head, e_tail;

    CRITICAL_SECTION ws_lock;
    HINTERNET ws_session;
    HINTERNET ws_conn;
    HINTERNET ws_handle;
    HANDLE ws_thread;
    int ws_stop;
    int ws_connected;
    uint32_t ws_gen;
    char ws_url[1024];
    char ws_token[256];

    pfn_ws_upgrade_t fn_ws_upgrade;
    pfn_ws_send_t    fn_ws_send;
    pfn_ws_recv_t    fn_ws_recv;
    pfn_ws_close_t   fn_ws_close;
} hc;

static void resolve_ws_procs(void) {
    HMODULE hm = GetModuleHandleA("winhttp.dll");
    if (!hm) hm = LoadLibraryA("winhttp.dll");
    if (!hm) return;
    hc.fn_ws_upgrade = (pfn_ws_upgrade_t)(void*)GetProcAddress(hm, "WinHttpWebSocketCompleteUpgrade");
    hc.fn_ws_send    = (pfn_ws_send_t)(void*)GetProcAddress(hm, "WinHttpWebSocketSend");
    hc.fn_ws_recv    = (pfn_ws_recv_t)(void*)GetProcAddress(hm, "WinHttpWebSocketReceive");
    hc.fn_ws_close   = (pfn_ws_close_t)(void*)GetProcAddress(hm, "WinHttpWebSocketClose");
}

static void push_event(int type, int tag, int status, const char* data, size_t len, const char* save_path, const char* err) {
    int next;
    char* copy = NULL;
    if (data != NULL) {
        copy = (char*)malloc(len + 1u);
        if (copy != NULL) {
            if (len > 0) memcpy(copy, data, len);
            copy[len] = 0;
        }
    }
    EnterCriticalSection(&hc.lock);
    next = (hc.e_tail + 1) % MAX_HTTP_EVS;
    if (next == hc.e_head) {
        free(hc.evs[hc.e_head].data);
        hc.evs[hc.e_head].data = NULL;
        hc.e_head = (hc.e_head + 1) % MAX_HTTP_EVS;
    }
    memset(&hc.evs[hc.e_tail], 0, sizeof hc.evs[hc.e_tail]);
    hc.evs[hc.e_tail].type = type;
    hc.evs[hc.e_tail].user_tag = tag;
    hc.evs[hc.e_tail].status_code = status;
    hc.evs[hc.e_tail].data = copy;
    hc.evs[hc.e_tail].data_len = copy ? len : 0;
    if (save_path && save_path[0]) {
        snprintf(hc.evs[hc.e_tail].save_path, sizeof hc.evs[hc.e_tail].save_path, "%s", save_path);
    }
    if (err && err[0]) {
        snprintf(hc.evs[hc.e_tail].err, sizeof hc.evs[hc.e_tail].err, "%s", err);
    }
    hc.e_tail = next;
    LeaveCriticalSection(&hc.lock);
}

static void build_full_url(const char* path_or_url, char* out, size_t cap) {
    char base[512];
    EnterCriticalSection(&hc.lock);
    snprintf(base, sizeof base, "%s", hc.base_url);
    LeaveCriticalSection(&hc.lock);

    if (path_or_url == NULL || path_or_url[0] == 0) {
        snprintf(out, cap, "%s/", base);
        return;
    }
    if (strncmp(path_or_url, "http://", 7) == 0 || strncmp(path_or_url, "https://", 8) == 0 ||
        strncmp(path_or_url, "ws://", 5) == 0 || strncmp(path_or_url, "wss://", 6) == 0) {
        snprintf(out, cap, "%s", path_or_url);
        return;
    }
    if (path_or_url[0] == '/') {
        snprintf(out, cap, "%s%s", base, path_or_url);
    } else {
        snprintf(out, cap, "%s/%s", base, path_or_url);
    }
}

static int parse_url_parts(const char* url, int* out_secure, wchar_t* whost, size_t whost_cap,
                           INTERNET_PORT* out_port, wchar_t* wpath, size_t wpath_cap) {
    const char* p = url;
    const char* slash;
    const char* colon;
    char host[256];
    char path[768];
    size_t hlen;
    int port = 80;
    int sec = 0;

    if (strncmp(p, "https://", 8) == 0) { sec = 1; p += 8; port = 443; }
    else if (strncmp(p, "wss://", 6) == 0) { sec = 1; p += 6; port = 443; }
    else if (strncmp(p, "http://", 7) == 0) { sec = 0; p += 7; port = 80; }
    else if (strncmp(p, "ws://", 5) == 0) { sec = 0; p += 5; port = 80; }
    else return 0;

    slash = strchr(p, '/');
    if (slash == NULL) {
        hlen = strlen(p);
        snprintf(path, sizeof path, "/");
    } else {
        hlen = (size_t)(slash - p);
        snprintf(path, sizeof path, "%s", slash);
    }
    if (hlen == 0 || hlen >= sizeof host) return 0;
    memcpy(host, p, hlen);
    host[hlen] = 0;

    colon = strrchr(host, ':');
    if (colon != NULL) {
        port = atoi(colon + 1);
        host[colon - host] = 0;
    }
    if (port <= 0 || port > 65535) return 0;

    *out_secure = sec;
    *out_port = (INTERNET_PORT)port;
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, (int)whost_cap);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, (int)wpath_cap);
    return 1;
}

static void execute_rest_job(const HTTP_JOB* job) {
    int secure = 0;
    INTERNET_PORT port = 80;
    wchar_t whost[256], wpath[768], wmethod[16], whdrs[512];
    HINTERNET hSess = NULL, hConn = NULL, hReq = NULL;
    DWORD status = 0, st_sz = sizeof status;
    char hdr_buf[512];
    char* resp_buf = NULL;
    size_t resp_len = 0, resp_cap = 0;
    FILE* save_fp = NULL;
    char err[128] = "";

    if (!parse_url_parts(job->url, &secure, whost, 256, &port, wpath, 768)) {
        push_event(HTTP_EV_RESPONSE, job->user_tag, 0, NULL, 0, job->save_path, "invalid URL");
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, job->method, -1, wmethod, 16);

    hSess = WinHttpOpen(L"UMvSC2-Netplay/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { snprintf(err, sizeof err, "WinHttpOpen failed (%lu)", GetLastError()); goto done; }
    WinHttpSetTimeouts(hSess, 4000, 4000, 8000, 15000);

    hConn = WinHttpConnect(hSess, whost, port, 0);
    if (!hConn) { snprintf(err, sizeof err, "WinHttpConnect failed (%lu)", GetLastError()); goto done; }

    hReq = WinHttpOpenRequest(hConn, wmethod, wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                              secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) { snprintf(err, sizeof err, "WinHttpOpenRequest failed (%lu)", GetLastError()); goto done; }

    hdr_buf[0] = 0;
    if (job->token[0] != 0) {
        snprintf(hdr_buf + strlen(hdr_buf), sizeof hdr_buf - strlen(hdr_buf), "Authorization: Bearer %s\r\n", job->token);
    }
    if (job->content_type[0] != 0) {
        snprintf(hdr_buf + strlen(hdr_buf), sizeof hdr_buf - strlen(hdr_buf), "Content-Type: %s\r\n", job->content_type);
    }
    if (hdr_buf[0] != 0) {
        MultiByteToWideChar(CP_UTF8, 0, hdr_buf, -1, whdrs, 512);
        WinHttpAddRequestHeaders(hReq, whdrs, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            job->body_len > 0 ? job->body : WINHTTP_NO_REQUEST_DATA,
                            (DWORD)job->body_len, (DWORD)job->body_len, 0)) {
        snprintf(err, sizeof err, "WinHttpSendRequest failed (%lu)", GetLastError());
        goto done;
    }
    if (!WinHttpReceiveResponse(hReq, NULL)) {
        snprintf(err, sizeof err, "WinHttpReceiveResponse failed (%lu)", GetLastError());
        goto done;
    }

    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &st_sz, WINHTTP_NO_HEADER_INDEX);

    if (job->save_path[0] != 0 && status >= 200 && status < 300) {
        save_fp = fopen(job->save_path, "wb");
        if (!save_fp) {
            snprintf(err, sizeof err, "failed to open output file");
            goto done;
        }
    }

    for (;;) {
        DWORD avail = 0, got = 0;
        uint8_t chunk[8192];
        if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
        while (avail > 0) {
            DWORD want = avail > sizeof chunk ? (DWORD)sizeof chunk : avail;
            if (!WinHttpReadData(hReq, chunk, want, &got) || got == 0) {
                avail = 0;
                break;
            }
            avail -= got;
            if (save_fp) {
                fwrite(chunk, 1, got, save_fp);
            } else {
                if (resp_len + got + 1 > resp_cap) {
                    size_t ncap = resp_cap == 0 ? 4096u : resp_cap * 2u;
                    while (ncap < resp_len + got + 1u) ncap *= 2u;
                    if (ncap > (16u << 20)) break;
                    char* nb = (char*)realloc(resp_buf, ncap);
                    if (!nb) break;
                    resp_buf = nb;
                    resp_cap = ncap;
                }
                memcpy(resp_buf + resp_len, chunk, got);
                resp_len += got;
            }
        }
    }

done:
    if (save_fp) fclose(save_fp);
    if (hReq) WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    if (hSess) WinHttpCloseHandle(hSess);
    push_event(HTTP_EV_RESPONSE, job->user_tag, (int)status, resp_buf, resp_len, job->save_path, err);
    free(resp_buf);
}

static DWORD WINAPI rest_worker_proc(LPVOID param) {
    (void)param;
    for (;;) {
        HTTP_JOB job;
        int have = 0;
        WaitForSingleObject(hc.job_sem, INFINITE);
        EnterCriticalSection(&hc.lock);
        if (hc.stop) {
            LeaveCriticalSection(&hc.lock);
            break;
        }
        if (hc.j_head != hc.j_tail) {
            job = hc.jobs[hc.j_head];
            memset(&hc.jobs[hc.j_head], 0, sizeof job);
            hc.j_head = (hc.j_head + 1) % MAX_HTTP_JOBS;
            have = 1;
        }
        LeaveCriticalSection(&hc.lock);
        if (have) {
            execute_rest_job(&job);
            free(job.body);
        }
    }
    return 0;
}

static DWORD WINAPI ws_worker_proc(LPVOID param) {
    uint32_t my_gen = (uint32_t)(uintptr_t)param;
    char url[1024], token[256], hdr_buf[512];
    int secure = 0;
    INTERNET_PORT port = 80;
    wchar_t whost[256], wpath[768], whdrs[512];
    HINTERNET hSess = NULL, hConn = NULL, hReq = NULL, hWS = NULL;
    char err[128] = "";
    char msg_buf[65536];
    size_t msg_len = 0;

    EnterCriticalSection(&hc.ws_lock);
    snprintf(url, sizeof url, "%s", hc.ws_url);
    snprintf(token, sizeof token, "%s", hc.ws_token);
    LeaveCriticalSection(&hc.ws_lock);

    if (!hc.fn_ws_upgrade || !hc.fn_ws_recv || !hc.fn_ws_close) {
        snprintf(err, sizeof err, "WinHttpWebSocket API unavailable");
        goto closed;
    }
    if (!parse_url_parts(url, &secure, whost, 256, &port, wpath, 768)) {
        snprintf(err, sizeof err, "invalid WebSocket URL");
        goto closed;
    }

    hSess = WinHttpOpen(L"UMvSC2-NetplayWS/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { snprintf(err, sizeof err, "WS WinHttpOpen failed"); goto closed; }
    hConn = WinHttpConnect(hSess, whost, port, 0);
    if (!hConn) { snprintf(err, sizeof err, "WS WinHttpConnect failed"); goto closed; }
    hReq = WinHttpOpenRequest(hConn, L"GET", wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                              secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) { snprintf(err, sizeof err, "WS WinHttpOpenRequest failed"); goto closed; }

    WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
    if (token[0] != 0) {
        snprintf(hdr_buf, sizeof hdr_buf, "Authorization: Bearer %s\r\n", token);
        MultiByteToWideChar(CP_UTF8, 0, hdr_buf, -1, whdrs, 512);
        WinHttpAddRequestHeaders(hReq, whdrs, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, NULL)) {
        snprintf(err, sizeof err, "WS handshake failed (%lu)", GetLastError());
        goto closed;
    }
    hWS = hc.fn_ws_upgrade(hReq, 0);
    WinHttpCloseHandle(hReq);
    hReq = NULL;
    if (!hWS) {
        snprintf(err, sizeof err, "WS upgrade failed (%lu)", GetLastError());
        goto closed;
    }

    EnterCriticalSection(&hc.ws_lock);
    if (hc.ws_stop || hc.ws_gen != my_gen) {
        LeaveCriticalSection(&hc.ws_lock);
        goto closed;
    }
    hc.ws_session = hSess;
    hc.ws_conn = hConn;
    hc.ws_handle = hWS;
    hc.ws_connected = 1;
    LeaveCriticalSection(&hc.ws_lock);

    push_event(HTTP_EV_WS_OPEN, (int)my_gen, 101, NULL, 0, NULL, NULL);

    for (;;) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bt;
        DWORD rc;
        if (msg_len >= sizeof msg_buf - 1) msg_len = 0;
        rc = hc.fn_ws_recv(hWS, msg_buf + msg_len, (DWORD)(sizeof msg_buf - 1 - msg_len), &got, &bt);
        if (rc != ERROR_SUCCESS || bt == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            break;
        }
        msg_len += got;
        if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || bt == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            msg_buf[msg_len] = 0;
            push_event(HTTP_EV_WS_MESSAGE, (int)my_gen, 200, msg_buf, msg_len, NULL, NULL);
            msg_len = 0;
        }
    }

closed:
    EnterCriticalSection(&hc.ws_lock);
    if (hc.ws_gen == my_gen) {
        hc.ws_connected = 0;
        hc.ws_handle = NULL;
        hc.ws_conn = NULL;
        hc.ws_session = NULL;
    }
    LeaveCriticalSection(&hc.ws_lock);

    if (hWS) WinHttpCloseHandle(hWS);
    if (hReq) WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    if (hSess) WinHttpCloseHandle(hSess);
    push_event(HTTP_EV_WS_CLOSED, (int)my_gen, 0, NULL, 0, NULL, err);
    return 0;
}

int http_client_init(const char* base_url) {
    if (hc.inited) return 1;
    memset(&hc, 0, sizeof hc);
    InitializeCriticalSection(&hc.lock);
    InitializeCriticalSection(&hc.ws_lock);
    resolve_ws_procs();
    hc.job_sem = CreateSemaphoreA(NULL, 0, MAX_HTTP_JOBS, NULL);
    snprintf(hc.base_url, sizeof hc.base_url, "%s", (base_url && base_url[0]) ? base_url : "http://127.0.0.1:8080");
    hc.worker_thread = CreateThread(NULL, 0, rest_worker_proc, NULL, 0, NULL);
    hc.inited = 1;
    return 1;
}

void http_client_shutdown(void) {
    int i;
    if (!hc.inited) return;
    http_client_ws_close();
    EnterCriticalSection(&hc.lock);
    hc.stop = 1;
    LeaveCriticalSection(&hc.lock);
    ReleaseSemaphore(hc.job_sem, 1, NULL);
    if (hc.worker_thread) {
        WaitForSingleObject(hc.worker_thread, 3000);
        CloseHandle(hc.worker_thread);
    }
    CloseHandle(hc.job_sem);
    for (i = 0; i < MAX_HTTP_JOBS; i++) free(hc.jobs[i].body);
    for (i = 0; i < MAX_HTTP_EVS; i++) free(hc.evs[i].data);
    DeleteCriticalSection(&hc.ws_lock);
    DeleteCriticalSection(&hc.lock);
    memset(&hc, 0, sizeof hc);
}

void http_client_set_base_url(const char* base_url) {
    if (!hc.inited || !base_url || !base_url[0]) return;
    EnterCriticalSection(&hc.lock);
    snprintf(hc.base_url, sizeof hc.base_url, "%s", base_url);
    /* Trim trailing slash */
    {
        size_t n = strlen(hc.base_url);
        while (n > 0 && hc.base_url[n - 1] == '/') hc.base_url[--n] = 0;
    }
    LeaveCriticalSection(&hc.lock);
}

const char* http_client_get_base_url(void) {
    return hc.base_url;
}

int http_client_request(const char* method, const char* path_or_url, const char* bearer_token,
                        const void* body, size_t body_len, const char* content_type,
                        const char* save_path, int user_tag) {
    int next;
    void* bcopy = NULL;
    if (!hc.inited) return 0;
    if (body && body_len > 0) {
        bcopy = malloc(body_len);
        if (!bcopy) return 0;
        memcpy(bcopy, body, body_len);
    }
    EnterCriticalSection(&hc.lock);
    next = (hc.j_tail + 1) % MAX_HTTP_JOBS;
    if (next == hc.j_head) {
        LeaveCriticalSection(&hc.lock);
        free(bcopy);
        return 0;
    }
    memset(&hc.jobs[hc.j_tail], 0, sizeof hc.jobs[hc.j_tail]);
    hc.jobs[hc.j_tail].used = 1;
    snprintf(hc.jobs[hc.j_tail].method, sizeof hc.jobs[hc.j_tail].method, "%s", method ? method : "GET");
    build_full_url(path_or_url, hc.jobs[hc.j_tail].url, sizeof hc.jobs[hc.j_tail].url);
    if (bearer_token) snprintf(hc.jobs[hc.j_tail].token, sizeof hc.jobs[hc.j_tail].token, "%s", bearer_token);
    if (content_type) snprintf(hc.jobs[hc.j_tail].content_type, sizeof hc.jobs[hc.j_tail].content_type, "%s", content_type);
    if (save_path) snprintf(hc.jobs[hc.j_tail].save_path, sizeof hc.jobs[hc.j_tail].save_path, "%s", save_path);
    hc.jobs[hc.j_tail].body = bcopy;
    hc.jobs[hc.j_tail].body_len = body_len;
    hc.jobs[hc.j_tail].user_tag = user_tag;
    hc.j_tail = next;
    LeaveCriticalSection(&hc.lock);
    ReleaseSemaphore(hc.job_sem, 1, NULL);
    return 1;
}

int http_client_ws_connect(const char* path_or_url, const char* bearer_token) {
    uint32_t gen;
    if (!hc.inited) return 0;
    http_client_ws_close();
    EnterCriticalSection(&hc.ws_lock);
    hc.ws_stop = 0;
    hc.ws_gen++;
    gen = hc.ws_gen;
    build_full_url(path_or_url, hc.ws_url, sizeof hc.ws_url);
    if (bearer_token) snprintf(hc.ws_token, sizeof hc.ws_token, "%s", bearer_token);
    else hc.ws_token[0] = 0;
    hc.ws_thread = CreateThread(NULL, 0, ws_worker_proc, (LPVOID)(uintptr_t)gen, 0, NULL);
    LeaveCriticalSection(&hc.ws_lock);
    return hc.ws_thread != NULL;
}

int http_client_ws_send(const char* utf8_text) {
    HINTERNET hWS;
    DWORD rc;
    if (!hc.inited || !utf8_text || !hc.fn_ws_send) return 0;
    EnterCriticalSection(&hc.ws_lock);
    hWS = hc.ws_connected ? hc.ws_handle : NULL;
    if (hWS) {
        rc = hc.fn_ws_send(hWS, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                           (void*)utf8_text, (DWORD)strlen(utf8_text));
        LeaveCriticalSection(&hc.ws_lock);
        return rc == ERROR_SUCCESS;
    }
    LeaveCriticalSection(&hc.ws_lock);
    return 0;
}

void http_client_ws_close(void) {
    HANDLE th = NULL;
    HINTERNET hWS = NULL;
    if (!hc.inited) return;
    EnterCriticalSection(&hc.ws_lock);
    hc.ws_stop = 1;
    hc.ws_connected = 0;
    hWS = hc.ws_handle;
    hc.ws_handle = NULL;
    th = hc.ws_thread;
    hc.ws_thread = NULL;
    LeaveCriticalSection(&hc.ws_lock);
    if (hWS && hc.fn_ws_close) {
        hc.fn_ws_close(hWS, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(hWS);
    }
    if (th) {
        WaitForSingleObject(th, 2000);
        CloseHandle(th);
    }
}

int http_client_ws_is_connected(void) {
    int c;
    if (!hc.inited) return 0;
    EnterCriticalSection(&hc.ws_lock);
    c = hc.ws_connected;
    LeaveCriticalSection(&hc.ws_lock);
    return c;
}

int http_client_poll(HTTP_EVENT* out) {
    if (!hc.inited || !out) return 0;
    EnterCriticalSection(&hc.lock);
    if (hc.e_head == hc.e_tail) {
        LeaveCriticalSection(&hc.lock);
        return 0;
    }
    *out = hc.evs[hc.e_head];
    memset(&hc.evs[hc.e_head], 0, sizeof hc.evs[hc.e_head]);
    hc.e_head = (hc.e_head + 1) % MAX_HTTP_EVS;
    LeaveCriticalSection(&hc.lock);
    return 1;
}

void http_event_free(HTTP_EVENT* ev) {
    if (ev && ev->data) {
        free(ev->data);
        ev->data = NULL;
        ev->data_len = 0;
    }
}
