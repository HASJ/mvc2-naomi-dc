/*
 * http_client.h  Non-blocking background WinHTTP REST & WebSocket client for SDL netplay UI
 */

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

enum {
    HTTP_EV_NONE = 0,
    HTTP_EV_RESPONSE = 1,
    HTTP_EV_WS_OPEN = 2,
    HTTP_EV_WS_MESSAGE = 3,
    HTTP_EV_WS_CLOSED = 4
};

typedef struct HTTP_EVENT {
    int   type;
    int   user_tag;
    int   status_code;
    char* data;        /* NUL-terminated heap buffer; free via http_event_free() */
    size_t data_len;
    char  save_path[512];
    char  err[128];
} HTTP_EVENT;

int  http_client_init(const char* base_url);
void http_client_shutdown(void);
void http_client_set_base_url(const char* base_url);
const char* http_client_get_base_url(void);

/* Enqueue async HTTP request (GET, POST, PATCH). If save_path != NULL, writes body directly to disk. */
int  http_client_request(const char* method, const char* path_or_url, const char* bearer_token,
                         const void* body, size_t body_len, const char* content_type,
                         const char* save_path, int user_tag);

/* Async WebSocket connection backed by WinHttpWebSocket* (single active WS room/queue connection). */
int  http_client_ws_connect(const char* path_or_url, const char* bearer_token);
int  http_client_ws_send(const char* utf8_text);
void http_client_ws_close(void);
int  http_client_ws_is_connected(void);

/* Non-blocking poll on SDL main thread; returns 1 and fills *out if an event is ready. */
int  http_client_poll(HTTP_EVENT* out);
void http_event_free(HTTP_EVENT* ev);

#endif
