/*
 * OpenBOR Online - lockstep netplay with a room-code relay.
 * Every player runs the same game; only button presses travel over the network.
 */
#ifndef NETPLAY_H
#define NETPLAY_H

typedef struct
{
    int active;                 // lockstep is running
    int slot;                   // my player slot (0-3)
    int mask;                   // slots that are playing
    int dropped;                // slots that left mid-game
    int delay;                  // input delay in frames
    unsigned frame;             // next frame to run
    unsigned seed;              // shared random seed
    int desync;                 // a checksum mismatch was seen
    int stalled;                // waiting for another player right now
} NetState;

extern NetState netplay;

#define NET_STEP_TICKS 4        // one lockstep frame = 4 engine ticks (200 ticks per second) = 50 frames per second

int  netplay_enabled(void);                                    // netplay.ini next to the game, or OPENBOR_NET=1
void netplay_pakid(const char *path, char *out, int outlen);   // short fingerprint of the game file
int  netplay_lobby(const char *pakid);                         // 1 = online game starting, 0 = play offline, -1 = quit
int  netplay_exchange(unsigned long long local, unsigned long long out[4]);   // one lockstep frame; 0 = connection lost
void netplay_check(unsigned hash);                             // call after each frame with a hash of the game state
void netplay_shutdown(void);

#endif
