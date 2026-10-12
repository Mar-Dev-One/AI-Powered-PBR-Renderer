#ifdef _WIN32
    #define _CRT_RAND_S   // rand_s(): the CRT's interface to the OS random generator
#endif

#include "CommandServer.h"

#include "../Commands/JsonRpc.h"
#include "../Core/Encoding.h"
#include "../Core/FileIO.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <process.h>

    typedef SOCKET socket_t;
    #define INVALID_SOCK      INVALID_SOCKET
    #define close_socket      closesocket
    #define socket_error()    WSAGetLastError()
    #define WOULD_BLOCK(e)    ((e) == WSAEWOULDBLOCK)
    #define current_pid()     _getpid()
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <sys/socket.h>
    #include <unistd.h>

    typedef int socket_t;
    #define INVALID_SOCK      (-1)
    #define close_socket      close
    #define socket_error()    errno
    #define WOULD_BLOCK(e)    ((e) == EAGAIN || (e) == EWOULDBLOCK)
    #define current_pid()     getpid()
#endif

#define HANDSHAKE_MAX (16u * 1024u)   // larger HTTP headers are refused

// --- Growable byte buffer ----------------------------------------------------------

typedef struct byte_buffer
{
    unsigned char* data;
    size_t         length;
    size_t         capacity;
} byte_buffer;

static void
buffer_append(byte_buffer* b, const void* data, size_t length)
{
    if (b->length + length > b->capacity)
    {
        size_t capacity = b->capacity ? b->capacity : 4096;
        while (capacity < b->length + length)
            capacity *= 2;

        unsigned char* grown = realloc(b->data, capacity);
        if (!grown)
            FATAL("server: out of memory");
        b->data     = grown;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, data, length);
    b->length += length;
}

// Drops the first `count` bytes.
static void
buffer_consume(byte_buffer* b, size_t count)
{
    memmove(b->data, b->data + count, b->length - count);
    b->length -= count;
}

static void
buffer_free(byte_buffer* b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

// --- SHA-1 (only for the WebSocket handshake) ------------------------------------------

typedef struct sha1_state
{
    uint32        h[5];
    unsigned char block[64];
    uint32        block_length;
    uint64        total_length;
} sha1_state;

static uint32 rotl(uint32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void
sha1_block(sha1_state* s, const unsigned char* block)
{
    uint32 w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32)block[i * 4] << 24) | ((uint32)block[i * 4 + 1] << 16) | ((uint32)block[i * 4 + 2] << 8) | block[i * 4 + 3];
    for (int i = 16; i < 80; ++i)
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; ++i)
    {
        uint32 f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }

        uint32 t = rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void
sha1(const unsigned char* data, size_t length, unsigned char out[20])
{
    sha1_state s = { { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 }, { 0 }, 0, 0 };

    for (size_t i = 0; i < length; ++i)
    {
        s.block[s.block_length++] = data[i];
        if (s.block_length == 64)
        {
            sha1_block(&s, s.block);
            s.block_length = 0;
        }
    }
    s.total_length = (uint64)length * 8;

    // Padding: 0x80, zeros, then the bit length big-endian.
    s.block[s.block_length++] = 0x80;
    if (s.block_length > 56)
    {
        while (s.block_length < 64)
            s.block[s.block_length++] = 0;
        sha1_block(&s, s.block);
        s.block_length = 0;
    }
    while (s.block_length < 56)
        s.block[s.block_length++] = 0;
    for (int i = 7; i >= 0; --i)
        s.block[s.block_length++] = (unsigned char)(s.total_length >> (i * 8));
    sha1_block(&s, s.block);

    for (int i = 0; i < 5; ++i)
    {
        out[i * 4 + 0] = (unsigned char)(s.h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s.h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s.h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s.h[i]);
    }
}

// --- Server state -------------------------------------------------------------------

typedef struct ws_client
{
    socket_t    sock;
    b8          in_use;
    b8          upgraded;     // handshake done: speaking WebSocket frames
    b8          closing;      // close once `out` has drained
    byte_buffer in;
    byte_buffer out;
    byte_buffer message;      // a fragmented text message being reassembled
    b8          in_message;
    char        role[SERVER_ROLE_MAX];   // claimed with server.hello; "" = none
    char        name[64];                // what the client called itself in server.hello
} ws_client;

struct command_server
{
    socket_t  listener;
    uint16    port;
    char      token[33];
    char      url[128];
    char      discovery_path[1024];
    ws_client clients[SERVER_MAX_CLIENTS];
};

static b8
set_non_blocking(socket_t sock)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(sock, FIONBIO, &on) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    return flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// 32 hex characters from the OS random generator.
static b8
make_token(char out[33])
{
    unsigned char bytes[16];
#ifdef _WIN32
    for (int i = 0; i < 16; i += 4)
    {
        unsigned int value;
        if (rand_s(&value) != 0)
            return false;
        memcpy(bytes + i, &value, 4);
    }
#else
    FILE* random = fopen("/dev/urandom", "rb");
    if (!random)
        return false;
    b8 ok = fread(bytes, 1, sizeof(bytes), random) == sizeof(bytes);
    fclose(random);
    if (!ok)
        return false;
#endif
    for (int i = 0; i < 16; ++i)
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
    return true;
}

// Compares in time independent of where the strings differ.
static b8
tokens_equal(const char* a, const char* b, size_t length)
{
    unsigned char diff = 0;
    for (size_t i = 0; i < length; ++i)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static void
default_discovery_path(char* out, size_t size)
{
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
#endif
    snprintf(out, size, "%s/.pbr-renderer/server.json", home ? home : ".");
}

static void
write_discovery_file(command_server* s)
{
    // The folder first (one level is enough: ~/.pbr-renderer).
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", s->discovery_path);
    char* slash = strrchr(dir, '/');
    char* backslash = strrchr(dir, '\\');
    if (backslash > slash)
        slash = backslash;
    if (slash)
    {
        *slash = '\0';
        file_make_directory(dir);
    }

    cJSON* info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "url", s->url);
    cJSON_AddNumberToObject(info, "port", s->port);
    cJSON_AddStringToObject(info, "token", s->token);
    cJSON_AddNumberToObject(info, "pid", current_pid());
    cJSON_AddStringToObject(info, "protocol", "jsonrpc-2.0 over websocket");

    char* text = cJSON_Print(info);
    file_write_string(s->discovery_path, text);
    cJSON_free(text);
    cJSON_Delete(info);
}

static socket_t
open_listener(uint16 port)
{
    socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCK)
        return INVALID_SOCK;

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;
    address.sin_port        = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only -- never other machines

    if (bind(sock, (struct sockaddr*)&address, sizeof(address)) != 0
        || listen(sock, SERVER_MAX_CLIENTS) != 0
        || !set_non_blocking(sock))
    {
        close_socket(sock);
        return INVALID_SOCK;
    }
    return sock;
}

command_server*
server_start(const server_config* config)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        LOG_ERROR("server: WSAStartup failed\n");
        return NULL;
    }
#endif

    command_server* s = calloc(1, sizeof(*s));
    if (!s)
        FATAL("server: out of memory");

    // The requested port, else (if the default was asked for and is taken
    // -- say, by a second copy of the app) any free one.
    uint16 port = config && config->port ? config->port : SERVER_DEFAULT_PORT;
    s->listener = open_listener(port);
    if (s->listener == INVALID_SOCK && !(config && config->port))
        s->listener = open_listener(0);

    if (s->listener == INVALID_SOCK || !make_token(s->token))
    {
        LOG_ERROR("server: could not listen on 127.0.0.1\n");
        if (s->listener != INVALID_SOCK)
            close_socket(s->listener);
        free(s);
        return NULL;
    }

    struct sockaddr_in bound;
    socklen_t          bound_length = sizeof(bound);
    getsockname(s->listener, (struct sockaddr*)&bound, &bound_length);
    s->port = ntohs(bound.sin_port);

    snprintf(s->url, sizeof(s->url), "ws://127.0.0.1:%u/?token=%s", (unsigned)s->port, s->token);

    if (config && config->discovery_path)
        snprintf(s->discovery_path, sizeof(s->discovery_path), "%s", config->discovery_path);
    else
        default_discovery_path(s->discovery_path, sizeof(s->discovery_path));
    write_discovery_file(s);

    for (uint32 i = 0; i < SERVER_MAX_CLIENTS; ++i)
        s->clients[i].sock = INVALID_SOCK;

    return s;
}

// --- Sending -------------------------------------------------------------------------

#define WS_CONTINUATION 0x0
#define WS_TEXT         0x1
#define WS_BINARY       0x2
#define WS_CLOSE        0x8
#define WS_PING         0x9
#define WS_PONG         0xA

static void
queue_frame(ws_client* c, int opcode, const void* payload, size_t length)
{
    unsigned char header[10];
    size_t        header_length;

    header[0] = (unsigned char)(0x80 | opcode);   // FIN + opcode; server frames are never masked
    if (length < 126)
    {
        header[1] = (unsigned char)length;
        header_length = 2;
    }
    else if (length <= 0xFFFF)
    {
        header[1] = 126;
        header[2] = (unsigned char)(length >> 8);
        header[3] = (unsigned char)length;
        header_length = 4;
    }
    else
    {
        header[1] = 127;
        for (int i = 0; i < 8; ++i)
            header[2 + i] = (unsigned char)((uint64)length >> ((7 - i) * 8));
        header_length = 10;
    }

    buffer_append(&c->out, header, header_length);
    if (length)
        buffer_append(&c->out, payload, length);
}

static void
queue_close(ws_client* c, uint16 status, const char* reason)
{
    unsigned char payload[125];
    size_t        reason_length = reason ? strlen(reason) : 0;
    if (reason_length > sizeof(payload) - 2)
        reason_length = sizeof(payload) - 2;

    payload[0] = (unsigned char)(status >> 8);
    payload[1] = (unsigned char)status;
    if (reason_length)
        memcpy(payload + 2, reason, reason_length);

    queue_frame(c, WS_CLOSE, payload, reason_length + 2);
    c->closing = true;
}

// Plain HTTP answer for a refused handshake, then close.
static void
refuse_http(ws_client* c, const char* status, const char* body)
{
    char response[512];
    int  length = snprintf(response, sizeof(response),
                           "HTTP/1.1 %s\r\nContent-Type: text/plain\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
                           status, (unsigned)strlen(body), body);
    buffer_append(&c->out, response, (size_t)length);
    c->closing = true;
}

static void
drop_client(ws_client* c)
{
    if (c->sock != INVALID_SOCK)
        close_socket(c->sock);
    buffer_free(&c->in);
    buffer_free(&c->out);
    buffer_free(&c->message);
    memset(c, 0, sizeof(*c));
    c->sock = INVALID_SOCK;
}

// --- Handshake -----------------------------------------------------------------------

// Value of HTTP header `name` (case-insensitive) in `headers`, copied into
// `out`. False if absent.
static b8
header_value(const char* headers, const char* name, char* out, size_t size)
{
    size_t      name_length = strlen(name);
    const char* line        = strstr(headers, "\r\n");

    while (line && line[2] != '\r')
    {
        line += 2;
        size_t i = 0;
        while (i < name_length && line[i] && tolower((unsigned char)line[i]) == tolower((unsigned char)name[i]))
            ++i;

        if (i == name_length && line[i] == ':')
        {
            const char* value = line + i + 1;
            while (*value == ' ' || *value == '\t')
                ++value;
            const char* end = strstr(value, "\r\n");
            size_t      length = end ? (size_t)(end - value) : strlen(value);
            while (length && (value[length - 1] == ' ' || value[length - 1] == '\t'))
                --length;
            snprintf(out, size, "%.*s", (int)length, value);
            return true;
        }
        line = strstr(line, "\r\n");
    }
    return false;
}

static b8
starts_with_nocase(const char* s, const char* prefix, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        if (!s[i] || tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i]))
            return false;
    }
    return true;
}

// Case-insensitive "does `list` (comma-separated, e.g. "keep-alive, Upgrade")
// contain `token` as a whole entry".
static b8
header_has_token(const char* list, const char* token)
{
    size_t token_length = strlen(token);
    for (const char* p = list; *p; ++p)
    {
        b8 at_start = p == list || p[-1] == ',' || p[-1] == ' ';
        if (at_start && starts_with_nocase(p, token, token_length))
        {
            char next = p[token_length];
            if (next == '\0' || next == ',' || next == ' ' || next == ';')
                return true;
        }
    }
    return false;
}

// Browsers send an Origin; anything not on this machine is refused (a web
// page must not be able to drive the engine even if the token leaked).
static b8
origin_allowed(const char* origin)
{
    static const char* const ALLOWED[] = { "http://127.0.0.1", "http://localhost", "https://127.0.0.1", "https://localhost", "null" };
    for (size_t i = 0; i < sizeof(ALLOWED) / sizeof(ALLOWED[0]); ++i)
    {
        size_t length = strlen(ALLOWED[i]);
        if (strncmp(origin, ALLOWED[i], length) == 0 && (origin[length] == '\0' || origin[length] == ':'))
            return true;
    }
    return false;
}

static void
handle_handshake(command_server* s, ws_client* c)
{
    unsigned char* end = NULL;
    for (size_t i = 3; i < c->in.length; ++i)
    {
        if (c->in.data[i - 3] == '\r' && c->in.data[i - 2] == '\n' && c->in.data[i - 1] == '\r' && c->in.data[i] == '\n')
        {
            end = c->in.data + i + 1;
            break;
        }
    }

    if (!end)
    {
        if (c->in.length > HANDSHAKE_MAX)
            refuse_http(c, "431 Request Header Fields Too Large", "headers too large");
        return;   // wait for the rest
    }

    size_t length = (size_t)(end - c->in.data);
    char*  request = malloc(length + 1);
    if (!request)
        FATAL("server: out of memory");
    memcpy(request, c->in.data, length);
    request[length] = '\0';
    buffer_consume(&c->in, length);

    // Request line: GET <path> HTTP/1.1
    char        path[512]  = "";
    const char* path_start = strncmp(request, "GET ", 4) == 0 ? request + 4 : NULL;
    const char* path_end   = path_start ? strchr(path_start, ' ') : NULL;
    if (path_end && (size_t)(path_end - path_start) < sizeof(path) && strncmp(path_end, " HTTP/1.1\r\n", 11) == 0)
        snprintf(path, sizeof(path), "%.*s", (int)(path_end - path_start), path_start);
    if (!path[0])
    {
        refuse_http(c, "400 Bad Request", "expected a WebSocket upgrade (GET)");
        free(request);
        return;
    }

    const char* token = strstr(path, "token=");
    if (!token || strlen(token + 6) < 32 || !tokens_equal(token + 6, s->token, 32)
        || (token[6 + 32] != '\0' && token[6 + 32] != '&'))
    {
        refuse_http(c, "403 Forbidden", "missing or wrong token (see ~/.pbr-renderer/server.json)");
        free(request);
        return;
    }

    char origin[256];
    if (header_value(request, "Origin", origin, sizeof(origin)) && !origin_allowed(origin))
    {
        refuse_http(c, "403 Forbidden", "origin not allowed");
        free(request);
        return;
    }

    char upgrade[64], connection[256], key[128], version[16];
    if (!header_value(request, "Upgrade", upgrade, sizeof(upgrade)) || !header_has_token(upgrade, "websocket")
        || !header_value(request, "Connection", connection, sizeof(connection)) || !header_has_token(connection, "upgrade")
        || !header_value(request, "Sec-WebSocket-Key", key, sizeof(key))
        || !header_value(request, "Sec-WebSocket-Version", version, sizeof(version)) || strcmp(version, "13") != 0)
    {
        refuse_http(c, "400 Bad Request", "expected a WebSocket (version 13) upgrade request");
        free(request);
        return;
    }
    free(request);

    // Sec-WebSocket-Accept = base64(sha1(key + magic GUID)), per RFC 6455.
    char combined[256];
    int  combined_length = snprintf(combined, sizeof(combined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char digest[20];
    sha1((const unsigned char*)combined, (size_t)combined_length, digest);
    char accept[32];
    base64_encode(digest, sizeof(digest), accept);

    char response[256];
    int  response_length = snprintf(response, sizeof(response),
                                    "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    buffer_append(&c->out, response, (size_t)response_length);
    c->upgraded = true;
}

// --- Frames --------------------------------------------------------------------------

static ws_client*
find_role(command_server* s, const char* role)
{
    for (uint32 i = 0; i < SERVER_MAX_CLIENTS; ++i)
    {
        ws_client* c = &s->clients[i];
        if (c->in_use && c->upgraded && !c->closing && c->role[0] && strcmp(c->role, role) == 0)
            return c;
    }
    return NULL;
}

// server.hello: the server's own method, about the connection rather than
// the world -- so it is answered here and never reaches the commands.
static char*
handle_hello(command_server* s, ws_client* c, const cJSON* request)
{
    const cJSON* id     = cJSON_GetObjectItemCaseSensitive(request, "id");
    const cJSON* params = cJSON_GetObjectItemCaseSensitive(request, "params");
    const cJSON* role   = cJSON_GetObjectItemCaseSensitive(params, "role");
    const cJSON* name   = cJSON_GetObjectItemCaseSensitive(params, "client");

    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", id ? cJSON_Duplicate(id, true) : cJSON_CreateNull());

    const char* error = NULL;
    const char* code  = CMD_ERROR_INVALID_ARGS;
    if ((role && !cJSON_IsString(role) && !cJSON_IsNull(role)) || (name && !cJSON_IsString(name)))
        error = "server.hello takes {\"client\": <name>, \"role\": <role or null>}";
    else if (cJSON_IsString(role) && (!role->valuestring[0] || strlen(role->valuestring) >= SERVER_ROLE_MAX))
        error = "\"role\" must be 1 to 31 characters";
    else if (cJSON_IsString(role))
    {
        ws_client* holder = find_role(s, role->valuestring);
        if (holder && holder != c)
        {
            error = "another client already holds that role";
            code  = CMD_ERROR_CONFLICT;
        }
    }

    if (error)
    {
        cJSON* e = cJSON_AddObjectToObject(response, "error");
        cJSON_AddNumberToObject(e, "code", strcmp(code, CMD_ERROR_CONFLICT) == 0 ? JSONRPC_COMMAND_FAILED : JSONRPC_INVALID_PARAMS);
        cJSON_AddStringToObject(e, "message", error);
        cJSON_AddStringToObject(cJSON_AddObjectToObject(e, "data"), "code", code);
    }
    else
    {
        if (role)
            snprintf(c->role, sizeof(c->role), "%s", cJSON_IsString(role) ? role->valuestring : "");
        if (name)
            snprintf(c->name, sizeof(c->name), "%s", name->valuestring);

        cJSON* result = cJSON_AddObjectToObject(response, "result");
        cJSON_AddStringToObject(result, "server", "pbr-renderer");
        cJSON_AddItemToObject(result, "role", c->role[0] ? cJSON_CreateString(c->role) : cJSON_CreateNull());
    }

    char* text = id ? cJSON_PrintUnformatted(response) : NULL;   // a notification gets no answer
    cJSON_Delete(response);
    return text;
}

static void
handle_message(command_server* s, command_context* ctx, ws_client* c, const char* text)
{
    char* response = NULL;

    // Cheap test first: only parse here what might be server.hello.
    cJSON* request = strstr(text, "server.hello") ? cJSON_Parse(text) : NULL;
    const cJSON* method = cJSON_GetObjectItemCaseSensitive(request, "method");
    if (cJSON_IsString(method) && strcmp(method->valuestring, SERVER_HELLO_METHOD) == 0)
        response = handle_hello(s, c, request);
    else
        response = jsonrpc_handle(ctx, text);
    cJSON_Delete(request);

    if (response)
    {
        queue_frame(c, WS_TEXT, response, strlen(response));
        cJSON_free(response);
    }
}

// Parses and handles every complete frame in `c->in`.
static void
handle_frames(command_server* s, command_context* ctx, ws_client* c)
{
    while (!c->closing && c->in.length >= 2)
    {
        const unsigned char* d = c->in.data;
        b8     fin    = (d[0] & 0x80) != 0;
        int    opcode = d[0] & 0x0F;
        b8     masked = (d[1] & 0x80) != 0;
        uint64 length = d[1] & 0x7F;
        size_t header = 2;

        if (d[0] & 0x70)
        {
            queue_close(c, 1002, "reserved bits set");
            return;
        }
        if (length == 126)
        {
            if (c->in.length < 4) return;
            length = ((uint64)d[2] << 8) | d[3];
            header = 4;
        }
        else if (length == 127)
        {
            if (c->in.length < 10) return;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = (length << 8) | d[2 + i];
            header = 10;
        }

        if (!masked)
        {
            queue_close(c, 1002, "client frames must be masked");
            return;
        }
        if (length > SERVER_MAX_MESSAGE || c->message.length + length > SERVER_MAX_MESSAGE)
        {
            queue_close(c, 1009, "message too big");
            return;
        }
        if (c->in.length < header + 4 + length)
            return;   // wait for the rest

        const unsigned char* mask    = d + header;
        unsigned char*       payload = c->in.data + header + 4;
        for (uint64 i = 0; i < length; ++i)
            payload[i] ^= mask[i & 3];

        size_t frame_length = header + 4 + (size_t)length;

        if (opcode >= WS_CLOSE)
        {
            // Control frames: never fragmented, at most 125 bytes.
            if (!fin || length > 125)
            {
                queue_close(c, 1002, "bad control frame");
                return;
            }
            if (opcode == WS_CLOSE)
            {
                queue_close(c, 1000, NULL);   // answer the close, then hang up
                return;
            }
            if (opcode == WS_PING)
                queue_frame(c, WS_PONG, payload, (size_t)length);
            // WS_PONG: nothing to do
        }
        else if (opcode == WS_TEXT || opcode == WS_CONTINUATION)
        {
            if ((opcode == WS_TEXT) == c->in_message)
            {
                queue_close(c, 1002, "unexpected continuation");
                return;
            }
            buffer_append(&c->message, payload, (size_t)length);
            c->in_message = !fin;

            if (fin)
            {
                buffer_append(&c->message, "", 1);   // NUL-terminate
                handle_message(s, ctx, c, (const char*)c->message.data);
                c->message.length = 0;
            }
        }
        else
        {
            queue_close(c, 1003, "only text (JSON-RPC) messages are accepted");
            return;
        }

        buffer_consume(&c->in, frame_length);
    }
}

// --- Polling --------------------------------------------------------------------------

static void
accept_clients(command_server* s)
{
    for (;;)
    {
        socket_t sock = accept(s->listener, NULL, NULL);
        if (sock == INVALID_SOCK)
            return;   // nothing waiting (or a transient error)

        ws_client* slot = NULL;
        for (uint32 i = 0; i < SERVER_MAX_CLIENTS && !slot; ++i)
        {
            if (!s->clients[i].in_use)
                slot = &s->clients[i];
        }

        if (!slot || !set_non_blocking(sock))
        {
            close_socket(sock);
            continue;
        }

        int on = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));

        memset(slot, 0, sizeof(*slot));
        slot->sock   = sock;
        slot->in_use = true;
    }
}

// Reads everything available. False if the peer closed or errored.
static b8
receive(ws_client* c)
{
    unsigned char chunk[16384];
    for (;;)
    {
        int received = recv(c->sock, (char*)chunk, (int)sizeof(chunk), 0);
        if (received > 0)
        {
            buffer_append(&c->in, chunk, (size_t)received);
            if (c->in.length > SERVER_MAX_MESSAGE + HANDSHAKE_MAX)
                return false;
            continue;
        }
        if (received == 0)
            return false;   // orderly shutdown
        return WOULD_BLOCK(socket_error());
    }
}

// Sends as much of `out` as the socket takes. False on error.
static b8
flush(ws_client* c)
{
    while (c->out.length)
    {
        int sent = send(c->sock, (const char*)c->out.data, (int)c->out.length, 0);
        if (sent > 0)
        {
            buffer_consume(&c->out, (size_t)sent);
            continue;
        }
        return sent < 0 && WOULD_BLOCK(socket_error());
    }
    return true;
}

void
server_poll(command_server* s, command_context* ctx)
{
    if (!s)
        return;

    accept_clients(s);

    for (uint32 i = 0; i < SERVER_MAX_CLIENTS; ++i)
    {
        ws_client* c = &s->clients[i];
        if (!c->in_use)
            continue;

        if (!c->closing && !receive(c))
        {
            drop_client(c);
            continue;
        }

        if (!c->closing && !c->upgraded)
            handle_handshake(s, c);
        if (!c->closing && c->upgraded)
            handle_frames(s, ctx, c);

        if (!flush(c) || (c->closing && c->out.length == 0))
            drop_client(c);
    }
}

void
server_broadcast(command_server* s, const char* text)
{
    if (!s)
        return;

    size_t length = strlen(text);
    for (uint32 i = 0; i < SERVER_MAX_CLIENTS; ++i)
    {
        ws_client* c = &s->clients[i];
        if (c->in_use && c->upgraded && !c->closing)
            queue_frame(c, WS_TEXT, text, length);
    }
    // Sent on the next server_poll().
}

void
server_notify(command_server* s, const char* method, const cJSON* params)
{
    if (!s)
        return;

    char* text = jsonrpc_notification(method, params);
    server_broadcast(s, text);
    cJSON_free(text);
}

b8
server_has_role(command_server* s, const char* role)
{
    return s && find_role(s, role) != NULL;
}

b8
server_notify_role(command_server* s, const char* role, const char* method, const cJSON* params)
{
    ws_client* c = s ? find_role(s, role) : NULL;
    if (!c)
        return false;

    char* text = jsonrpc_notification(method, params);
    queue_frame(c, WS_TEXT, text, strlen(text));
    cJSON_free(text);
    return true;   // sent on the next server_poll()
}

void
server_on_change(void* server, const cJSON* event)
{
    server_notify(server, SERVER_CHANGE_NOTIFICATION, event);
}

uint16
server_port(const command_server* s)
{
    return s ? s->port : 0;
}

const char*
server_url(const command_server* s)
{
    return s ? s->url : "";
}

uint32
server_client_count(const command_server* s)
{
    uint32 count = 0;
    for (uint32 i = 0; s && i < SERVER_MAX_CLIENTS; ++i)
        count += s->clients[i].in_use && s->clients[i].upgraded;
    return count;
}

void
server_stop(command_server* s)
{
    if (!s)
        return;

    for (uint32 i = 0; i < SERVER_MAX_CLIENTS; ++i)
    {
        if (s->clients[i].in_use)
            drop_client(&s->clients[i]);
    }
    close_socket(s->listener);
    remove(s->discovery_path);
    free(s);

#ifdef _WIN32
    WSACleanup();
#endif
}
