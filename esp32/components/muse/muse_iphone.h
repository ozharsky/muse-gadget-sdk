/*
 * The iPhone link: Apple's own Bluetooth services, no app on the phone.
 *   ANCS -- the phone's notifications appear on the gadget (a chime and a caption).
 *   AMS  -- now playing (track, artist, playing/paused) and remote control of the player.
 * On in Settings > Bluetooth > iPhone: the gadget advertises until the phone pairs (the passkey
 * shows on the glass); after that the phone reconnects on its own whenever it is near.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { MUSE_IPHONE_OFF, MUSE_IPHONE_WAITING, MUSE_IPHONE_PAIRING, MUSE_IPHONE_CONNECTED } muse_iphone_state_t;

typedef struct {
    bool active;          /* AMS reported a track */
    bool playing;
    char title[64];
    char artist[48];
} muse_now_playing_t;

/* AMS remote commands (the Apple spec's numbers). */
enum { MUSE_MEDIA_PLAY = 0, MUSE_MEDIA_PAUSE = 1, MUSE_MEDIA_TOGGLE = 2, MUSE_MEDIA_NEXT = 3,
       MUSE_MEDIA_PREV = 4, MUSE_MEDIA_VOL_UP = 5, MUSE_MEDIA_VOL_DOWN = 6 };

void muse_iphone_start(void);                 /* once, after the radios are handed their settings */
void muse_iphone_apply(void);                 /* the setting changed */
muse_iphone_state_t muse_iphone_state(void);
uint32_t muse_iphone_gen(void);               /* bumps when now-playing changes */
bool muse_iphone_now_playing(muse_now_playing_t *out);
bool muse_iphone_media(int cmd);              /* any task */
void muse_iphone_bench_now(const char *title, const char *artist, bool playing);   /* ">media.demo=" */
