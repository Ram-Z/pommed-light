/*
 * pommed - Apple laptops hotkeys handler daemon
 *
 * $Id$
 *
 * Copyright (C) 2006-2007 Julien BLACHE <jb@jblache.org>
 * Copyright (C) 2006 Soeren SONNENBURG <debian@nn7.de>
 *
 * Portions of the code below dealing with the audio thread were shamelessly
 * stolen from pbbuttonsd. Thanks ! ;-)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <errno.h>

#include <syslog.h>

#include <linux/input.h>
#include <linux/uinput.h>

#include <pthread.h>

#define NDEBUG
#include <alsa/asoundlib.h>

#include <audiofile.h>

#include "pommed.h"
#include "conffile.h"
#include "audio.h"
#include "beep.h"



/* Added to linux/input.h after Linux 2.6.18 */
#ifndef BUS_VIRTUAL
# define BUS_VIRTUAL 0x06
#endif


struct _beep_info beep_info;
static int beep_thread_running = 0;


/* Beep thread */
static void
beep_thread_command(int command);

static void
beep_thread_cleanup(void);

static int
beep_thread_init(void);


void
beep_beep(void)
{
  if (beep_cfg.enabled == 0)
    return;

  if (audio_info.muted)
    return;

  beep_thread_command(AUDIO_CLICK);
}


int
beep_open_device(void)
{
  struct uinput_user_dev dv;
  int fd;
  int ret;

  beep_info.fd = -1;

  if (beep_cfg.enabled == 0)
    return -1;

  fd = open("/dev/input/uinput", O_RDWR, 0);
  if (fd < 0)
    {
      logmsg(LOG_ERR, "beep: could not open /dev/input/uinput: %s", strerror(errno));
      logmsg(LOG_ERR, "beep: Do you have the uinput module loaded?");

      return -1;
    }

  memset(&dv, 0, sizeof(dv));
  strcpy(dv.name, BEEP_DEVICE_NAME);
  dv.id.bustype = BUS_VIRTUAL;
  dv.id.vendor = 0;
  dv.id.product = 0;
  dv.id.version = 1;

  ret = write(fd, &dv, sizeof(dv));
  if (ret != sizeof(dv))
    {
      logmsg(LOG_ERR, "beep: could not set device name: %s", strerror(errno));

      close(fd);
      return -1;
    }

  ret = ioctl(fd, UI_SET_EVBIT, EV_SND);
  if (ret != 0)
    {
      logmsg(LOG_ERR, "beep: could not request EV_SND: %s", strerror(errno));

      close(fd);
      return -1;
    }

  ret = ioctl(fd, UI_SET_SNDBIT, SND_BELL);
  if (ret != 0)
    {
      logmsg(LOG_ERR, "beep: could not request SND_BELL: %s", strerror(errno));

      close(fd);
      return -1;
    }

  ret = ioctl(fd, UI_SET_SNDBIT, SND_TONE);
  if (ret != 0)
    {
      logmsg(LOG_ERR, "beep: could not request SND_TONE: %s", strerror(errno));

      close(fd);
      return -1;
    }

  ret = ioctl(fd, UI_DEV_CREATE, NULL);
  if (ret != 0)
    {
      logmsg(LOG_ERR, "beep: could not create uinput device: %s", strerror(errno));

      close(fd);
      return -1;
  }

  beep_info.fd = fd;

  return 0;
}

void
beep_close_device(void)
{
  if (beep_cfg.enabled == 0)
    return;

  ioctl(beep_info.fd, UI_DEV_DESTROY, NULL);

  close(beep_info.fd);

  beep_info.fd = -1;
}


int
beep_init(void)
{
  int ret;

  if (beep_cfg.enabled == 0)
    return 0;

  ret = beep_thread_init();
  if (ret < 0)
    {
      logmsg(LOG_ERR, "beep: thread init failed, disabling");

      beep_cfg.enabled = 0;

      return -1;
    }

  beep_thread_running = 1;

  return 0;
}

void
beep_cleanup(void)
{
  if (!beep_thread_running)
    return;

  beep_thread_command(AUDIO_COMMAND_QUIT);
  beep_thread_cleanup();
}

void
beep_fix_config(void)
{
  if (beep_cfg.enabled == 0)
    return;

  if (beep_cfg.beepfile == NULL)
    beep_cfg.beepfile = strdup(BEEP_DEFAULT_FILE);

  if (access(beep_cfg.beepfile, R_OK) != 0)
    {
      logmsg(LOG_WARNING, "beep: cannot access WAV file %s: %s", beep_cfg.beepfile, strerror(errno));

      if (access(BEEP_DEFAULT_FILE, R_OK) == 0)
	{
	  logmsg(LOG_WARNING, "beep: falling back to default file %s", BEEP_DEFAULT_FILE);

	  free(beep_cfg.beepfile);
	  beep_cfg.beepfile = strdup(BEEP_DEFAULT_FILE);
	}
      else
	{
	  logmsg(LOG_ERR, "beep: cannot access default file %s: %s", BEEP_DEFAULT_FILE, strerror(errno));
	  logmsg(LOG_ERR, "beep: disabling beeper");

	  beep_cfg.enabled = 0;
	}
    }
}


/* 
 * Beep thread - taken from gpomme/audio.c
 * If there's a bug somewhere below, it's also in gpomme, so please fix it there too.
 */

struct dspdata _dsp;

/* Called from the main thread */
static struct sample *
beep_load_sample(char *filename)
{
  AFfilehandle affd;     /* filehandle for soundfile from libaudiofile */
  AFframecount framecount;
  int dummy, channels, byteorder, framesize, precision, err = 0;
  struct sample *sample;

  int ret;

  sample = (struct sample *) malloc(sizeof(struct sample));
  if (sample == NULL)
    return NULL;

  affd = afOpenFile(filename, "r", 0);
  if (affd > 0)
    {
      afGetSampleFormat(affd, AF_DEFAULT_TRACK, &dummy, &precision);
      channels = afGetChannels(affd, AF_DEFAULT_TRACK);
      byteorder = afGetVirtualByteOrder(affd, AF_DEFAULT_TRACK);
      framesize = (int) afGetFrameSize(affd, AF_DEFAULT_TRACK, 0);
      framecount = afGetFrameCount(affd, AF_DEFAULT_TRACK);
      sample->speed = (int) afGetRate(affd, AF_DEFAULT_TRACK);

      if (channels <= 2)
	sample->channels = channels;
      else
	err = -1;

      if (precision == 8)
	sample->format = SND_PCM_FORMAT_S8;
      else if (precision == 16)
	{
	  if (byteorder == AF_BYTEORDER_LITTLEENDIAN)
	    sample->format = SND_PCM_FORMAT_S16_LE;
	  else
	    sample->format = SND_PCM_FORMAT_S16_BE;
	}
      else
	err = -1;

      if (err == 0)
	{
	  sample->framesize = framesize;
	  sample->framecount = framecount;
	  sample->audiodatalen = framecount * framesize;

	  sample->audiodata = (char *) malloc(sample->audiodatalen);
	  if (sample->audiodata != NULL)
	    {
	      ret = afReadFrames(affd, AF_DEFAULT_TRACK, sample->audiodata, framecount);
	      if (ret != framecount)
		{
		  free(sample->audiodata);
		  err = -1;
		}
	    }
	  else
	    ret = -1;
	}
      afCloseFile(affd);
    }

  if (err == -1)
    {
      free(sample);
      return NULL;
    }

  sample->periods = sample->framesize;
  sample->buffersize = (sample->periods * 8192) >> 2;

  return sample;
}


/* Called from the audio thread */
static void
beep_play_sample(struct dspdata *dsp, int cmd)
{
  snd_pcm_t *pcm_handle;          
  snd_pcm_hw_params_t *hwparams;

  char *pcm_name = "default";

  struct sample *s = dsp->sample[cmd];

  snd_pcm_hw_params_alloca(&hwparams);

  if (snd_pcm_open(&pcm_handle, pcm_name, SND_PCM_STREAM_PLAYBACK, 0) < 0)
    {
      logmsg(LOG_WARNING, "beep: error opening PCM device %s", pcm_name);
      return;
    }

  if (snd_pcm_hw_params_any(pcm_handle, hwparams) < 0)
    {
      logmsg(LOG_WARNING, "beep: cannot configure PCM device");
      return;
    }

  if (snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
    {
      logmsg(LOG_WARNING, "beep: error setting access");
      return;
    }

  if (snd_pcm_hw_params_set_format(pcm_handle, hwparams, s->format) < 0)
    {
      logmsg(LOG_WARNING, "beep: error setting format");
      return;
    }

  if (snd_pcm_hw_params_set_rate_near(pcm_handle, hwparams, &s->speed, 0) < 0)
    {
      logmsg(LOG_WARNING, "beep: error setting rate");
      return;
    }

  /* Set number of channels */
  if (snd_pcm_hw_params_set_channels_near(pcm_handle, hwparams, &s->channels) < 0)
    {
      logmsg(LOG_WARNING, "beep: error setting channels");
      return;
    }

  /* Set number of periods. Periods used to be called fragments. */ 
  if (snd_pcm_hw_params_set_periods_near(pcm_handle, hwparams, &s->periods, 0) < 0)
    {
      logmsg(LOG_WARNING, "beep: error setting periods");
      return;
    }

  /* Set buffer size (in frames). The resulting latency is given by */
  /* latency = periodsize * periods / (rate * bytes_per_frame)     */
  if (snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hwparams, &s->buffersize) < 0)
    {
      logmsg(LOG_WARNING, "beep: error setting buffersize");
      return;
    }

  /* Apply HW parameter settings to */
  /* PCM device and prepare device  */
  if (snd_pcm_hw_params(pcm_handle, hwparams) < 0)
    {
      logmsg(LOG_WARNING, "beep: error setting HW params");
      return;
    }

  int pcmreturn;
  /* Write num_frames frames from buffer data to    */ 
  /* the PCM device pointed to by pcm_handle.       */
  /* Returns the number of frames actually written. */
  while ((pcmreturn = snd_pcm_writei(pcm_handle, s->audiodata, s->framecount)) < 0)
    {
      snd_pcm_prepare(pcm_handle);
    }

  /* Stop PCM device and drop pending frames */
  snd_pcm_drop(pcm_handle);

  /* Stop PCM device after pending frames have been played */ 
  snd_pcm_close(pcm_handle);
}


/* Called from the audio thread
 * Audio thread main loop
 */
void *
beep_thread (void *arg)
{
  struct dspdata *dsp = (struct dspdata *) arg;
  for (;;)
    {
      pthread_mutex_lock(&dsp->mutex);
      pthread_cond_wait(&dsp->cond, &dsp->mutex);
      pthread_mutex_unlock(&dsp->mutex);

      switch (dsp->command)
	{
	  case AUDIO_CLICK:
	    dsp->command = AUDIO_COMMAND_NONE;

	    beep_play_sample(dsp, AUDIO_CLICK);
	    break;
	  case AUDIO_COMMAND_QUIT:
	    pthread_exit(NULL);
	    break;
	  case AUDIO_COMMAND_NONE:
	    break;
	}
    }

  return NULL;
}


/* Called from the main thread
 * This function wakes the audio thread
 */
static void
beep_thread_command(int command)
{
  if (!_dsp.sample)
    return;

  pthread_mutex_lock(&(_dsp.mutex));

  _dsp.command = command;

  pthread_cond_signal(&(_dsp.cond));
  pthread_mutex_unlock(&(_dsp.mutex));
}


/* Called from the main thread */
static void
beep_thread_cleanup(void)
{
  int i;

  if (!_dsp.sample)
    return;

  for (i = 0; i < AUDIO_N; i++)
    {
      if (_dsp.sample[i] == NULL)
	continue;

      if (_dsp.sample[i]->audiodata != NULL)
	free(_dsp.sample[i]->audiodata);

      free(_dsp.sample[i]);
    }

  pthread_mutex_destroy(&(_dsp.mutex));
  pthread_cond_destroy(&(_dsp.cond));
}

/* Called from the main thread
 * This function sets up the sound playing thread.
 * It starts the thread or if an error occur cleans
 * up all the audio stuff
 */
static int
beep_thread_init(void)
{
  pthread_attr_t attr;
  int ret;

  _dsp.sample[AUDIO_CLICK] = beep_load_sample(beep_cfg.beepfile);

  if (_dsp.sample[AUDIO_CLICK] == NULL)
    return -1;

  _dsp.thread = 0;

  pthread_mutex_init(&(_dsp.mutex), NULL);
  pthread_cond_init (&(_dsp.cond), NULL);
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  ret = pthread_create(&(_dsp.thread), &attr, beep_thread, (void *) &_dsp);
  if (ret != 0)
    {
      beep_thread_cleanup();
      ret = -1;
    }

  pthread_attr_destroy(&attr);

  return ret;
}
