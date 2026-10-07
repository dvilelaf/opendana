/* audio_sdl.c - SDL2 output backend for the pure-C audio engine.
 *
 * The callback consumes samples produced by simulation ticks. When the device
 * cannot open at OD_AUDIO_RATE we fall back to a rate the driver supports
 * (48 kHz) and linearly resample, so the game is never silent for want of an
 * exact sample rate.
 */
#include "opendana/audio_sdl.h"
#include "opendana/ext/settings.h"

#include <stdlib.h>
#include <string.h>

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    od_audio_sdl *as = (od_audio_sdl *)userdata;
    int16_t *out = (int16_t *)stream;
    int frames = len / (int)sizeof(int16_t);

    if (as->out_rate == OD_AUDIO_RATE) {
        od_audio_render(&as->audio, out, frames);
    } else {
        /* Linear interpolation from OD_AUDIO_RATE to as->out_rate. The device
         * callback never advances the engine clock; it only consumes the
         * samples the simulation already produced, so a underrun stays
         * silence. */
        double step = (double)OD_AUDIO_RATE / (double)as->out_rate;
        if (!as->started) {
            /* Prime prev/next from real samples so the first output is not
             * zero. */
            od_audio_render(&as->audio, &as->next, 1);
            as->prev = as->next;
            as->started = 1;
        }
        for (int i = 0; i < frames; i++) {
            int v = as->prev + (int)((as->next - as->prev) * as->phase);
            out[i] = (int16_t)v;
            as->phase += step;
            while (as->phase >= 1.0) {
                as->prev = as->next;
                od_audio_render(&as->audio, &as->next, 1);
                as->phase -= 1.0;
            }
        }
    }

    /* C5.1 mute/volume: only the samples handed to the device are scaled. The
     * engine already produced them, so the simulation state and the PCM used
     * by `--hash-out`/`--audio-out` are identical muted or not. */
    od_audio_apply_gain(out, (size_t)frames, as->gain_permille);
}

void od_audio_sdl_set_gain(od_audio_sdl *as, int gain_permille)
{
    if (as == NULL)
        return;
    if (gain_permille < 0)
        gain_permille = 0;
    if (gain_permille > 1000)
        gain_permille = 1000;
    if (as->dev != 0)
        SDL_LockAudioDevice(as->dev);
    as->gain_permille = gain_permille;
    if (as->dev != 0)
        SDL_UnlockAudioDevice(as->dev);
}

void od_audio_sdl_tick(od_audio_sdl *as)
{
    if (as == NULL) return;
    if (as->dev != 0) SDL_LockAudioDevice(as->dev);
    od_audio_tick(&as->audio);
    if (as->dev != 0) SDL_UnlockAudioDevice(as->dev);
}

void od_audio_sdl_accepted_sfx(od_audio_sdl *as, int sfx, int accepted)
{
    if (accepted) od_audio_sdl_play_sfx(as, sfx);
}

void od_audio_sdl_reset_init(od_audio_sdl *as)
{
    if (as == NULL) return;
    if (as->dev != 0) SDL_LockAudioDevice(as->dev);
    od_audio_reset_init(&as->audio);
    if (as->dev != 0) SDL_UnlockAudioDevice(as->dev);
}

od_status od_audio_sdl_request(od_audio_sdl *as, unsigned request)
{
    if (as == NULL) return OD_ERR_FORMAT;
    if (as->dev != 0) SDL_LockAudioDevice(as->dev);
    od_status result = od_audio_request(&as->audio, request);
    if (as->dev != 0) SDL_UnlockAudioDevice(as->dev);
    return result;
}

od_status od_audio_sdl_open(od_audio_sdl *as, const char *assets_dir)
{
    if (as == NULL)
        return OD_ERR_FORMAT;
    memset(as, 0, sizeof(*as));
    as->dev = 0;
    as->gain_permille = 1000; /* unity until the app applies the settings */

    od_status st = od_audio_init(&as->audio, assets_dir);
    if (st != OD_OK)
        return st;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        od_audio_shutdown(&as->audio);
        return OD_ERR_IO;
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = OD_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = as;

    as->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (as->dev == 0) {
        /* Retry at a common device rate and resample, so the game is never
         * silent for want of an exact 48 kHz stream. */
        want.freq = 44100;
        as->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    }
    if (as->dev == 0) {
        od_audio_shutdown(&as->audio);
        return OD_ERR_IO;
    }

    as->out_rate = have.freq;
    /* The device callback owns no time: it only drains samples the simulation
     * already produced. The resampler primes itself on its first call. */
    as->prev = as->next = 0;
    as->phase = 0.0;
    as->started = 0;
    SDL_PauseAudioDevice(as->dev, 0);
    return OD_OK;
}

void od_audio_sdl_close(od_audio_sdl *as)
{
    if (as == NULL)
        return;
    if (as->dev != 0) {
        SDL_CloseAudioDevice(as->dev);
        as->dev = 0;
    }
    od_audio_shutdown(&as->audio);
}

void od_audio_sdl_pause(od_audio_sdl *as, int pause)
{
    if (as != NULL && as->dev != 0)
        SDL_PauseAudioDevice(as->dev, pause ? 1 : 0);
}

void od_audio_sdl_play_music(od_audio_sdl *as, int track)
{
    if (as == NULL)
        return;
    if (as->dev != 0)
        SDL_LockAudioDevice(as->dev);
    od_audio_play_music(&as->audio, track);
    if (as->dev != 0)
        SDL_UnlockAudioDevice(as->dev);
}

void od_audio_sdl_stop_music(od_audio_sdl *as)
{
    if (as == NULL)
        return;
    if (as->dev != 0)
        SDL_LockAudioDevice(as->dev);
    od_audio_stop_music(&as->audio);
    if (as->dev != 0)
        SDL_UnlockAudioDevice(as->dev);
}

void od_audio_sdl_play_sfx(od_audio_sdl *as, int sfx)
{
    if (as == NULL)
        return;
    if (as->dev != 0)
        SDL_LockAudioDevice(as->dev);
    od_audio_play_sfx(&as->audio, sfx);
    if (as->dev != 0)
        SDL_UnlockAudioDevice(as->dev);
}

/* Only pointer-free mutable backend data. The audio engine's own snapshot
 * contains the pending ring, requests and synthesis/sequence clocks; its load
 * API retains live observer callbacks. Device policy (gain) is not a save. */
typedef struct {
    od_audio audio;
    int out_rate, started;
    int16_t prev, next;
    double phase;
} sdl_audio_state;

static od_status sdl_state(od_audio_sdl *as, od_flow *f,
    const char *path, const od_state_context *identity, int load)
{
    if (!as || !f || !path || !identity) return OD_ERR_RANGE;
    if (as->dev) SDL_LockAudioDevice(as->dev);
    sdl_audio_state snapshot;
    memset(&snapshot,0,sizeof snapshot);
    snapshot.audio = as->audio; snapshot.audio.engine = NULL;
    snapshot.out_rate = as->out_rate; snapshot.started = as->started;
    snapshot.prev = as->prev; snapshot.next = as->next; snapshot.phase = as->phase;
    od_state_context ctx = *identity;
    ctx.audio = &as->audio; ctx.host = &snapshot; ctx.host_size = sizeof snapshot;
    od_status result = load ? od_state_load_ex(f,path,&ctx) :
                              od_state_save_ex(f,path,&ctx);
    if (load && result == OD_OK) {
        od_audio_engine *engine = as->audio.engine;
        as->audio = snapshot.audio; as->audio.engine = engine;
        /* A saved interpolator is valid only at its saved rate. A different
         * device primes from the restored source queue, never future samples. */
        if (as->out_rate == snapshot.out_rate) {
            as->started = snapshot.started; as->prev = snapshot.prev;
            as->next = snapshot.next; as->phase = snapshot.phase;
        } else {
            as->started = 0; as->prev = as->next = 0; as->phase = 0;
        }
    }
    if (as->dev) SDL_UnlockAudioDevice(as->dev);
    return result;
}
od_status od_audio_sdl_state_save(od_audio_sdl *as, const od_flow *f,
    const char *path, const od_state_context *identity)
{ return sdl_state(as,(od_flow *)f,path,identity,0); }
od_status od_audio_sdl_state_load(od_audio_sdl *as, od_flow *f,
    const char *path, const od_state_context *identity)
{ return sdl_state(as,f,path,identity,1); }

/* C6.34: checkpoint of the live engine, so a nested run (the editor's Test, the
 * catalogue's Play) can use the same device and engine and hand it back as it
 * found it. The engine block holds the sequencer, the synthesis clocks and the
 * pending PCM ring; the scalars and the interpolator are copied beside it. The
 * device, the gain and the observers stay live and are not part of it. */
struct od_audio_sdl_checkpoint {
    void *engine;
    size_t size;
    uint64_t hash;
    od_audio audio;
    int out_rate, started;
    int16_t prev, next;
    double phase;
};

/* FNV-1a over the engine block, the counters and the interpolator. The caller
 * holds the device lock, so the callback cannot move the state meanwhile. */
static uint64_t live_hash(const od_audio_sdl *as, void *scratch)
{
    uint64_t h = 1469598103934665603ull;
    const unsigned char *b = scratch;
    od_audio_state_save(&as->audio, scratch);
    for (size_t i = 0; i < od_audio_state_size(); i++)
        h = (h ^ b[i]) * 1099511628211ull;
    uint64_t v[6] = { as->audio.ticks, as->audio.samples, as->audio.overruns,
                      (uint64_t)as->started, (uint64_t)(uint16_t)as->prev << 16 |
                      (uint16_t)as->next, 0 };
    memcpy(&v[5], &as->phase, sizeof v[5]);
    b = (const unsigned char *)v;
    for (size_t i = 0; i < sizeof v; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

uint64_t od_audio_sdl_checkpoint_hash(const od_audio_sdl_checkpoint *cp)
{
    return cp ? cp->hash : 0;
}

od_audio_sdl_checkpoint *od_audio_sdl_checkpoint_save(od_audio_sdl *as)
{
    if (as == NULL || as->audio.engine == NULL)
        return NULL;
    od_audio_sdl_checkpoint *cp = calloc(1, sizeof *cp);
    if (cp == NULL)
        return NULL;
    cp->size = od_audio_state_size();
    cp->engine = malloc(cp->size);
    if (cp->engine == NULL) {
        free(cp);
        return NULL;
    }
    if (as->dev != 0) SDL_LockAudioDevice(as->dev);
    od_audio_state_save(&as->audio, cp->engine);
    cp->audio = as->audio;
    cp->out_rate = as->out_rate; cp->started = as->started;
    cp->prev = as->prev; cp->next = as->next; cp->phase = as->phase;
    cp->hash = live_hash(as, cp->engine);
    if (as->dev != 0) SDL_UnlockAudioDevice(as->dev);
    return cp;
}

uint64_t od_audio_sdl_checkpoint_restore(od_audio_sdl *as,
                                          const od_audio_sdl_checkpoint *cp)
{
    if (as == NULL || cp == NULL || as->audio.engine == NULL)
        return 0;
    if (as->dev != 0) SDL_LockAudioDevice(as->dev);
    od_audio_engine *engine = as->audio.engine;
    od_audio_state_load(&as->audio, cp->engine);
    as->audio = cp->audio;
    as->audio.engine = engine;
    as->started = cp->started; as->prev = cp->prev;
    as->next = cp->next; as->phase = cp->phase;
    void *scratch = malloc(cp->size);
    uint64_t h = scratch ? live_hash(as, scratch) : 0;
    free(scratch);
    if (as->dev != 0) SDL_UnlockAudioDevice(as->dev);
    return h;
}

void od_audio_sdl_checkpoint_free(od_audio_sdl_checkpoint *cp)
{
    if (cp == NULL)
        return;
    free(cp->engine);
    free(cp);
}
