/*
 * Copyright (C) 2007 Mark Hills <mark@pogo.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#include <stdio.h>
#include <string.h>
#include <sys/poll.h>
#include <alsa/asoundlib.h>

#include "device.h"
#include "timecoder.h"
#include "player.h"


/* This structure doesn't have corresponding functions to be an
 * abstraction of the ALSA calls; it is merely a container for these
 * variables. */

struct alsa_pcm_t {
    snd_pcm_t *pcm;

    struct pollfd *pe;
    int pe_count; /* number of pollfd entries */

    signed short *buf;
    snd_pcm_uframes_t period;
};


struct alsa_t {
    struct alsa_pcm_t capture, playback;
};


static void alsa_error(int r)
{
    fputs("alsa: ", stderr);
    fputs(snd_strerror(r), stderr);
    fputc('\n', stderr);
}


static int pcm_open(struct alsa_pcm_t *alsa, const char *device_name,
                    snd_pcm_stream_t stream, int buffer_time)
{
    int r, dir;
    unsigned int p;
    snd_pcm_hw_params_t *hw_params;
    
    r = snd_pcm_open(&alsa->pcm, device_name, stream, SND_PCM_NONBLOCK);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }

    r = snd_pcm_hw_params_malloc(&hw_params);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }

    r = snd_pcm_hw_params_any(alsa->pcm, hw_params);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }
    
    r = snd_pcm_hw_params_set_access(alsa->pcm, hw_params,
                                     SND_PCM_ACCESS_RW_INTERLEAVED);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }
    
    r = snd_pcm_hw_params_set_format(alsa->pcm, hw_params,
                                     SND_PCM_FORMAT_S16_LE);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }

    r = snd_pcm_hw_params_set_rate(alsa->pcm, hw_params, DEVICE_RATE, 0);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }
    
    r = snd_pcm_hw_params_set_channels(alsa->pcm, hw_params, DEVICE_CHANNELS);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }

    p = buffer_time * 1000; /* microseconds */
    dir = 0;
    r = snd_pcm_hw_params_set_buffer_time_near(alsa->pcm, hw_params, &p, &dir);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }

    if(p > buffer_time * 1000 + 500) {
        fprintf(stderr, "Buffer time of %dms is too small (try %dms).\n",
                buffer_time, p / 1000);
        return -1;
    }

    r = snd_pcm_hw_params(alsa->pcm, hw_params);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }
    
    r = snd_pcm_hw_params_get_period_size(hw_params, &alsa->period, &dir);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }

    snd_pcm_hw_params_free(hw_params);

    alsa->buf = malloc(alsa->period * DEVICE_CHANNELS * sizeof(signed short));
    if(!alsa->buf) {
        perror("malloc");
        return -1;
    }

    return 0;
}


static int pcm_close(struct alsa_pcm_t *alsa)
{
    int r;

    r = snd_pcm_close(alsa->pcm);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }
    free(alsa->buf);

    return 0;
}


static int pcm_pollfds(struct alsa_pcm_t *alsa, struct pollfd *pe, int n)
{
    int r, count;

    count = snd_pcm_poll_descriptors_count(alsa->pcm);
    if(count > n)
        return -1;

    if(count == 0) 
        alsa->pe = NULL;
    else {
        r = snd_pcm_poll_descriptors(alsa->pcm, pe, count);
        
        if(r < 0) {
            alsa_error(r);
            return -1;
        }

        alsa->pe = pe;
    }

    alsa->pe_count = count;
    return count;
}


static int pcm_revents(struct alsa_pcm_t *alsa, unsigned short *revents) {
    int r;

    r = snd_pcm_poll_descriptors_revents(alsa->pcm, alsa->pe, alsa->pe_count,
                                         revents);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }
    
    return 0;
}



/* Start the audio device capture and playback */

static int start(struct device_t *dv)
{
    int r;
    struct alsa_t *alsa = (struct alsa_t*)dv->local;

    r = snd_pcm_start(alsa->capture.pcm);
    if(r < 0) {
        alsa_error(r);
        return -1;
    }

    return 0;
}


/* Register this device's interest in a set of pollfd file
 * descriptors */

static int pollfds(struct device_t *dv, struct pollfd *pe, int pe_size)
{
    int total, r;
    struct alsa_t *alsa = (struct alsa_t*)dv->local;

    total = 0;

    r = pcm_pollfds(&alsa->capture, pe, pe_size);
    if(r < 0)
        return -1;
    
    pe += r;
    pe_size -= r;
    total += r;
    
    r = pcm_pollfds(&alsa->playback, pe, pe_size);
    if(r < 0)
        return -1;
    
    total += r;
    
    return total;
}
    

/* Collect audio from the player and push it into the device's buffer,
 * for playback */

static int playback(struct device_t *dv)
{
    int r;
    struct alsa_t *alsa = (struct alsa_t*)dv->local;

    /* Always push some audio to the soundcard, even if it means
     * silence. This has shown itself to be much more reliable than
     * constantly starting and stopping -- which can affect other
     * devices to the one which is doing the stopping. */
    
    if(dv->player && dv->player->playing)
        player_collect(dv->player, alsa->playback.buf, alsa->playback.period);
    else {
        memset(alsa->playback.buf, 0,
               alsa->playback.period * DEVICE_CHANNELS * sizeof(short));
    }    

    r = snd_pcm_writei(alsa->playback.pcm, alsa->playback.buf,
                       alsa->playback.period);
    if(r < 0)
        return r;
        
    if(r < alsa->playback.period) {
        fprintf(stderr, "alsa: playback underrun %d/%ld.\n", r,
                alsa->playback.period);
    }

    return 0;
}


/* Pull audio from the device's buffer for capture, and pass it
 * through to the timecoder */

static int capture(struct device_t *dv)
{
    int r;
    struct alsa_t *alsa = (struct alsa_t*)dv->local;

    r = snd_pcm_readi(alsa->capture.pcm, alsa->capture.buf,
                      alsa->capture.period);
    if(r < 0)
        return r;
    
    if(r < alsa->capture.period) {
        fprintf(stderr, "alsa: capture underrun %d/%ld.\n",
                r, alsa->capture.period);
    }
    
    if(dv->timecoder) {
        timecoder_submit(dv->timecoder, alsa->capture.buf, r);
        
        if(dv->player)
            player_sync(dv->player);
    }

    return 0;
}


/* After poll() has returned, instruct a device to do all it can at
 * the present time. Return zero if success, otherwise -1 */

static int handle(struct device_t *dv)
{
    int r;
    unsigned short revents;
    struct alsa_t *alsa = (struct alsa_t*)dv->local;

    /* Check input buffer for timecode capture */
    
    r = pcm_revents(&alsa->capture, &revents);
    if(r < 0)
        return -1;
    
    if(revents & POLLIN) { 
        r = capture(dv);
        
        if(r < 0) {
            if(r == -EPIPE) {
                fputs("alsa: capture xrun.\n", stderr);

                r = snd_pcm_prepare(alsa->capture.pcm);
                if(r < 0) {
                    alsa_error(r);
                    return -1;
                }

                r = snd_pcm_start(alsa->capture.pcm);
                if(r < 0) {
                    alsa_error(r);
                    return -1;
                }

            } else {
                alsa_error(r);
                return -1;
            }
        } 
    }
    
    /* Check the output buffer for playback */
    
    r = pcm_revents(&alsa->playback, &revents);
    if(r < 0)
        return -1;
    
    if(revents & POLLOUT) {
        r = playback(dv);
        
        if(r < 0) {
            if(r == -EPIPE) {
                fputs("alsa: playback xrun.\n", stderr);
                
                r = snd_pcm_prepare(alsa->playback.pcm) < 0;
                if(r < 0) {
                    alsa_error(r);
                    return -1;
                }

                /* The device starts when data is written. POLLOUT
                 * events are generated in prepared state. */

            } else {
                alsa_error(r);
                return -1;
            }
        }
    }

    return 0;
}


/* Close ALSA device and clear any allocations */

static int clear(struct device_t *dv)
{
    struct alsa_t *alsa = (struct alsa_t*)dv->local;

    pcm_close(&alsa->capture);
    pcm_close(&alsa->playback);
    free(dv->local);

    return 0;
}


/* Open ALSA device. Do not operate on audio until device_start() */

int alsa_init(struct device_t *dv, const char *device_name, int buffer_time)
{
    struct alsa_t *alsa;

    alsa = malloc(sizeof(struct alsa_t));
    if(!alsa) {
        perror("malloc");
        return -1;
    }

    if(pcm_open(&alsa->capture, device_name, SND_PCM_STREAM_CAPTURE,
                buffer_time) < 0)
    {
        fputs("Failed to open device for capture.\n", stderr);
        goto fail;
    }
    
    if(pcm_open(&alsa->playback, device_name, SND_PCM_STREAM_PLAYBACK,
                buffer_time) < 0)
    {
        fputs("Failed to open device for playback.\n", stderr);
        goto fail;
    }

    dv->local = alsa;

    dv->pollfds = pollfds;
    dv->handle = handle;
    dv->start = start;
    dv->stop = NULL;
    dv->clear = clear;

    return 0;

 fail:
    free(alsa);
    return -1;
}