/* audio_sdl.h - SDL2 output backend for the pure-C audio engine.
 *
 * Only this file needs SDL. The game loop creates one od_audio_sdl, calls
 * od_audio_sdl_open() with the assets directory, and then drives music/SFX
 * through the wrappers below (which lock the audio device so the callback
 * thread never sees a torn state).
 */
#ifndef OD_AUDIO_SDL_H
#define OD_AUDIO_SDL_H

#include "opendana/audio.h"
#include "opendana/ext/state.h"

#include <SDL.h>

typedef struct {
    od_audio audio;
    SDL_AudioDeviceID dev;
    int out_rate;     /* device rate; may differ from OD_AUDIO_RATE */

    /* Linear resampler state used when out_rate != OD_AUDIO_RATE. */
    int16_t prev, next;
    double  phase;
    int     started;  /* the interpolator has pulled its first pair */

    /* C5.1: gain applied to every sample handed to the device. 1000 = unity,
     * 0 = silence (mute). The simulation and the core PCM never see it. */
    int     gain_permille;
} od_audio_sdl;

/* Load extracted sequence data and open a mono S16 device. */
od_status od_audio_sdl_open(od_audio_sdl *as, const char *assets_dir);
void od_audio_sdl_close(od_audio_sdl *as);

/* Pause/resume the device (pause=1 silences output). */
void od_audio_sdl_pause(od_audio_sdl *as, int pause);

/* C5.1: set the output gain in permille (0 = mute, 1000 = unity). Applied to
 * the samples just before they reach the device; the simulation is untouched. */
void od_audio_sdl_set_gain(od_audio_sdl *as, int gain_permille);

/* Thread-safe wrappers around the core engine. */
void od_audio_sdl_play_music(od_audio_sdl *as, int track);
void od_audio_sdl_stop_music(od_audio_sdl *as);
void od_audio_sdl_play_sfx(od_audio_sdl *as, int sfx);
/* Once at tick start; consumes events accepted during the previous tick. */
void od_audio_sdl_tick(od_audio_sdl *as);
/* Start-up register writes; see od_audio_reset_init(). */
void od_audio_sdl_reset_init(od_audio_sdl *as);
void od_audio_sdl_accepted_sfx(od_audio_sdl *as, int sfx, int accepted);
/* Preferred for the game's confirmed sound_request from the Hero script. */
od_status od_audio_sdl_request(od_audio_sdl *as, unsigned request);

/* Snapshot the actual SDL sequencer, its pending PCM, and interpolator under
 * the device lock. Load replaces (does not append to) post-save queued audio.
 * Device/gain/observers remain live; no nested SDL locking in state.c. */
od_status od_audio_sdl_state_save(od_audio_sdl *as, const od_flow *f,
    const char *path, const od_state_context *identity);
od_status od_audio_sdl_state_load(od_audio_sdl *as, od_flow *f,
    const char *path, const od_state_context *identity);

/* C6.34: save the live engine before a nested run on the same device and put it
 * back after, so the interrupted game's audio resumes as if the run never was.
 * Gain and observers are not part of it. NULL when there is no engine. */
typedef struct od_audio_sdl_checkpoint od_audio_sdl_checkpoint;
od_audio_sdl_checkpoint *od_audio_sdl_checkpoint_save(od_audio_sdl *as);
/* Restore returns the hash of the engine state right after the restore (taken
 * under the same lock); it equals od_audio_sdl_checkpoint_hash(cp) when the
 * state came back exactly. */
uint64_t od_audio_sdl_checkpoint_restore(od_audio_sdl *as,
                                          const od_audio_sdl_checkpoint *cp);
uint64_t od_audio_sdl_checkpoint_hash(const od_audio_sdl_checkpoint *cp);
void od_audio_sdl_checkpoint_free(od_audio_sdl_checkpoint *cp);

#endif /* OD_AUDIO_SDL_H */
