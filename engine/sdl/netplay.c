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
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "netplay.h"
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

static void load_config(void)
{
    FILE *f = open_ini(); char line[200];
    const char *e;
    if (f)
    {
        while (fgets(line, sizeof line, f))
        {
            trim(line);
            if (!strncmp(line, "server=", 7))
            {
                char *c = strrchr(line + 7, ':');
                if (c) { *c = 0; cfg_port = atoi(c + 1); }
                snprintf(cfg_host, sizeof cfg_host, "%s", line + 7);
            }
            else if (!strncmp(line, "name=", 5)) snprintf(cfg_name, sizeof cfg_name, "%.16s", line + 5);
        }
        fclose(f);
    }
    if ((e = getenv("OPENBOR_RELAY")) && *e) { char tmp[128]; char *c; snprintf(tmp, sizeof tmp, "%s", e); c = strrchr(tmp, ':'); if (c) { *c = 0; cfg_port = atoi(c + 1); } snprintf(cfg_host, sizeof cfg_host, "%s", tmp); }
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
        ns_close(sock); sock = NS_INVALID;
    }
    freeaddrinfo(res);
    if (sock == NS_INVALID) { snprintf(last_err, sizeof last_err, "cannot connect to %s:%d", cfg_host, cfg_port); return 0; }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    ns_nonblock(sock); inlen = outlen = 0;
    return 1;
}

static void ns_flush(void)
{
    while (outlen > 0 && sock != NS_INVALID)
    {
        int n = (int)send(sock, outbuf, outlen, 0);
        if (n > 0) { memmove(outbuf, outbuf + n, outlen - n); outlen -= n; }
        else if (n < 0 && ns_wouldblock()) break;
        else { ns_close(sock); sock = NS_INVALID; break; }
    }
}

static void ns_send(const char *fmt, ...)
{
    char line[200]; int n; va_list ap;
    va_start(ap, fmt); vsnprintf(line, sizeof line - 2, fmt, ap); va_end(ap);
    n = (int)strlen(line); line[n++] = '\n';
    if (outlen + n > (int)sizeof outbuf) { ns_flush(); if (outlen + n > (int)sizeof outbuf) return; }
    memcpy(outbuf + outlen, line, n); outlen += n; ns_flush();
}

static void handle_line(char *line);
static void ns_poll(void)
{
    char tmp[4096]; int n;
    if (sock == NS_INVALID) return;
    ns_flush();
    for (;;)
    {
        n = (int)recv(sock, tmp, sizeof tmp, 0);
        if (n > 0)
        {
            if (inlen + n > (int)sizeof inbuf) n = (int)sizeof inbuf - inlen;
            memcpy(inbuf + inlen, tmp, n); inlen += n;
        }
        else if (n == 0) { ns_close(sock); sock = NS_INVALID; break; }
        else { if (!ns_wouldblock()) { ns_close(sock); sock = NS_INVALID; } break; }
    }
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
    SDL_Surface *ws = SDL_GetWindowSurface(lwin);
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
    lwin = SDL_CreateWindow("OpenBOR Online", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, LW * 3, LH * 3, SDL_WINDOW_SHOWN);
    if (!lwin) return 0;
    lsurf = SDL_CreateRGBSurfaceWithFormat(0, LW, LH, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!lsurf) return 0;
    SDL_StartTextInput();
    return 1;
}
static void lobby_close(void)
{
    SDL_StopTextInput();
    if (lsurf) { SDL_FreeSurface(lsurf); lsurf = NULL; }
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

int netplay_lobby(const char *pakid)
{
    const char *items[] = { "Host a public game", "Host a private game", "Join with a room code", "Browse public games", "Play offline", "Quit" };
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
                int ww = LW, wh = LH, tx, ty, vk = 0; SDL_GetWindowSize(lwin, &ww, &wh);
                tx = e.button.x * LW / (ww > 0 ? ww : LW); ty = e.button.y * LH / (wh > 0 ? wh : LH);
                if (scr == S_MENU) { for (i = 0; i < 6; i++) if (ty >= 42 + i * 16 && ty < 42 + i * 16 + 15) { sel = i; vk = SDLK_RETURN; } }
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
                    if (k == SDLK_UP) sel = (sel + 5) % 6; else if (k == SDLK_DOWN) sel = (sel + 1) % 6;
                    else if (k == SDLK_ESCAPE) { result = -1; running = 0; }
                    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
                    {
                        last_err[0] = 0;
                        if (sel == 4) { result = 0; running = 0; }
                        else if (sel == 5) { result = -1; running = 0; }
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
            for (i = 0; i < 6; i++) { if (i == sel) lrect(8, 44 + i * 16 - 2, LW - 16, 14, C_SEL); ltext(16, 44 + i * 16, i == sel ? C_TXT : C_DIM, items[i]); }
            ltext(12, 150, C_DIM, "Tap an option (or Up/Down + Enter)");
            { char b[96]; snprintf(b, sizeof b, "Name: %s   Relay: %s:%d", cfg_name, cfg_host, cfg_port); ltext(12, 166, C_DIM, b); }
            if (last_err[0]) ltext(12, 182, C_HOT, last_err);
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
    else if (sock != NS_INVALID) { ns_send("LEAVE"); ns_flush(); ns_close(sock); sock = NS_INVALID; }
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
    if (sock != NS_INVALID) { ns_send("LEAVE"); ns_flush(); ns_close(sock); sock = NS_INVALID; }
    netplay.active = 0;
}
