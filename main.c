#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <curl/curl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include <pthread.h>

#include "webp/encode.h"
#include "webp/decode.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#ifdef JPEG_STBI
    #define STB_IMAGE_WRITE_IMPLEMENTATION
    #include "stb_image_write.h"
#else
    #include "jpeglib.h"
#endif
#define PNG_SIMPLIFIED_WRITE_SUPPORTED
#include "png.h"
#include "avif.h"
#include "b64/b64.h"
#include "http.h"
#include "unavailable_b64.h"
#include "da.h"

#define PORT 3456
#define QUALITY 80
#define INITIAL_REPORTS 32
#define MAX_EXT_LEN 8

#ifndef MAX_CURL_THREADS
#define MAX_CURL_THREADS 10
#endif
#define CURL_IMG_TIMEOUT 5000
#define CURL_PAGE_TIMEOUT 15000

#define PRERENDER "http://localhost:3000/?t=5000&url="

#define WEBPAGE_BUF_SIZE 1024*1024
#define FILE_BUF_SIZE    20*1024*1024
#define DECODE_BUF_SIZE  200*1024*1024

static char webpage_buf[WEBPAGE_BUF_SIZE];
static char file_buf[FILE_BUF_SIZE];
static char decode_buf[DECODE_BUF_SIZE];
static char encode_buf[FILE_BUF_SIZE];

typedef struct {
    char *pixels;
    int w;
    int h;
    int n;
} ImgData;

typedef struct {
    char *content;
    int len;
} BufAndLen;

#define EXTENSION_COUNT 3
static char *extensions[EXTENSION_COUNT] = {"png", "webp", "jpeg"};

typedef struct {
    char *b64_encoded;
    size_t size;
} Converted;

typedef struct {
    char src[URL_MAX_LEN];
    size_t original_ext;
    Converted extensions[EXTENSION_COUNT];
} ImgReport;

int encode_webp(ImgData *img, char *out) {
    WebPPicture pic;
    WebPPictureInit(&pic);
    pic.width = img->w;
    pic.height = img->h;
    pic.writer = WebPMemoryWrite; 
    WebPMemoryWriter wrt;
    pic.custom_ptr = &wrt;
    WebPPictureAlloc(&pic);
    WebPMemoryWriterInit(&wrt);
    
    int import;
    if (img->n == 4) import = WebPPictureImportRGBA(&pic, (const uint8_t*)img->pixels, img->w*img->n);
    else if (img->n == 3) import = WebPPictureImportRGB(&pic, (const uint8_t*)img->pixels, img->w*img->n);
    else {
        dprintf(2, "ERROR: Don't know import function for %d components\n", img->n);
        WebPPictureFree(&pic);
        WebPMemoryWriterClear(&wrt);
        return 0;
    }
    if (import < 1) {
        dprintf(2, "ERROR: Importing pixels into webp failed with code %d\n", pic.error_code);
        WebPPictureFree(&pic);
        WebPMemoryWriterClear(&wrt);
        return 0;
    }
    
    WebPConfig config;
    WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, QUALITY);
    WebPEncode(&config, &pic);
    
    memcpy(out, wrt.mem, wrt.size);
    int size = wrt.size;
    
    WebPPictureFree(&pic);
    WebPMemoryWriterClear(&wrt);
    return size;
}

int stbi_decode(ImgData *img, char *in, int in_len) {
    int w, h, n;
    char *data = (char*)stbi_load_from_memory((unsigned char*)in, in_len, &w, &h, &n, 0);
    if (data == NULL) {
        dprintf(2, "ERROR: stbi_load_from_memory error: %s\n", stbi_failure_reason());
        return -1;
    }
    if (w*h*n > DECODE_BUF_SIZE) {
        dprintf(2, "ERROR: %d is big for decoding\n", w*h*n);
        free(data);
        return -1;
    } 
    memcpy(img->pixels, data, w*h*n);
    img->w = w;
    img->h = h;
    img->n = n;
    free(data);
    return 0;
}

size_t curl_callback(char *chunk, size_t _one, size_t chunk_size, BufAndLen *data) {
    memcpy(data->content+data->len, chunk, chunk_size);
    data->len += chunk_size;
    return chunk_size;
}

int curl(BufAndLen *buf, char *url, int timeout) {
    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
    char error[1024];
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != 0) {
        dprintf(2, "ERROR: Could not get %s: %s (code %d)\n", url, error, res);
        buf->len = 0;
    }
    return buf->len;
}

void stbi_encode_func(void *context, void *data, int size) {
    BufAndLen *buf = (BufAndLen*)context;
    memcpy(buf->content+buf->len, data, size);
    buf->len += size;
}

bool mkdir_p(char *path) {
    char dir[128] = {0};
    int dir_len = 0;
    int skip = 1;
    int len = strlen(path);
    bool success = TRUE;
    for (int i = 0; i < len; i++) {
        dir[dir_len++] = path[i];
        if (path[i] == '/') {
            if (skip) {
                skip = 0;
                continue;
            }
            dir[dir_len] = '\0';
            if (mkdir(dir, 0777) < 0) {
                // dprintf(2, "ERROR: mkdir %s: %s\n", dir, strerror(errno));
                // return 0;
                success = FALSE;
            } else success = TRUE;
        }
    }
    return success;
}

bool decode(ImgData *img_data, char *in_format, BufAndLen img) {
    if (strcmp(in_format, "png") == 0 || strcmp(in_format, "jpeg") == 0 || strcmp(in_format, "jpg") == 0) {
        if (stbi_decode(img_data, img.content, img.len) < 0) {
            dprintf(2, "ERROR: Decoding failed for some reason\n");
            return FALSE;
        }
    } else if (strcmp(in_format, "webp") == 0) {
        unsigned char *webp = WebPDecodeRGB((unsigned char *)img.content, img.len, &img_data->w, &img_data->h);
        if (webp == NULL) {
            dprintf(2, "ERROR: WebPDecodeRGB failed\n");
            return FALSE;
        } else {
            if (img_data->w*img_data->h > DECODE_BUF_SIZE) {
                dprintf(2, "ERROR: %d is big for decoding\n", img_data->w*img_data->h);
                return FALSE;
            } else {
                memcpy(img_data->pixels, webp, img_data->w*img_data->h);
                img_data->n = 3;
            }
        }
        if (webp != NULL) free(webp);
    } else if (strcmp(in_format, "avif") == 0) {
        dprintf(2, "ERROR: TODO: decode avif\n");
        return FALSE;
    } else {
        dprintf(2, "ERROR: Don't know how to decode %s\n", in_format);
        return FALSE;
    }
    return img_data->w > 0 && img_data->h > 0;
}
    
size_t encode(ImgData *img_data, char *out_format, char *encode_buf) {
    size_t encoded_size = 0;
    if (strcmp(out_format, "png") == 0) {
        png_image png = {0};
        png.version = PNG_IMAGE_VERSION;
        png.opaque = NULL;
        png.width = img_data->w;
        png.height = img_data->h;
        png.format = PNG_FORMAT_RGB;
        png.colormap_entries = 0;
        
        png_alloc_size_t size;
        png_image_write_get_memory_size(png, size, 0, img_data->pixels, 0, NULL);
        png_image_write_to_memory(&png, encode_buf, &size, 0, img_data->pixels, 0, NULL);
        encoded_size = (int)size;
    } else if (strcmp(out_format, "jpeg") == 0) {
#ifdef JPEG_STBI
        BufAndLen buf = {0};
        buf.content = encode_buf;
        int result = stbi_write_jpg_to_func(&stbi_encode_func, &buf, img_data->w,
            img_data->h, img_data->n, img_data->pixels, QUALITY);
        if (result < 1) {
            dprintf(2, "ERROR: stbi_write_jpg_to_func failed\n");
        }
        encoded_size = buf.len;
#else
        struct jpeg_error_mgr jerr;
        struct jpeg_compress_struct cinfo;
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_compress(&cinfo);
        cinfo.image_width = img_data->w;
        cinfo.image_height = img_data->h;
        cinfo.input_components = img_data->n;
        if (img_data->n == 4) cinfo.in_color_space = JCS_EXT_RGBA;
        else if (img_data->n == 3) cinfo.in_color_space = JCS_RGB;
        else {
            dprintf(2, "ERROR: don't know JPEG's in_color_space for %d components\n", img_data->n);
            goto done;
        }
        unsigned long jpeg_size = 0;
        unsigned char *jpeg_buf = NULL;
        jpeg_mem_dest(&cinfo, &jpeg_buf, &jpeg_size);
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, QUALITY, TRUE);
        jpeg_start_compress(&cinfo, TRUE);
        int row_stride = img_data->w * img_data->n;
        JSAMPROW row_pointer[1]; 
        while (cinfo.next_scanline < cinfo.image_height) {
          /* jpeg_write_scanlines expects an array of pointers to scanlines.
           * Here the array is only one element long, but you could pass
           * more than one scanline at a time if that's more convenient.
           */
            row_pointer[0] = (unsigned char*)&img_data->pixels[cinfo.next_scanline * row_stride];
            jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
        encoded_size = (int)jpeg_size;
        memcpy(encode_buf, jpeg_buf, encoded_size);
        free(jpeg_buf);
#endif
    } else if (strcmp(out_format, "webp") == 0) {
        encoded_size = encode_webp(img_data, encode_buf);
    } else if (strcmp(out_format, "avif") == 0) {
        dprintf(2, "ERROR: TODO: encode avif\n");
        goto done;
    } else {
        dprintf(2, "ERROR: Don't know how to encode %s\n", out_format);
        goto done;
    }

done:
    return encoded_size;
}

int next_img(char *src_buf, BufAndLen page, size_t max_src_len) {
    int i = 0;
    for (; strncmp(page.content+i, "<img ", 5) != 0; i++) {
        if (i == page.len - 5) {
            return 0;
        }
    }
    i += 5;
    for (; strncmp(page.content+i, "src=\"", 5) != 0; i++) {
        if (i == page.len - 5 || page.content[i] == '>') {
            return 0;
        }
    }
    i += 5;
    int src_len = 0;
    for (; i < page.len && page.content[i] != '"'; i++) {
        if (strncmp(page.content+i, "amp;", 4) == 0) {
            i += 3;
            continue;
        }
        if (src_len < max_src_len) {
            src_buf[src_len++] = page.content[i];
        }
    }
    if (src_len > max_src_len) src_len = 0;
    src_buf[src_len] = '\0';
    return i;
}

// TODO check magic bytes or shit for extension recognition
int guess_ext(char *src, size_t src_len, char *ext_buf) {
    int ext_i = src_len-1;
    
    int has_ext = 1;
    for (; src[ext_i-1] != '.'; ext_i--) {
        if (ext_i == 1 || src[ext_i-1] == '/') {
            has_ext = 0;
            break;
        }
    }

    if (!has_ext) {
        return 0;
    }
    size_t ext_len = 0;
    for (; ext_i < src_len; ext_i++) ext_buf[ext_len++] = src[ext_i];
    ext_buf[ext_len] = '\0';
    return ext_len;
}

bool generate_img_report(ImgReport *report, BufAndLen img, char *full_src) {
    char ext[MAX_EXT_LEN];
    char without_query[256];
    if (!guess_ext(without_query, http_trim_query(full_src, without_query), ext)) {
        printf("INFO: Could not guess extension of %s\n", full_src);
        return FALSE;
    }
    
    ImgData decoded;
    decoded.pixels = decode_buf;
    if (!decode(&decoded, ext, img)) {
        return FALSE;
    }
    
    strcpy(report->src, full_src);
    for (int i = 0; i < EXTENSION_COUNT; i++) {
        char *out_ext = extensions[i];
        if (strcmp(ext, out_ext) == 0) {
            report->extensions[i].size = img.len;
            report->original_ext = i;
            continue;
        }
    
        size_t encoded_size = encode(&decoded, out_ext, encode_buf);
        report->extensions[i].size = encoded_size;
        if (encoded_size > 0) {
            report->extensions[i].b64_encoded = b64_encode((unsigned char*)encode_buf, encoded_size);
        } else {
            dprintf(2, "ERROR: Could not convert %s to %s\n", full_src, out_ext);
        }
    }
    return TRUE;
}

void get_full_src(char *full_src, char *src, char *host) {
    strcpy(full_src, src);
    if (!has_protocol_prefix(src)) {
        if (src[0] != '/') {
            strcpy(full_src+strlen(host)+1, src);
            full_src[strlen(host)] = '/';
        } else {
            strcpy(full_src+strlen(host), src);
        }
        strncpy(full_src, host, strlen(host));
    }
}

typedef struct {
    pthread_t pthread;
    char src[URL_MAX_LEN];
    BufAndLen buf;
} CurlThreadArg;

void *img_report_pthread(void *void_arg) {
    CurlThreadArg *arg = (CurlThreadArg*)void_arg;
    arg->buf.content = malloc(FILE_BUF_SIZE);
    curl(&arg->buf, arg->src, CURL_IMG_TIMEOUT);
    return void_arg;
}

void join_threads(DA *reports, DA pthread_da) {
    for (int i = 0; i < pthread_da.len; i++) {
        CurlThreadArg *pthread = (CurlThreadArg*)da_at(pthread_da, i);
        pthread_join(pthread->pthread, NULL);
        if (pthread->buf.len < 1) {
            continue;
        }
    
        ImgReport *report = da_at(*reports, reports->len);
        bool success = generate_img_report(report, pthread->buf, pthread->src);
        free(pthread->buf.content);
        if (!success) continue;
        da_append(reports, report);
    }
}

bool generate_page_reports(char *input_url, DA *reports, int use_prerender) {
    char host[32] = {0};
    char *protocol;
    if (has_http_prefix(input_url)) protocol = "http://";
    else protocol = "https://";
    strcpy(host, protocol);
    for (int i = strlen(protocol); i < strlen(input_url) && input_url[i] != '/'; i++) {
        host[i] = input_url[i];
    }
    
    char request_url[256] = {0};
    if (use_prerender){
        printf("INFO: Using prerender for %s\n", input_url);
        strcpy(request_url, PRERENDER);
    }
    strcpy(request_url+strlen(request_url), input_url);
    
    BufAndLen page = {0};
    page.content = webpage_buf;
    if (curl(&page, request_url, CURL_PAGE_TIMEOUT) < 1) {
        dprintf(2, "ERROR: Could not get page %s\n", request_url);
        return FALSE;
    }
    page.content[page.len] = 0;
    char src[URL_MAX_LEN];
    int offset = 0;
    
    DA pthread_da;
    da_alloc(&pthread_da, INITIAL_REPORTS, sizeof(CurlThreadArg));
    
    for (int iter = 0; iter < 1000; iter++) {
        offset = next_img(src, page, URL_MAX_LEN);
        page.content += offset;
        page.len -= offset;
        if (offset == 0) {
            break;
        }

        if (strncmp(src, "data:", 5) == 0) {
            continue;
        }
        char full_src[256];
        get_full_src(full_src, src, host);
        printf("INFO: Processing %s\n", full_src);
        
        CurlThreadArg *pthread = da_at(pthread_da, pthread_da.len);
        strcpy(pthread->src, full_src);
        if (pthread_create(&pthread->pthread, NULL, img_report_pthread, (void*)pthread) != 0) {
            dprintf(2, "ERROR: Could not create pthread\n");
            exit(1);
        }
        da_append(&pthread_da, pthread);
        
        if (pthread_da.len == MAX_CURL_THREADS) {
            join_threads(reports, pthread_da);
            da_reset(&pthread_da);
        }
    }
    join_threads(reports, pthread_da);
    da_free(&pthread_da);
    return TRUE;
}

int get_bytes_str(size_t bytes, char *buf) {
    if (bytes < 1024) {
        return sprintf(buf, "%zu B", bytes);
    }
    if (bytes >= 1024 && bytes <= 1024*1024) {
        return sprintf(buf, "%.2f KB", bytes/1024.f);
    }
    return sprintf(buf, "%.2f MB", bytes/1024.f/1024);
}

bool serve_report(HttpReq *request, HttpResp *response, int clientfd) {
    char input_url[128];
    if (!http_get_query_param(request, "page", input_url)) {
        dprintf(2, "ERROR: No `page` provided\n");
        http_not_found(clientfd);
        return FALSE;
    }
    char use_prerender_req[2];
    http_get_query_param(request, "prerender", use_prerender_req);
    int use_prerender = 0;
    if (strcmp(use_prerender_req, "1") == 0) use_prerender = 1;
    printf("INFO: Will try to handle %s\n", input_url);
    if (!has_protocol_prefix(input_url)) {
        http_not_found(clientfd);
        dprintf(2, "ERROR: %s is not a web page address\n", input_url);
        return FALSE;
    }
    
    DA reports_da;
    ImgReport *reports = da_alloc(&reports_da, INITIAL_REPORTS, sizeof(ImgReport));
    bool success = generate_page_reports(input_url, &reports_da, use_prerender);
    if (!success) {
        return FALSE;
    }
    printf("INFO: Generated %zu image reports for %s\n", reports_da.len, input_url);
    
    response->body.len = sprintf(response->body.ptr, "<table>");
    for (int ext = 0; ext < EXTENSION_COUNT; ext++) {
        size_t total = 0;
        for (size_t i = 0; i < reports_da.len; i++) {
            total += reports[i].extensions[ext].size;
        }
        char bytes_str[32];
        get_bytes_str(total, bytes_str);
        http_body_appendf(&response->body, "<th><b>%s (total - %s)</b><br></th>", extensions[ext], bytes_str);
    }
    for (size_t i = 0; i < reports_da.len; i++) {
        http_body_appendf(&response->body, "<tr>");
        int success = 0;
        for (int ext = 0; ext < EXTENSION_COUNT; ext++) {
            if (reports[i].extensions[ext].size > 0 && reports[i].original_ext != ext) success = 1;
        }
        if (!success) continue;
        for (int ext = 0; ext < EXTENSION_COUNT; ext++) {
            char *img_style = "";
            char *size_suf = "";
            if (ext == reports[i].original_ext) {
                img_style = "border: 5px solid green";
                size_suf = " (original)";
            }
            
            http_body_appendf(&response->body, "<td><a target=\"_blank\" href=\"", img_style);
            if (reports[i].original_ext == ext) http_body_appendf(&response->body, "%s", reports[i].src);
            else http_body_appendf(&response->body, "http://localhost:%d/convert/%s/%s", PORT, extensions[ext], reports[i].src);
            
            http_body_appendf(&response->body, "\"><img style=\"%s\" src=\"", img_style);
            if (reports[i].original_ext == ext) {
                http_body_appendf(&response->body, "%s", reports[i].src);
            } else {
                char *b64;
                char *report_ext;
                if (reports[i].extensions[ext].size) {
                    b64 = reports[i].extensions[ext].b64_encoded;
                    report_ext = extensions[ext];
                } else {
                    b64 = UNAVAILABLE_B64;
                    report_ext = UNAVAILABLE_B64_EXT;
                }
                http_body_appendf(&response->body, "data:image/%s;base64,%s", report_ext, b64);
            }
            char bytes_str[32];
            get_bytes_str(reports[i].extensions[ext].size, bytes_str);
            http_body_appendf(&response->body, "\" height=\"200px\"></a><br>%s%s<br></td>", bytes_str, size_suf);
        }
        http_body_appendf(&response->body, "</tr>");
    }
    http_body_appendf(&response->body, "</table>");
  
    da_free(&reports_da);
    strcpy(response->headers[0].k, "Access-Control-Allow-Origin");
    strcpy(response->headers[0].v, "*");
    response->headers_count++;
    
    http_respond(clientfd, 200, response);
    return TRUE;
}

bool serve_static_file(HttpReq *request, HttpResp *response, int clientfd) {
    char *path = &request->path[1];
    if (strlen(path) == 0) {
        path = "index.html";
    }
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) return FALSE;
    
    int size = read(fd, file_buf, FILE_BUF_SIZE);
    close(fd);
    if (size < 0) return FALSE;
    
    response->body.ptr = file_buf;
    response->body.len = size;
    
    http_respond(clientfd, 200, response);
    return TRUE;
}

#define CONVERT_PATH "/convert/"

size_t encode_by_url(char *src, char *ext, char *encode_buf) {
    char in_ext[MAX_EXT_LEN];
    char without_query[256];
    if (!guess_ext(without_query, http_trim_query(src, without_query), in_ext)) {
        printf("INFO: Could not guess extension of %s\n", src);
        return 0;
    }
    BufAndLen img = {0};
    img.content = file_buf;
    printf("INFO: Getting %s\n", src);
    if (curl(&img, src, CURL_IMG_TIMEOUT) < 1) return 0;
    printf("INFO: Encoding %s\n", src);
    ImgData decoded;
    decoded.pixels = decode_buf;
    if (!decode(&decoded, in_ext, img)) return 0;
    return encode(&decoded, ext, encode_buf);
}

bool serve_convert(HttpReq *request, HttpResp *response, int clientfd) {
    char ext[MAX_EXT_LEN];
    char *url_ext = &request->url[strlen(CONVERT_PATH)];
    int path_len = strlen(url_ext);
    int i = 0;
    for (;i < MAX_EXT_LEN && url_ext[i] != '/' && i < path_len; i++) {
        ext[i] = url_ext[i];
    }
    ext[i++] = 0;
    char *src = &url_ext[i];
    size_t size = encode_by_url(src, ext, encode_buf);
    if (size < 1) return FALSE;
    strcpy(response->headers[response->headers_count++].k, "Content-Type");
    strcpy(response->headers[response->headers_count].v, "image/");
    strcpy(response->headers[response->headers_count].v, ext);
    response->body.ptr = encode_buf;
    response->body.len = size;
    http_respond(clientfd, 200, response);
    return TRUE;
}

// TODO close socket on SIGINT
int serve() {
    int sockfd = http_server(PORT);
    if (sockfd < 0) {
        dprintf(2, "Could not create server\n");
        return 1;
    }
    printf("INFO: Started HTTP server at http://localhost:%d\n", PORT);
    HttpReq request = {0};
    HttpResp response = {0};
    response.body.ptr = malloc(FILE_BUF_SIZE);
    response.body.cap = FILE_BUF_SIZE;
    for (;;) {
        int clientfd = http_next_request(sockfd, &request);
        if (clientfd < 0) continue;
        printf("INFO: %s %s\n", request.method, request.path);
        if (strcmp(request.path, "/report") == 0) {
            serve_report(&request, &response, clientfd);
        } else if (strncmp(request.path, CONVERT_PATH, strlen(CONVERT_PATH)) == 0 &&
            strlen(&request.path[strlen(CONVERT_PATH)]) > 0) {
            if (!serve_convert(&request, &response, clientfd)) http_not_found(clientfd);
        } else {
            if (!serve_static_file(&request, &response, clientfd)) http_not_found(clientfd);
        }
        close(clientfd);
    }
    free(response.body.ptr);
    return 0;
}

size_t replace_slash_with_0(char *str, char *dst) {
    int len = strlen(str);
    size_t parts = 0;
    for (int i = 0; i < len; i++) {
        if (str[i] == '/') {
            parts++;
            dst[i] = '\0';
            continue;
        }
        dst[i] = str[i];
    }
    return parts;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (argc != 3) {
            dprintf(2, "ERROR: %s <in_url> <out_ext>\n", argv[0]);
            return 1;
        }
        
        char *full_src = argv[1];
       
        char *out_ext = argv[2];
        size_t encoded_size = encode_by_url(full_src, out_ext, encode_buf);
        
        char img_host[128];
        http_get_host(img_host, full_src);
        
        char img_path[256];
        http_trim_query(full_src+strlen(img_host), img_path);
        
        char chopped_parts[256];
        size_t parts = replace_slash_with_0(img_path+1, chopped_parts);
        int filename_start = 0;
        for (size_t i = 0; i < parts; i++) {
            filename_start += strlen(chopped_parts+filename_start) + 1;
        }

        char out_file_path[256];
        strcpy(out_file_path, "./result/");
        strcpy(out_file_path + strlen(out_file_path), chopped_parts+filename_start);
        strcpy(out_file_path + strlen(out_file_path), ".");
        strcpy(out_file_path + strlen(out_file_path), out_ext);
    
        printf("INFO: Encoding %s into %s\n", full_src, out_file_path);
        if (encoded_size > 0) {
            int mkdir_ok = mkdir_p(out_file_path);
            if (!mkdir_ok) {
                dprintf(2, "ERROR: Could not mkdir %s\n", out_file_path);
                // return 1;
            }
            int out_fd = open(out_file_path, O_CREAT|O_WRONLY, 0644);
            if (write(out_fd, encode_buf, encoded_size) < 0) {
                dprintf(2, "ERROR: Could not write to %s: %s\n", out_file_path, strerror(errno));
            }
            close(out_fd);
        } else {
            dprintf(2, "ERROR: Could not encode image\n");
            return 1;
        }
        char bytes_str[32];
        get_bytes_str((size_t)encoded_size, bytes_str);
        printf("INFO: Created file %s of size %s\n", out_file_path, bytes_str);
        return 0;
    }
    return serve();
}
