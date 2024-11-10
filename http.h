#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>
#include <strings.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>

#define LISTEN_BACKLOG 32

#define HTTP_BAD_REQUEST "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n\r\n"
#define HTTP_NOT_FOUND "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n\r\n"
#define URL_MAX_LEN 1024

typedef struct KV {
    char k[64];
    char v[64];
} KV;

typedef KV HttpHeader;

typedef struct {
    size_t len;
    size_t cap;
    char *ptr;
} HttpBody;

typedef struct {
    HttpBody body;
    HttpHeader headers[32];
    size_t headers_count;
} HttpResp;

typedef struct {
    char method[8];
    char path[URL_MAX_LEN];
    char url[URL_MAX_LEN];
    int clientfd;
    KV query[32];
    size_t query_count;
} HttpReq;

size_t http_body_appendf(HttpBody *body, char *fmt, ...);

int http_server(uint16_t port);
int http_next_request(int sockfd, HttpReq *request);
int http_respond(int clientfd, int status, HttpResp *response);

int http_not_found(int clientfd);

size_t http_trim_query(char *url, char *path);
size_t http_parse_url_and_query(char *url, char *path, KV *query_buf);
int    http_get_query_param(HttpReq *request, char *param, char *buf);
size_t http_get_host(char *buf, char *url);

int has_http_prefix(char *str)     { return strncmp("http://", str, 7) == 0; }
int has_https_prefix(char *str)    { return strncmp("https://", str, 8) == 0; }
int has_protocol_prefix(char *str) { return has_http_prefix(str) || has_https_prefix(str); }

HttpBody *http_body_realloc(HttpBody *body, size_t size) {
    body->cap = body->cap + size*2;
    body->ptr = realloc(body->ptr, body->cap);
    return body;
}

size_t http_body_appendf(HttpBody *body, char *fmt, ...) {
    va_list va;
	va_start(va, fmt);
	size_t space = body->cap - body->len;
	size_t size = vsnprintf(&body->ptr[body->len], space, fmt, va);
	if (size >= space) {
    	http_body_realloc(body, size);
    	vsprintf(&body->ptr[body->len], fmt, va);
	}
	va_end(va);
	body->len += size;
	return size;
}

size_t http_get_host(char *buf, char *url) {
    size_t url_len = strlen(url);
    int slashes = 0;
    int buf_i = 0;
    for (int i = 0; i < url_len && (url[i] != '/' || slashes++ < 2); i++) {
        buf[buf_i++] = url[i];
    }
    buf[buf_i] = '\0';
    return buf_i;
}

int http_get_query_param(HttpReq *request, char *param, char *buf) {
    for (size_t i = 0; i < request->query_count; i++) {
        if (strcmp(request->query[i].k, param) == 0) {
            strcpy(buf, request->query[i].v);
            return 1;
        }
    }
    return 0;
}

int http_server(uint16_t port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd < 0) {
        dprintf(2, "Could not create socket: %s\n", strerror(errno));
        return -1;
    }
    int one = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        dprintf(2, "Could not set SO_REUSEADDR: %s\n", strerror(errno));
        return -1;
    }
    struct sockaddr_in sockaddr = {
        .sin_family=AF_INET,
       	.sin_port=htons(port),
       	.sin_addr={.s_addr=INADDR_ANY},
    };
    if (bind(sockfd, (struct sockaddr*)&sockaddr, sizeof(sockaddr)) < 0) {
        dprintf(2, "Could not bind socket to port %d: %s\n", port, strerror(errno));
        return -1;
    }
    if (listen(sockfd, LISTEN_BACKLOG) < 0) {
        dprintf(2, "Could listen at socket: %s\n", strerror(errno));
        return -1;
    }
    return sockfd;
}

int http_not_found(int clientfd) {
    return write(clientfd, HTTP_NOT_FOUND, strlen(HTTP_NOT_FOUND));
}

int http_respond(int clientfd, int status, HttpResp *response) {
    char buf[sizeof(HttpResp)];
    int size = sprintf(buf, "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\n", status, "TODO", response->body.len);
    for (size_t i = 0; i < response->headers_count; i++) {
        size += sprintf(buf+size, "%s: %s\r\n", response->headers[i].k, response->headers[i].v);
    }
    memcpy(buf+size, "\r\n", 2);
    size += 2;
    if (response->body.len == 0) {
        memcpy(buf+size, "\r\n", 2);
        size += 2;
        return write(clientfd, buf, size);
    }
    write(clientfd, buf, size);
    size += write(clientfd, response->body.ptr, response->body.len);
    size += write(clientfd, "\r\n", 2);
    return size;
}

size_t http_trim_query(char *url, char *trimmed) {
    size_t i = 0;
    size_t url_len = strlen(url);
    for (; i < url_len && url[i] != '?'; i++) trimmed[i] = url[i];
    trimmed[i] = '\0';
    return i;
}

size_t http_parse_url_and_query(char *url, char *without_query, KV *query_buf) {
    size_t url_len = strlen(url);
    size_t query_i = http_trim_query(url, without_query);
    query_i++;
    size_t query_count = 0;
    for (; query_i < url_len; query_count++) {
        size_t k = 0;
        for (; query_i < url_len && url[query_i] != '='; query_i++) {
            (query_buf+query_count)->k[k++] = url[query_i];
        }
        (query_buf+query_count)->k[k] = '\0';
        query_i++;
        size_t v = 0;
        for (; query_i < url_len && url[query_i] != '&'; query_i++) {
            (query_buf+query_count)->v[v++] = url[query_i];
        }
        (query_buf+query_count)->v[v] = '\0';
        query_i++;
    }
    return query_count;
}

int http_next_request(int sockfd, HttpReq *request) {
    request->clientfd = accept(sockfd, NULL, NULL);
    if (request->clientfd < 0) {
        dprintf(2, "Could not accept client: %s\n", strerror(errno));
        return -1;
    }
    char buf[sizeof(HttpReq)];
    
    int read_bytes = read(request->clientfd, buf, sizeof(HttpReq));
    int req_len = read_bytes;
    
    int cur = 0;
    for (; cur < req_len && buf[cur] != ' '; cur++) {
        request->method[cur] = buf[cur];
    }
    request->method[cur] = '\0';
    int path_start = ++cur;
    for (; cur < req_len && buf[cur] != ' ' && cur-path_start < URL_MAX_LEN; cur++) {
        request->url[cur-path_start] = buf[cur];
    }
    request->url[cur-path_start] = '\0';
    
    request->query_count = http_parse_url_and_query(request->url, request->path, request->query);
    return request->clientfd;
}
