/*
 * Doom sound backend for MOSS — SB16 via kernel syscalls.
 *
 * Software-mixes up to 16 channels of 8-bit unsigned PCM from WAD
 * lumps, and feeds unsigned 8-bit mono output to the SB16 ring
 * buffer through SYS_AUDIO_WRITE.
 *
 * Also provides a stub music module (no music playback).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "deh_str.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
#include "doomtype.h"

#include <moss/audio.h>

/* --------------------------------------------------------------- */
/*  Configuration                                                  */
/* --------------------------------------------------------------- */

#define NUM_CHANNELS    16
#define MIX_RATE        11025   /* Hz — matches most Doom SFX       */
#define MIX_BUF_SAMPLES 512    /* samples per Update() batch        */

/* --------------------------------------------------------------- */
/*  Channel state                                                  */
/* --------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;        /* unsigned 8-bit PCM samples       */
    uint32_t       length;      /* total samples                    */
    uint32_t       pos;         /* current playback position        */
    int            vol;         /* 0-127                            */
    int            sep;         /* 0-254 (128 = centre)             */
    int            active;
    sfxinfo_t     *sfx;         /* back-pointer for SoundIsPlaying  */
} channel_t;

static channel_t channels[NUM_CHANNELS];
static boolean   sound_inited = false;
static boolean   use_prefix;

/* Output mixing buffer (unsigned 8-bit) */
static uint8_t mix_buf[MIX_BUF_SAMPLES];

/* libsamplerate stubs (required by other Doom code) */
int   use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
char *timidity_cfg_path = "";

/* --------------------------------------------------------------- */
/*  SFX cache                                                      */
/* --------------------------------------------------------------- */

/* Cached decoded sound — stored in sfxinfo->driver_data */
typedef struct {
    uint8_t  *samples;   /* unsigned 8-bit PCM (allocated via Z_Malloc) */
    uint32_t  length;    /* number of samples                           */
    uint32_t  rate;      /* original sample rate                        */
} cached_sfx_t;

static boolean CacheSFX(sfxinfo_t *sfx)
{
    int lumpnum = sfx->lumpnum;
    byte *data  = W_CacheLumpNum(lumpnum, PU_STATIC);
    int lumplen = W_LumpLength(lumpnum);

    /* Validate DMX header: 0x03 0x00, then 16-bit rate, 32-bit length */
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    uint32_t rate   = (uint32_t)data[2] | ((uint32_t)data[3] << 8);
    uint32_t length = (uint32_t)data[4] | ((uint32_t)data[5] << 8)
                    | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

    if (length > (uint32_t)(lumplen - 8) || length <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    /* Skip 16-byte DMX padding at start and end */
    byte *pcm     = data + 8 + 16;
    uint32_t plen = length - 32;

    cached_sfx_t *c = Z_Malloc(sizeof(*c), PU_STATIC, NULL);
    c->samples = Z_Malloc(plen, PU_STATIC, NULL);
    c->length  = plen;
    c->rate    = rate;
    memcpy(c->samples, pcm, plen);

    sfx->driver_data = c;

    W_ReleaseLumpNum(lumpnum);
    return true;
}

/* --------------------------------------------------------------- */
/*  Lump name helper                                               */
/* --------------------------------------------------------------- */

static void GetSfxLumpName(sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    if (sfx->link != NULL)
        sfx = sfx->link;

    if (use_prefix)
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    else
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
}

/* --------------------------------------------------------------- */
/*  sound_module_t implementation                                  */
/* --------------------------------------------------------------- */

static boolean I_MOSS_InitSound(boolean _use_sfx_prefix)
{
    use_prefix = _use_sfx_prefix;

    for (int i = 0; i < NUM_CHANNELS; i++)
        channels[i].active = 0;

    /* Start kernel SB16 playback */
    moss_audio_start(MIX_RATE);

    sound_inited = true;
    return true;
}

static void I_MOSS_ShutdownSound(void)
{
    sound_inited = false;
}

static int I_MOSS_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];
    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void I_MOSS_UpdateSound(void)
{
    if (!sound_inited)
        return;

    /* Produce as many samples as the ring buffer can accept,
     * up to MIX_BUF_SAMPLES per call. */
    uint32_t space = moss_audio_avail();
    uint32_t samples = space;
    if (samples > MIX_BUF_SAMPLES)
        samples = MIX_BUF_SAMPLES;
    if (samples == 0)
        return;

    /* Mix all active channels */
    for (uint32_t i = 0; i < samples; i++) {
        int32_t mixed = 0;

        for (int ch = 0; ch < NUM_CHANNELS; ch++) {
            if (!channels[ch].active)
                continue;
            if (channels[ch].pos >= channels[ch].length) {
                channels[ch].active = 0;
                continue;
            }

            /* Signed sample: centre unsigned 8-bit around 0 */
            int32_t s = (int32_t)channels[ch].data[channels[ch].pos] - 128;
            /* Scale by volume (0-127) */
            s = s * channels[ch].vol / 127;
            mixed += s;

            channels[ch].pos++;
        }

        /* Clamp back to unsigned 8-bit */
        mixed += 128;
        if (mixed < 0)   mixed = 0;
        if (mixed > 255) mixed = 255;
        mix_buf[i] = (uint8_t)mixed;
    }

    moss_audio_write(mix_buf, samples);
}

static void I_MOSS_UpdateSoundParams(int handle, int vol, int sep)
{
    if (!sound_inited || handle < 0 || handle >= NUM_CHANNELS)
        return;
    channels[handle].vol = vol;
    channels[handle].sep = sep;
}

static int I_MOSS_StartSound(sfxinfo_t *sfxinfo, int channel,
                             int vol, int sep)
{
    if (!sound_inited || channel < 0 || channel >= NUM_CHANNELS)
        return -1;

    /* Stop any existing sound on this channel */
    channels[channel].active = 0;

    /* Cache the sound if not already done */
    if (sfxinfo->driver_data == NULL) {
        if (!CacheSFX(sfxinfo))
            return -1;
    }

    cached_sfx_t *c = sfxinfo->driver_data;

    channels[channel].data   = c->samples;
    channels[channel].length = c->length;
    channels[channel].pos    = 0;
    channels[channel].vol    = vol;
    channels[channel].sep    = sep;
    channels[channel].sfx    = sfxinfo;
    channels[channel].active = 1;

    return channel;
}

static void I_MOSS_StopSound(int handle)
{
    if (handle < 0 || handle >= NUM_CHANNELS)
        return;
    channels[handle].active = 0;
}

static boolean I_MOSS_SoundIsPlaying(int handle)
{
    if (handle < 0 || handle >= NUM_CHANNELS)
        return false;
    return channels[handle].active ? true : false;
}

static void I_MOSS_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    char namebuf[9];

    for (int i = 0; i < num_sounds; i++) {
        GetSfxLumpName(&sounds[i], namebuf, sizeof(namebuf));
        sounds[i].lumpnum = W_CheckNumForName(namebuf);
        if (sounds[i].lumpnum != -1)
            CacheSFX(&sounds[i]);
    }
}

/* --------------------------------------------------------------- */
/*  Module descriptor                                              */
/* --------------------------------------------------------------- */

static snddevice_t sound_moss_devices[] = {
    SNDDEVICE_SB,
};

sound_module_t DG_sound_module = {
    sound_moss_devices,
    arrlen(sound_moss_devices),
    I_MOSS_InitSound,
    I_MOSS_ShutdownSound,
    I_MOSS_GetSfxLumpNum,
    I_MOSS_UpdateSound,
    I_MOSS_UpdateSoundParams,
    I_MOSS_StartSound,
    I_MOSS_StopSound,
    I_MOSS_SoundIsPlaying,
    I_MOSS_CacheSounds,
};

/* --------------------------------------------------------------- */
/*  Stub music module (no music playback yet)                      */
/* --------------------------------------------------------------- */

static boolean I_MOSS_InitMusic(void)          { return true; }
static void    I_MOSS_ShutdownMusic(void)      { }
static void    I_MOSS_SetMusicVolume(int v)    { (void)v; }
static void    I_MOSS_PauseMusic(void)         { }
static void    I_MOSS_ResumeMusic(void)        { }
static void   *I_MOSS_RegisterSong(void *data, int len)
                                               { (void)data; (void)len; return (void *)1; }
static void    I_MOSS_UnRegisterSong(void *h)  { (void)h; }
static void    I_MOSS_PlaySong(void *h, boolean l)
                                               { (void)h; (void)l; }
static void    I_MOSS_StopSong(void)           { }
static boolean I_MOSS_MusicIsPlaying(void)     { return false; }

static snddevice_t music_moss_devices[] = {
    SNDDEVICE_SB,
};

music_module_t DG_music_module = {
    music_moss_devices,
    arrlen(music_moss_devices),
    I_MOSS_InitMusic,
    I_MOSS_ShutdownMusic,
    I_MOSS_SetMusicVolume,
    I_MOSS_PauseMusic,
    I_MOSS_ResumeMusic,
    I_MOSS_RegisterSong,
    I_MOSS_UnRegisterSong,
    I_MOSS_PlaySong,
    I_MOSS_StopSong,
    I_MOSS_MusicIsPlaying,
    NULL,   /* Poll */
};
