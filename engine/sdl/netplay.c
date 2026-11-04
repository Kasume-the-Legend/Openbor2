/*
 * OpenBOR Online - lockstep netplay with a room-code relay.
 *
 * Protocol (text lines over TCP, see netplay/relay.js):
 *   HELLO 1 <name>   HOST <pub> <max> <pakid>   JOIN <code> <pakid>   LIST   READY <0|1>   START   I <frame> <hex>   C <frame> <hex>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <SDL.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
#include "netplay.h"
#ifdef NETPLAY_TLS
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#endif
#define hankaku_font10 np_font10   /* the font table lives in a header; keep this file's copy private */
#include "hankaku.h"


#if defined(WIN) || defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET nsock_t;
#define NS_INVALID INVALID_SOCKET
#define ns_close closesocket
#define ns_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
static void ns_nonblock(nsock_t s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
typedef int nsock_t;
#define NS_INVALID (-1)
#define ns_close close
#define ns_wouldblock() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
static void ns_nonblock(nsock_t s) { int f = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, f | O_NONBLOCK); }
#endif

NetState netplay = { 0, 0, 0, 0, 4, 0, 0, 0, 0 };

#define RING 256
#define KEYMASK_ALL 0xffffffffffffffffULL
static nsock_t sock = NS_INVALID;
static char inbuf[16384]; static int inlen = 0;
static char outbuf[16384]; static int outlen = 0;
static char cfg_host[128] = "127.0.0.1"; static int cfg_port = 7777; static char cfg_name[24] = "Player";
static int cfg_tls = 0, cfg_ws = 0; static char cfg_path[128] = "/ws";     /* wss://host/path = WebSocket over TLS (Cloudflare); host:port = plain TCP relay */
static unsigned char win[8192]; static int winlen = 0;                       /* raw bytes from the wire (WebSocket frames) */

/* room state */
static char room_code[16] = "";
static int my_slot = -1, host_slot = -1;
static char r_name[4][24]; static int r_present[4], r_ready[4], r_host[4];
static int started = 0, rl_n = 0, rl_done = 0;
static struct { char code[8]; int n, max; char pak[28]; char host[24]; } rl[16];
static char last_err[96] = "";
static unsigned go_seed = 0; static int go_mask = 0, go_delay = 4;

/* lockstep state */
static unsigned long long inputs[4][RING]; static unsigned char valid[4][RING];
static unsigned myhash[RING]; static unsigned char myhash_ok[RING];
static unsigned rhash[4][RING]; static unsigned char rhash_ok[4][RING];
static unsigned long long local_prev = 0;
static Uint32 base_ms = 0; static int base_set = 0;
static int fuzz = 0;
static unsigned fuzz_state = 1;

/* ------------------------------------------------------------------ small helpers */
static void trim(char *s) { int n = (int)strlen(s); while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0; }

static FILE *open_ini(void)
{
    FILE *f = fopen("netplay.ini", "r");
#if defined(ANDROID)
    if (!f)
    {   /* Android: put netplay.ini in the app folder (Android/data/org.openbor.engine/files) or next to the game */
        const char *dir = SDL_AndroidGetExternalStoragePath();
        if (dir) { char path[512]; snprintf(path, sizeof path, "%s/netplay.ini", dir); f = fopen(path, "r"); }
    }
#endif
    return f;
}

/* "wss://host[:port][/path]", "ws://host[:port][/path]" or "host[:port]" */
static void parse_server(const char *v)
{
    char tmp[200], *c, *sl; const char *h = v;
    cfg_tls = cfg_ws = 0; cfg_port = 7777; snprintf(cfg_path, sizeof cfg_path, "/ws");
    if (!strncmp(v, "wss://", 6)) { cfg_tls = cfg_ws = 1; cfg_port = 443; h = v + 6; }
    else if (!strncmp(v, "ws://", 5)) { cfg_ws = 1; cfg_port = 80; h = v + 5; }
    else if (!strncmp(v, "https://", 8)) { cfg_tls = cfg_ws = 1; cfg_port = 443; h = v + 8; }
    snprintf(tmp, sizeof tmp, "%s", h);
    if (cfg_ws && (sl = strchr(tmp, '/'))) { if (sl[1]) snprintf(cfg_path, sizeof cfg_path, "%s", sl); *sl = 0; }
    c = strrchr(tmp, ':'); if (c) { *c = 0; cfg_port = atoi(c + 1); }
    snprintf(cfg_host, sizeof cfg_host, "%s", tmp);
}

static void load_config(void)
{
    FILE *f = open_ini(); char line[200];
    const char *e;
#ifdef NETPLAY_DEFAULT_SERVER
    parse_server(NETPLAY_DEFAULT_SERVER);        /* baked in at build time (RELAY_URL); netplay.ini or OPENBOR_RELAY still override it */
#endif
    if (f)
    {
        while (fgets(line, sizeof line, f))
        {
            trim(line);
            if (!strncmp(line, "server=", 7)) parse_server(line + 7);
            else if (!strncmp(line, "name=", 5)) snprintf(cfg_name, sizeof cfg_name, "%.16s", line + 5);
        }
        fclose(f);
    }
    if ((e = getenv("OPENBOR_RELAY")) && *e) parse_server(e);
    if ((e = getenv("OPENBOR_NAME")) && *e) snprintf(cfg_name, sizeof cfg_name, "%.16s", e);
    { char *p; for (p = cfg_name; *p; p++) if (!isalnum((unsigned char)*p) && *p != ' ' && *p != '-' && *p != '_') *p = '_'; }
    if (!cfg_name[0]) snprintf(cfg_name, sizeof cfg_name, "Player");
}

int netplay_enabled(void)
{
    const char *e = getenv("OPENBOR_NET");
    if (e && e[0] == '0') return 0;
    if (e && e[0]) return 1;
#if defined(ANDROID)
    return 1;   /* Android: the lobby always shows, and "Play offline" starts the game normally */
#else
    { FILE *f = open_ini(); if (f) { fclose(f); return 1; } }
    return 0;
#endif
}

static unsigned crc32_update(unsigned crc, const unsigned char *p, size_t n)
{
    static unsigned t[256]; static int init = 0; size_t i;
    if (!init) { unsigned k, j, c; for (k = 0; k < 256; k++) { c = k; for (j = 0; j < 8; j++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[k] = c; } init = 1; }
    crc = ~crc; for (i = 0; i < n; i++) crc = t[(crc ^ p[i]) & 0xff] ^ (crc >> 8); return ~crc;
}

void netplay_pakid(const char *path, char *out, int outlen)
{
    FILE *f = fopen(path, "rb"); unsigned crc = 0; long size = 0; unsigned char buf[65536]; size_t n; long chunk = 4L * 1024 * 1024;
    if (!f) { snprintf(out, outlen, "nopak"); return; }
    fseek(f, 0, SEEK_END); size = ftell(f);
    fseek(f, 0, SEEK_SET);
    { long left = size < chunk ? size : chunk; while (left > 0 && (n = fread(buf, 1, left > (long)sizeof buf ? sizeof buf : (size_t)left, f)) > 0) { crc = crc32_update(crc, buf, n); left -= (long)n; } }
    if (size > 2 * chunk) { long left = chunk; fseek(f, size - chunk, SEEK_SET); while (left > 0 && (n = fread(buf, 1, left > (long)sizeof buf ? sizeof buf : (size_t)left, f)) > 0) { crc = crc32_update(crc, buf, n); left -= (long)n; } }
    fclose(f);
    snprintf(out, outlen, "%lx-%08x", size, crc);
}

/* ------------------------------------------------------------------ transport: TCP, optionally TLS, optionally WebSocket */
#define TR_WB (-1)      /* would block */
#define TR_ERR (-2)
#ifdef NETPLAY_TLS
static mbedtls_net_context tls_net; static mbedtls_ssl_context tls_ssl; static mbedtls_ssl_config tls_conf;
static mbedtls_entropy_context tls_ent; static mbedtls_ctr_drbg_context tls_drbg; static int tls_up = 0, tls_pend = 0;
#endif

static void tr_close(void)
{
#ifdef NETPLAY_TLS
    if (tls_up) { mbedtls_ssl_close_notify(&tls_ssl); mbedtls_ssl_free(&tls_ssl); mbedtls_ssl_config_free(&tls_conf); mbedtls_ctr_drbg_free(&tls_drbg); mbedtls_entropy_free(&tls_ent); tls_up = 0; tls_pend = 0; }
#endif
    if (sock != NS_INVALID) ns_close(sock);
    sock = NS_INVALID; winlen = 0;
}

static int tr_write(const void *p, int n)
{
#ifdef NETPLAY_TLS
    if (tls_up)
    {
        int use = tls_pend ? tls_pend : n, r;
        r = mbedtls_ssl_write(&tls_ssl, (const unsigned char *)p, use);
        if (r > 0) { tls_pend = 0; return r; }
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) { tls_pend = use; return TR_WB; }
        return TR_ERR;
    }
#endif
    { int r = (int)send(sock, (const char *)p, n, 0); if (r > 0) return r; if (r < 0 && ns_wouldblock()) return TR_WB; return TR_ERR; }
}

static int tr_read(void *p, int n)   /* >0 bytes, 0 = closed, TR_WB, TR_ERR */
{
#ifdef NETPLAY_TLS
    if (tls_up)
    {
        int r = mbedtls_ssl_read(&tls_ssl, (unsigned char *)p, n);
        if (r > 0) return r;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return TR_WB;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return TR_ERR;
    }
#endif
    { int r = (int)recv(sock, (char *)p, n, 0); if (r > 0) return r; if (r == 0) return 0; return ns_wouldblock() ? TR_WB : TR_ERR; }
}

static int tr_tls_start(void)
{
#ifdef NETPLAY_TLS
    Uint32 t0 = SDL_GetTicks(); int r;
    mbedtls_net_init(&tls_net); tls_net.fd = (int)sock;
    mbedtls_ssl_init(&tls_ssl); mbedtls_ssl_config_init(&tls_conf); mbedtls_entropy_init(&tls_ent); mbedtls_ctr_drbg_init(&tls_drbg);
    tls_up = 1; tls_pend = 0;
    if (mbedtls_ctr_drbg_seed(&tls_drbg, mbedtls_entropy_func, &tls_ent, (const unsigned char *)"openbor", 7) != 0) goto fail;
    if (mbedtls_ssl_config_defaults(&tls_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) goto fail;
    /* Only game button presses and room codes travel here, and a phone has no CA store we can rely on, so the
       connection is encrypted but the server certificate is not checked. */
    mbedtls_ssl_conf_authmode(&tls_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&tls_conf, mbedtls_ctr_drbg_random, &tls_drbg);
    if (mbedtls_ssl_setup(&tls_ssl, &tls_conf) != 0) goto fail;
    mbedtls_ssl_set_hostname(&tls_ssl, cfg_host);                       /* SNI: Cloudflare needs it */
    mbedtls_ssl_set_bio(&tls_ssl, &tls_net, mbedtls_net_send, mbedtls_net_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&tls_ssl)) != 0)
    {
        if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) || SDL_GetTicks() - t0 > 10000) goto fail;
        SDL_Delay(5);
    }
    return 1;
fail:
    snprintf(last_err, sizeof last_err, "secure connection to %s failed", cfg_host);
    return 0;
#else
    snprintf(last_err, sizeof last_err, "this build has no wss:// support");
    return 0;
#endif
}

static const char b64t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static unsigned rng_state = 0x9e3779b9u;
static unsigned rng_next(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static int ws_handshake(void)
{
    char req[512], key[32], resp[2048]; unsigned char raw[16]; int i, n, sent = 0, got = 0, hdr = -1; Uint32 t0 = SDL_GetTicks();
    rng_state ^= (unsigned)SDL_GetTicks() * 2654435761u; if (!rng_state) rng_state = 1;
    for (i = 0; i < 16; i++) raw[i] = (unsigned char)(rng_next() >> 11);
    for (i = 0; i < 15; i += 3) { unsigned v = (raw[i] << 16) | (raw[i + 1] << 8) | raw[i + 2]; key[i / 3 * 4] = b64t[v >> 18]; key[i / 3 * 4 + 1] = b64t[(v >> 12) & 63]; key[i / 3 * 4 + 2] = b64t[(v >> 6) & 63]; key[i / 3 * 4 + 3] = b64t[v & 63]; }
    { unsigned v = raw[15] << 16; key[20] = b64t[v >> 18]; key[21] = b64t[(v >> 12) & 63]; key[22] = '='; key[23] = '='; key[24] = 0; }
    n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: OpenBOR-Online\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", cfg_path, cfg_host, key);
    while (sent < n)
    {
        int r = tr_write(req + sent, n - sent);
        if (r > 0) sent += r; else if (r == TR_WB && SDL_GetTicks() - t0 < 10000) SDL_Delay(5); else goto fail;
    }
    while (hdr < 0)
    {
        int r = tr_read(resp + got, (int)sizeof resp - 1 - got);
        if (r > 0) { got += r; resp[got] = 0; { char *e = strstr(resp, "\r\n\r\n"); if (e) hdr = (int)(e - resp) + 4; } if (got >= (int)sizeof resp - 1 && hdr < 0) goto fail; }
        else if (r == TR_WB && SDL_GetTicks() - t0 < 10000) SDL_Delay(5);
        else goto fail;
    }
    if (strncmp(resp, "HTTP/1.1 101", 12)) goto fail;
    winlen = 0; if (got > hdr && got - hdr <= (int)sizeof win) { memcpy(win, resp + hdr, got - hdr); winlen = got - hdr; }
    return 1;
fail:
    snprintf(last_err, sizeof last_err, "relay at %s did not accept the connection", cfg_host);
    return 0;
}

/* wrap one message in a (masked, as clients must) WebSocket frame and queue it */
static void ws_frame(int opcode, const unsigned char *pl, int n)
{
    unsigned char hdr[8]; int hl = 0, i; unsigned m = rng_next();
    if (n > 125) return;
    if (outlen + 6 + n > (int)sizeof outbuf) return;
    hdr[hl++] = 0x80 | opcode; hdr[hl++] = 0x80 | n;
    memcpy(hdr + hl, &m, 4);
    memcpy(outbuf + outlen, hdr, hl + 4); 
    for (i = 0; i < n; i++) outbuf[outlen + hl + 4 + i] = (char)(pl[i] ^ ((unsigned char *)&m)[i & 3]);
    outlen += hl + 4 + n;
}

/* ------------------------------------------------------------------ sockets */
static int ns_connect(void)
{
    struct addrinfo hints, *res = NULL, *a; char port[16]; int one = 1;
#if defined(WIN) || defined(_WIN32)
    static int wsa = 0; if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); wsa = 1; }
#endif
    memset(&hints, 0, sizeof hints); hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof port, "%d", cfg_port);
    if (getaddrinfo(cfg_host, port, &hints, &res) != 0 || !res) { snprintf(last_err, sizeof last_err, "cannot find %s", cfg_host); return 0; }
    for (a = res; a; a = a->ai_next)
    {
        sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (sock == NS_INVALID) continue;
        if (connect(sock, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        tr_close();
    }
    freeaddrinfo(res);
    if (sock == NS_INVALID) { snprintf(last_err, sizeof last_err, "cannot connect to %s:%d", cfg_host, cfg_port); return 0; }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    ns_nonblock(sock); inlen = outlen = winlen = 0;
    if (cfg_tls && !tr_tls_start()) { tr_close(); return 0; }
    if (cfg_ws && !ws_handshake()) { tr_close(); return 0; }
    return 1;
}

static void ns_flush(void)
{
    while (outlen > 0 && sock != NS_INVALID)
    {
        int n = tr_write(outbuf, outlen);
        if (n > 0) { memmove(outbuf, outbuf + n, outlen - n); outlen -= n; }
        else if (n == TR_WB) break;
        else { tr_close(); break; }
    }
}

static void ns_send(const char *fmt, ...)
{
    char line[200]; int n; va_list ap;
    va_start(ap, fmt); vsnprintf(line, sizeof line - 2, fmt, ap); va_end(ap);
    n = (int)strlen(line); line[n++] = '\n';
    if (outlen + n + 8 > (int)sizeof outbuf) { ns_flush(); if (outlen + n + 8 > (int)sizeof outbuf) return; }
    if (cfg_ws) ws_frame(1, (const unsigned char *)line, n);
    else { memcpy(outbuf + outlen, line, n); outlen += n; }
    ns_flush();
}

static void handle_line(char *line);
/* pull complete WebSocket frames out of win[] and append their text to inbuf */
static void ws_parse(void)
{
    for (;;)
    {
        int op, len, hl = 2;
        if (winlen < 2) return;
        op = win[0] & 15; len = win[1] & 127;
        if (len == 126) { if (winlen < 4) return; len = (win[2] << 8) | win[3]; hl = 4; }
        else if (len == 127) { tr_close(); return; }                       /* far larger than any message we use */
        if (winlen < hl + len) { if (hl + len > (int)sizeof win) tr_close(); return; }
        if (op == 1 || op == 2 || op == 0) { int k = len; if (inlen + k > (int)sizeof inbuf) k = (int)sizeof inbuf - inlen; memcpy(inbuf + inlen, win + hl, k); inlen += k; }
        else if (op == 9) { if (len <= 125) ws_frame(10, win + hl, len); }
        else if (op == 8) { tr_close(); return; }
        memmove(win, win + hl + len, winlen - hl - len); winlen -= hl + len;
    }
}

static void ns_poll(void)
{
    char tmp[4096]; int n;
    if (sock == NS_INVALID) return;
    ns_flush();
    for (;;)
    {
        if (sock == NS_INVALID) break;
        n = tr_read(tmp, sizeof tmp);
        if (n > 0)
        {
            if (cfg_ws)
            {
                if (winlen + n > (int)sizeof win) n = (int)sizeof win - winlen;
                memcpy(win + winlen, tmp, n); winlen += n; ws_parse();
            }
            else
            {
                if (inlen + n > (int)sizeof inbuf) n = (int)sizeof inbuf - inlen;
                memcpy(inbuf + inlen, tmp, n); inlen += n;
            }
        }
        else if (n == 0 || n == TR_ERR) { tr_close(); break; }
        else break;
    }
    if (cfg_ws && sock != NS_INVALID) ns_flush();                          /* pong replies */
    for (;;)
    {
        char *nl = (char *)memchr(inbuf, '\n', inlen); int len;
        if (!nl) { if (inlen >= (int)sizeof inbuf - 1) inlen = 0; break; }
        len = (int)(nl - inbuf); *nl = 0; trim(inbuf);
        handle_line(inbuf);
        memmove(inbuf, nl + 1, inlen - len - 1); inlen -= len + 1;
    }
}

/* ------------------------------------------------------------------ incoming */
static void handle_line(char *line)
{
    char *t[8]; int nt = 0; char *p = line;
    while (nt < 7 && *p) { t[nt++] = p; while (*p && *p != ' ') p++; if (*p) *p++ = 0; }
    t[nt] = NULL; (void)t;
    if (nt == 0) return;
    if (!strcmp(t[0], "ROOM") && nt >= 3) { snprintf(room_code, sizeof room_code, "%.8s", t[1]); my_slot = atoi(t[2]); last_err[0] = 0; }
    else if (!strcmp(t[0], "ERR")) { snprintf(last_err, sizeof last_err, "%s", line + 4); }
    else if (!strcmp(t[0], "P") && nt >= 5)
    {
        int i = atoi(t[1]) & 3; r_present[i] = strcmp(t[2], "-") != 0; r_ready[i] = r_present[i] ? atoi(t[2]) : 0; r_host[i] = atoi(t[3]);
        snprintf(r_name[i], sizeof r_name[i], "%s", t[4]); if (r_host[i] && r_present[i]) host_slot = i;
    }
    else if (!strcmp(t[0], "RL") && nt >= 6 && rl_n < 16)
    {
        snprintf(rl[rl_n].code, sizeof rl[rl_n].code, "%s", t[1]); rl[rl_n].n = atoi(t[2]); rl[rl_n].max = atoi(t[3]);
        snprintf(rl[rl_n].pak, sizeof rl[rl_n].pak, "%s", t[4]); snprintf(rl[rl_n].host, sizeof rl[rl_n].host, "%s", t[5]); rl_n++;
    }
    else if (!strcmp(t[0], "RLEND")) rl_done = 1;
    else if (!strcmp(t[0], "GO") && nt >= 4) { go_seed = (unsigned)strtoul(t[1], NULL, 10); go_mask = atoi(t[2]); go_delay = atoi(t[3]); started = 1; }
    else if (!strcmp(t[0], "DROP") && nt >= 2) { int s = atoi(t[1]) & 3; netplay.dropped |= 1 << s; }
    else if (!strcmp(t[0], "I") && nt >= 4)
    {
        int s = atoi(t[1]) & 3; unsigned f = (unsigned)strtoul(t[2], NULL, 10);
        inputs[s][f % RING] = strtoull(t[3], NULL, 16); valid[s][f % RING] = 1;
    }
    else if (!strcmp(t[0], "C") && nt >= 4)
    {
        int rs = atoi(t[1]) & 3; unsigned f = (unsigned)strtoul(t[2], NULL, 10), h = (unsigned)strtoul(t[3], NULL, 16);
        rhash[rs][f % RING] = h; rhash_ok[rs][f % RING] = 1;
        if (myhash_ok[f % RING] && myhash[f % RING] != h && !netplay.desync)
        {
            netplay.desync = 1;
            fprintf(stderr, "[netplay] DESYNC at frame %u: player %d has a different game state. Everyone needs the same game file and save data.\n", f, rs + 1);
        }
    }
}

/* ------------------------------------------------------------------ lobby UI */
#define LW 320
#define LH 200
static SDL_Window *lwin = NULL; static SDL_Surface *lsurf = NULL;
static int l_engine_win = 0;   /* 1 = drawing through the game's own window (always on Android: one window only, and it is an OpenGL window) */

/* The game's window and renderer (sdl/video.c, or android/app/jni/openbor/video.c). */
extern SDL_Window *window; extern SDL_Renderer *renderer;
static SDL_Texture *lobby_tex = NULL; static SDL_Renderer *lobby_tex_r = NULL; static SDL_Rect lobby_dst;

static int video_lobby_available(void) { return window != NULL && renderer != NULL; }

static int video_lobby_present(SDL_Surface *s)
{
    int ow = 0, oh = 0, dw, dh;
    if (!video_lobby_available() || !s) return 0;
    if (lobby_tex && lobby_tex_r != renderer) lobby_tex = NULL;          /* went away with the old renderer */
    if (!lobby_tex)
    {
        lobby_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, s->w, s->h);
        lobby_tex_r = renderer;
        if (!lobby_tex) return 0;
    }
    SDL_UpdateTexture(lobby_tex, NULL, s->pixels, s->pitch);
    SDL_GetRendererOutputSize(renderer, &ow, &oh);
    if (ow <= 0 || oh <= 0) return 0;
    dw = ow; dh = ow * s->h / s->w;
    if (dh > oh) { dh = oh; dw = oh * s->w / s->h; }
    lobby_dst.x = (ow - dw) / 2; lobby_dst.y = (oh - dh) / 2; lobby_dst.w = dw; lobby_dst.h = dh;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, lobby_tex, NULL, &lobby_dst);
    SDL_RenderPresent(renderer);
    return 1;
}

/* window coordinates (mouse / touch) -> lobby coordinates; 0 if the tap is outside the picture */
static int video_lobby_map(int x, int y, int sw, int sh, int *lx, int *ly)
{
    int ww = 0, wh = 0, ow = 0, oh = 0, px, py;
    if (!window || !renderer || lobby_dst.w <= 0) return 0;
    SDL_GetWindowSize(window, &ww, &wh); SDL_GetRendererOutputSize(renderer, &ow, &oh);
    if (ww <= 0 || wh <= 0) return 0;
    px = x * ow / ww; py = y * oh / wh;
    *lx = (px - lobby_dst.x) * sw / lobby_dst.w; *ly = (py - lobby_dst.y) * sh / lobby_dst.h;
    return *lx >= 0 && *ly >= 0 && *lx < sw && *ly < sh;
}

static void video_lobby_release(void)
{
    if (lobby_tex) { SDL_DestroyTexture(lobby_tex); lobby_tex = NULL; }
    lobby_dst.w = 0;
}
static void lrect(int x, int y, int w, int h, unsigned col)
{
    SDL_Rect r; r.x = x; r.y = y; r.w = w; r.h = h; SDL_FillRect(lsurf, &r, col);
}
static void ltext(int x, int y, unsigned col, const char *s)
{
    unsigned *px = (unsigned *)lsurf->pixels; int pitch = lsurf->pitch / 4;
    for (; *s; s++, x += 6)
    {
        int ch = (unsigned char)*s, y1, x1; const unsigned char *g;
        if (ch < 0x20) ch = 0; else if (ch < 0x80) ch -= 0x20; else ch = 0;
        g = &hankaku_font10[ch * 10];
        for (y1 = 0; y1 < 10; y1++)
            for (x1 = 0; x1 < 5; x1++)
                if ((g[y1] >> x1) & 1) { int X = x + x1, Y = y + y1; if (X >= 0 && X < LW && Y >= 0 && Y < LH) px[Y * pitch + X] = col; }
    }
}
static void lpresent(void)
{
    SDL_Surface *ws;
    if (l_engine_win) { video_lobby_present(lsurf); return; }
    ws = SDL_GetWindowSurface(lwin);
    if (ws) { SDL_BlitScaled(lsurf, NULL, ws, NULL); SDL_UpdateWindowSurface(lwin); }
}
#define C_BG 0x0c1020u
#define C_TXT 0xe8ecffu
#define C_DIM 0x8890b0u
#define C_HOT 0xff6a4au
#define C_OK 0x6aff9au
#define C_SEL 0x2a3a70u

static int lobby_open(void)
{
    if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_InitSubSystem(SDL_INIT_VIDEO);
    l_engine_win = video_lobby_available();
    if (l_engine_win)
    {
        lsurf = SDL_CreateRGBSurfaceWithFormat(0, LW, LH, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!lsurf) return 0;
        SDL_StartTextInput();
        return 1;
    }
    lwin = SDL_CreateWindow("OpenBOR Online", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, LW * 3, LH * 3, SDL_WINDOW_SHOWN);
    if (!lwin) return 0;
    lsurf = SDL_CreateRGBSurfaceWithFormat(0, LW, LH, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!lsurf) return 0;
    SDL_StartTextInput();
    return 1;
}
static void audio_test_stop(void);
static void lobby_close(void)
{
    audio_test_stop();
    SDL_StopTextInput();
    if (lsurf) { SDL_FreeSurface(lsurf); lsurf = NULL; }
    if (l_engine_win) { video_lobby_release(); l_engine_win = 0; }
    if (lwin) { SDL_DestroyWindow(lwin); lwin = NULL; }
}

typedef enum { S_MENU, S_CODE, S_LIST, S_ROOM } lscreen;

static void draw_header(const char *sub)
{
    lrect(0, 0, LW, LH, C_BG);
    ltext(12, 10, C_HOT, "OPENBOR ONLINE");
    ltext(12, 24, C_DIM, sub);
}

static int connect_and_hello(void)
{
    if (sock != NS_INVALID) return 1;
    if (!ns_connect()) return 0;
    ns_send("HELLO 1 %s", cfg_name); return 1;
}

/* "Test sound": plays a 1 second beep through SDL, independent of the game, and shows which audio driver the phone gave us. */
static SDL_AudioDeviceID at_dev = 0; static Uint32 at_until = 0; static unsigned at_phase = 0;
static void at_callback(void *ud, Uint8 *stream, int len)
{
    short *o = (short *)stream; int i, n = len / 4; (void)ud;
    for (i = 0; i < n; i++) { short v = (short)(((at_phase / 50) & 1) ? 6000 : -6000); at_phase++; o[2 * i] = v; o[2 * i + 1] = v; }   /* ~440 Hz square at 44.1 kHz */
}
static void audio_test_start(void)
{
    SDL_AudioSpec want, got; const char *drv;
    if (at_dev) { SDL_CloseAudioDevice(at_dev); at_dev = 0; }
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) { snprintf(last_err, sizeof last_err, "sound: cannot start audio: %.60s", SDL_GetError()); return; }
    SDL_zero(want); want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = at_callback;
    at_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    drv = SDL_GetCurrentAudioDriver();
    if (!at_dev) { snprintf(last_err, sizeof last_err, "sound FAILED (%s): %.50s", drv ? drv : "no driver", SDL_GetError()); return; }
    SDL_PauseAudioDevice(at_dev, 0); at_until = SDL_GetTicks() + 1200;
    snprintf(last_err, sizeof last_err, "sound opened: %s %dHz - did you hear a beep?", drv ? drv : "?", got.freq);
}
static void audio_test_stop(void) { if (at_dev) { SDL_CloseAudioDevice(at_dev); at_dev = 0; } }
static void audio_test_tick(void)
{
    if (at_dev && SDL_GetTicks() > at_until) { SDL_CloseAudioDevice(at_dev); at_dev = 0; }
}

int netplay_lobby(const char *pakid)
{
    const char *items[] = { "Host a public game", "Host a private game", "Join with a room code", "Browse public games", "Play offline", "Test sound", "Quit" };
    lscreen scr = S_MENU; int sel = 0, running = 1, result = 0, i;
    char code[8] = ""; int codelen = 0; int lsel = 0;
    const char *auto_mode = getenv("NETPLAY_AUTO"); int auto_need = getenv("NETPLAY_N") ? atoi(getenv("NETPLAY_N")) : 2;
    int auto_done = 0; Uint32 lastping = SDL_GetTicks();
    load_config();
    fuzz = getenv("NETPLAY_FUZZ") != NULL;
    memset(r_present, 0, sizeof r_present);
    if (!lobby_open()) { fprintf(stderr, "[netplay] cannot open the lobby window\n"); return 0; }

    while (running)
    {
        SDL_Event e; Uint32 now = SDL_GetTicks();
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT) { result = -1; running = 0; }
            if (e.type == SDL_TEXTINPUT && scr == S_CODE)
            {
                const char *c = e.text.text;
                for (; *c && codelen < 5; c++) if (isalnum((unsigned char)*c)) code[codelen++] = (char)toupper((unsigned char)*c);
                code[codelen] = 0;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
            {   /* touch / mouse: turn a tap into the same key the keyboard would send */
                int ww = LW, wh = LH, tx, ty, vk = 0;
                if (l_engine_win) { if (!video_lobby_map(e.button.x, e.button.y, LW, LH, &tx, &ty)) { tx = ty = -100; } }
                else { SDL_GetWindowSize(lwin, &ww, &wh); tx = e.button.x * LW / (ww > 0 ? ww : LW); ty = e.button.y * LH / (wh > 0 ? wh : LH); }
                if (scr == S_MENU) { for (i = 0; i < 7; i++) if (ty >= 42 + i * 16 && ty < 42 + i * 16 + 15) { sel = i; vk = SDLK_RETURN; } }
                else if (scr == S_CODE)
                {
                    if (ty >= 120 && ty < 142 && tx >= 12 && tx < 100) vk = SDLK_RETURN;
                    else if (ty >= 120 && ty < 142 && tx >= 110 && tx < 198) vk = SDLK_ESCAPE;
                    else { SDL_StartTextInput(); }
                }
                else if (scr == S_LIST)
                {
                    if (ty >= 150 && ty < 172 && tx >= 12 && tx < 100) vk = SDLK_ESCAPE;
                    else if (ty >= 150 && ty < 172 && tx >= 110 && tx < 198) vk = SDLK_F5;
                    else for (i = 0; i < rl_n; i++) if (ty >= 42 + i * 14 && ty < 42 + i * 14 + 13) { lsel = i; vk = SDLK_RETURN; }
                }
                else if (scr == S_ROOM)
                {
                    if (ty >= 150 && ty < 172 && tx >= 12 && tx < 130) vk = SDLK_r;
                    else if (ty >= 150 && ty < 172 && tx >= 140 && tx < 220) vk = SDLK_ESCAPE;
                }
                if (vk) { e.type = SDL_KEYDOWN; e.key.repeat = 0; e.key.keysym.sym = vk; }
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_AC_BACK) e.key.keysym.sym = SDLK_ESCAPE;   /* Android back button */
            if (e.type == SDL_KEYDOWN && !e.key.repeat)
            {
                SDL_Keycode k = e.key.keysym.sym;
                if (scr == S_MENU)
                {
                    if (k == SDLK_UP) sel = (sel + 6) % 7; else if (k == SDLK_DOWN) sel = (sel + 1) % 7;
                    else if (k == SDLK_ESCAPE) { result = -1; running = 0; }
                    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
                    {
                        last_err[0] = 0;
                        if (sel == 4) { result = 0; running = 0; }
                        else if (sel == 5) { audio_test_start(); }
                        else if (sel == 6) { result = -1; running = 0; }
                        else if (!connect_and_hello()) { /* last_err set */ }
                        else if (sel == 0 || sel == 1) { ns_send("HOST %d 4 %s", sel == 0 ? 1 : 0, pakid); scr = S_ROOM; }
                        else if (sel == 2) { scr = S_CODE; codelen = 0; code[0] = 0; }
                        else if (sel == 3) { rl_n = 0; rl_done = 0; lsel = 0; ns_send("LIST"); scr = S_LIST; }
                    }
                }
                else if (scr == S_CODE)
                {
                    if (k == SDLK_ESCAPE) scr = S_MENU;
                    else if (k == SDLK_BACKSPACE && codelen > 0) code[--codelen] = 0;
                    else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && codelen > 0) { ns_send("JOIN %s %s", code, pakid); scr = S_ROOM; }
                }
                else if (scr == S_LIST)
                {
                    if (k == SDLK_ESCAPE) scr = S_MENU;
                    else if (k == SDLK_UP && rl_n) lsel = (lsel + rl_n - 1) % rl_n; else if (k == SDLK_DOWN && rl_n) lsel = (lsel + 1) % rl_n;
                    else if (k == SDLK_F5) { rl_n = 0; rl_done = 0; ns_send("LIST"); }
                    else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && rl_n) { ns_send("JOIN %s %s", rl[lsel].code, pakid); scr = S_ROOM; }
                }
                else if (scr == S_ROOM)
                {
                    if (k == SDLK_ESCAPE) { ns_send("LEAVE"); room_code[0] = 0; my_slot = -1; memset(r_present, 0, sizeof r_present); scr = S_MENU; }
                    else if (k == SDLK_r && my_slot >= 0) ns_send("READY %d", r_ready[my_slot] ? 0 : 1);
                }
            }
        }
        ns_poll();
        if (sock == NS_INVALID && scr != S_MENU) { snprintf(last_err, sizeof last_err, "lost connection to the relay"); scr = S_MENU; room_code[0] = 0; my_slot = -1; }
        if (scr == S_ROOM && my_slot < 0 && last_err[0]) scr = S_MENU;           // join refused
        if (started) { result = 1; running = 0; }
        if (sock != NS_INVALID && now - lastping > 10000) { ns_send("PING"); lastping = now; }

        /* scripted start for automated tests: NETPLAY_AUTO=host | join:CODE */
        if (auto_mode && !auto_done)
        {
            if (!strcmp(auto_mode, "host") && scr == S_MENU && connect_and_hello()) { ns_send("HOST 0 4 %s", pakid); scr = S_ROOM; }
            else if (!strncmp(auto_mode, "join:", 5) && scr == S_MENU && connect_and_hello()) { ns_send("JOIN %s %s", auto_mode + 5, pakid); scr = S_ROOM; }
            if (scr == S_ROOM && my_slot >= 0)
            {
                int cnt = 0; for (i = 0; i < 4; i++) if (r_present[i]) cnt++;
                if (!r_ready[my_slot] && (my_slot != host_slot || cnt >= auto_need)) ns_send("READY 1");
                if (my_slot == host_slot && cnt >= auto_need) auto_done = 1;
            }
        }

        draw_header(scr == S_MENU ? "Each player needs the same game file." : scr == S_CODE ? "Type the 5-letter room code" : scr == S_LIST ? "Public games (F5 refresh)" : "Room");
        if (scr == S_MENU)
        {
            for (i = 0; i < 7; i++) { if (i == sel) lrect(8, 44 + i * 16 - 2, LW - 16, 14, C_SEL); ltext(16, 44 + i * 16, i == sel ? C_TXT : C_DIM, items[i]); }
            ltext(12, 158, C_DIM, "Tap an option (or Up/Down + Enter)");
            { char b[96]; snprintf(b, sizeof b, "Name: %s   Relay: %s:%d", cfg_name, cfg_host, cfg_port); ltext(12, 170, C_DIM, b); }
            if (last_err[0]) ltext(12, 184, C_HOT, last_err);
        }
        else if (scr == S_CODE)
        {
            char b[24]; snprintf(b, sizeof b, "%s%s", code, (now / 400) % 2 ? "_" : " ");
            ltext(16, 60, C_TXT, b); ltext(12, 100, C_DIM, "Tap here to open the keyboard");
            lrect(12, 120, 88, 22, C_SEL); ltext(24, 126, C_TXT, "Join (Enter)"); lrect(110, 120, 88, 22, C_SEL); ltext(122, 126, C_TXT, "Back (Esc)");
            if (last_err[0]) ltext(12, 182, C_HOT, last_err);
        }
        else if (scr == S_LIST)
        {
            if (!rl_n) ltext(16, 50, C_DIM, rl_done ? "No public games right now." : "Looking...");
            for (i = 0; i < rl_n; i++)
            {
                char b[96]; int same = !strcmp(rl[i].pak, pakid);
                snprintf(b, sizeof b, "%s  %s  %d/%d  %s", rl[i].code, rl[i].host, rl[i].n, rl[i].max, same ? "" : "(different game)");
                if (i == lsel) lrect(8, 44 + i * 14 - 2, LW - 16, 13, C_SEL);
                ltext(16, 44 + i * 14, same ? C_TXT : C_DIM, b);
            }
            lrect(12, 150, 88, 22, C_SEL); ltext(24, 156, C_TXT, "Back (Esc)"); lrect(110, 150, 88, 22, C_SEL); ltext(122, 156, C_TXT, "Refresh (F5)");
            if (last_err[0]) ltext(12, 182, C_HOT, last_err);
        }
        else
        {
            char b[96]; int cnt = 0;
            snprintf(b, sizeof b, "Room code: %s", room_code[0] ? room_code : "...");
            ltext(12, 40, C_TXT, b);
            for (i = 0; i < 4; i++)
            {
                if (r_present[i]) { cnt++; snprintf(b, sizeof b, "P%d  %s%s", i + 1, r_name[i], r_host[i] ? " (host)" : ""); ltext(16, 62 + i * 16, C_TXT, b); ltext(200, 62 + i * 16, r_ready[i] ? C_OK : C_DIM, r_ready[i] ? "READY" : "not ready"); }
                else { snprintf(b, sizeof b, "P%d  waiting...", i + 1); ltext(16, 62 + i * 16, C_DIM, b); }
            }
            ltext(12, 132, C_DIM, "Game starts when everyone is ready.");
            lrect(12, 150, 118, 22, my_slot >= 0 && r_ready[my_slot] ? 0x1c5a34u : C_SEL); ltext(24, 156, C_TXT, my_slot >= 0 && r_ready[my_slot] ? "READY! (R)" : "Ready up (R)");
            lrect(140, 150, 80, 22, C_SEL); ltext(152, 156, C_TXT, "Leave (Esc)");
            if (last_err[0]) ltext(12, 182, C_HOT, last_err);
            (void)cnt;
        }
        audio_test_tick();
        lpresent();
        SDL_Delay(16);
    }

    lobby_close();
    if (result == 1)
    {
        netplay.active = 1; netplay.slot = my_slot; netplay.mask = go_mask; netplay.delay = go_delay < 1 ? 1 : go_delay > 12 ? 12 : go_delay;
        netplay.frame = 0; netplay.seed = go_seed; netplay.dropped = 0; netplay.desync = 0;
        memset(valid, 0, sizeof valid); memset(inputs, 0, sizeof inputs); memset(myhash_ok, 0, sizeof myhash_ok); memset(rhash_ok, 0, sizeof rhash_ok);
        local_prev = 0; base_set = 0; fuzz_state = 12345u + (unsigned)my_slot * 7919u;
    }
    else if (sock != NS_INVALID) { ns_send("LEAVE"); ns_flush(); tr_close(); }
    return result;
}

/* ------------------------------------------------------------------ lockstep */
static unsigned long long fuzz_keys(unsigned frame, unsigned long long mask)
{
    /* deterministic pseudo-random button mashing for automated tests: hold a pattern for a few frames at a time */
    unsigned long long k = 0; int b;
    if (frame % 6 == 0) fuzz_state = fuzz_state * 1664525u + 1013904223u;
    for (b = 0; b < 16; b++) if (((fuzz_state >> (b + 3)) & 3) == 0 && ((mask >> b) & 1)) k |= 1ULL << b;
    return k & mask;
}

int netplay_exchange(unsigned long long local, unsigned long long out[4])
{
    unsigned f = netplay.frame; int s, waited = 0; Uint32 t0, now, target;
    if (!netplay.active) return 0;
    if (f == 0)
    {
        for (s = 0; s < 4; s++) if (netplay.mask & (1 << s)) { int k; for (k = 0; k < netplay.delay; k++) { inputs[s][k % RING] = 0; valid[s][k % RING] = 1; } }
    }
    if (fuzz) local = fuzz_keys(f, local ? local | 0x7ffULL : 0x7ffULL);
    inputs[netplay.slot][(f + netplay.delay) % RING] = local; valid[netplay.slot][(f + netplay.delay) % RING] = 1;
    ns_send("I %u %llx", f + netplay.delay, local);
    /* wait until every player's input for this frame is here */
    t0 = SDL_GetTicks();
    for (;;)
    {
        int missing = 0;
        ns_poll();
        for (s = 0; s < 4; s++) if ((netplay.mask & (1 << s)) && !(netplay.dropped & (1 << s)) && !valid[s][f % RING]) missing = 1;
        if (!missing) break;
        if (sock == NS_INVALID) { netplay.active = 0; fprintf(stderr, "[netplay] connection lost\n"); return 0; }
        { SDL_Event e; while (SDL_PollEvent(&e)) { if (e.type == SDL_QUIT) { netplay.active = 0; return 0; } } }
        netplay.stalled = 1; waited = 1;
        if (SDL_GetTicks() - t0 > 30000) { netplay.active = 0; fprintf(stderr, "[netplay] timed out waiting for another player\n"); return 0; }
        SDL_Delay(1);
    }
    netplay.stalled = waited;
    for (s = 0; s < 4; s++)
    {
        out[s] = ((netplay.mask & (1 << s)) && valid[s][f % RING]) ? inputs[s][f % RING] : 0;
        valid[s][f % RING] = 0;
    }
    netplay.frame = f + 1;
    if ((f % 250) == 0 && getenv("NETPLAY_VERBOSE")) fprintf(stderr, "[netplay] slot %d frame %u\n", netplay.slot, f);
    /* pace to a steady 50 frames per second so every PC plays at the same speed */
    now = SDL_GetTicks();
    if (!base_set) { base_ms = now; base_set = 1; }
    target = base_ms + (f * 20u);
    if (now + 200 < target) { base_ms = now - f * 20u; target = now; }
    if ((int)(now - target) > 100) base_ms = now - f * 20u;         // fell behind (stall): do not race to catch up
    else if (now < target && !getenv("NETPLAY_NOSLEEP")) SDL_Delay(target - now);
    return 1;
}

void netplay_check(unsigned hash)
{
    unsigned f = netplay.frame;
    if (!netplay.active || f == 0 || (f % 50) != 0) return;
    { int rs; myhash[f % RING] = hash; myhash_ok[f % RING] = 1;
      for (rs = 0; rs < 4; rs++) if (rhash_ok[rs][f % RING] && rhash[rs][f % RING] != hash && !netplay.desync)
      { netplay.desync = 1; fprintf(stderr, "[netplay] DESYNC at frame %u: player %d has a different game state. Everyone needs the same game file and save data.\n", f, rs + 1); } }
    ns_send("C %u %x", f, hash);
}

void netplay_shutdown(void)
{
    if (sock != NS_INVALID) { ns_send("LEAVE"); ns_flush(); tr_close(); }
    netplay.active = 0;
}
