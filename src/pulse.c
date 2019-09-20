// pulse.c
// LiVES (lives-exe)
// (c) G. Finch <salsaman+lives@gmail.com> 2005 - 2018
// Released under the GPL 3 or later
// see file ../COPYING for licensing details

#ifdef HAVE_PULSE_AUDIO

#include "main.h"
#include "callbacks.h"
#include "support.h"
#include "effects.h"
#include "effects-weed.h"

#define afile mainw->files[pulsed->playing_file]

//#define DEBUG_PULSE

static pulse_driver_t pulsed;
static pulse_driver_t pulsed_reader;

static pa_threaded_mainloop *pa_mloop = NULL;
static pa_context *pcon = NULL;

static uint32_t pulse_server_rate = 0;

#define PULSE_READ_BYTES 48000

static uint8_t prbuf[PULSE_READ_BYTES * 2];

static size_t prb = 0;

static boolean seek_err;

///////////////////////////////////////////////////////////////////

void pa_mloop_lock(void) {
  pa_threaded_mainloop_lock(pa_mloop);
}


void pa_mloop_unlock(void) {
  pa_threaded_mainloop_unlock(pa_mloop);
}


static void pulse_server_cb(pa_context *c, const pa_server_info *info, void *userdata) {
  if (info == NULL) {
    pulse_server_rate = 0;
    return;
  }
  pulse_server_rate = info->sample_spec.rate;
}


static void stream_underflow_callback(pa_stream *s, void *userdata) {
  fprintf(stderr, "PA Stream underrun. \n");
}


static void stream_overflow_callback(pa_stream *s, void *userdata) {
  fprintf(stderr, "Stream overrun. \n");
}


boolean lives_pulse_init(short startup_phase) {
  // startup pulseaudio server
  char *msg;
  pa_context_state_t pa_state;
  boolean timeout;
  int alarm_handle;

  if (pa_mloop != NULL) return TRUE;

  pa_mloop = pa_threaded_mainloop_new();
  pcon = pa_context_new(pa_threaded_mainloop_get_api(pa_mloop), "LiVES");
  pa_context_connect(pcon, NULL, (pa_context_flags_t)0, NULL);
  pa_threaded_mainloop_start(pa_mloop);

  pa_state = pa_context_get_state(pcon);

  alarm_handle = lives_alarm_set(LIVES_SHORT_TIMEOUT);
  while (pa_state != PA_CONTEXT_READY && !(timeout = lives_alarm_get(alarm_handle))) {
    sched_yield();
    lives_usleep(prefs->sleep_time);
    pa_state = pa_context_get_state(pcon);
  }

  lives_alarm_clear(alarm_handle);

  if (pa_context_get_state(pcon) == PA_CONTEXT_READY) timeout = FALSE;

  if (timeout) {
    pa_context_unref(pcon);
    pcon = NULL;
    pulse_shutdown();

    LIVES_WARN("Unable to connect to the pulseaudio server");

    if (!mainw->foreign) {
      if (startup_phase == 0 && capable->has_sox_play) {
        do_error_dialog_with_check(
          _("\nUnable to connect to the pulseaudio server.\nFalling back to sox audio player.\nYou can change this in Preferences/Playback.\n"),
          WARN_MASK_NO_PULSE_CONNECT);
        switch_aud_to_sox(prefs->warning_mask & WARN_MASK_NO_PULSE_CONNECT);
      } else if (startup_phase == 0) {
        do_error_dialog_with_check(
          _("\nUnable to connect to the pulseaudio server.\nFalling back to none audio player.\nYou can change this in Preferences/Playback.\n"),
          WARN_MASK_NO_PULSE_CONNECT);
        switch_aud_to_none(TRUE);
      } else {
        msg = lives_strdup(_("\nUnable to connect to the pulseaudio server.\n"));
        if (startup_phase != 2) {
          do_blocking_error_dialog(msg);
          mainw->aplayer_broken = TRUE;
        } else {
          do_blocking_error_dialogf("%s%s", msg, _("LiVES will exit and you can choose another audio player.\n"));
        }
        lives_free(msg);
      }
    }
    return FALSE;
  }
  return TRUE;
}


void pulse_get_rec_avals(pulse_driver_t *pulsed) {
  mainw->rec_aclip = pulsed->playing_file;
  if (mainw->rec_aclip != -1) {
    mainw->rec_aseek = (double)pulsed->real_seek_pos / (double)(afile->arps * afile->achans * afile->asampsize / 8);
    mainw->rec_avel = SIGNED_DIVIDE((double)pulsed->in_arate, (double)afile->arate);
  }
}


static void pulse_set_rec_avals(pulse_driver_t *pulsed) {
  // record direction change (internal)
  mainw->rec_aclip = pulsed->playing_file;
  if (mainw->rec_aclip != -1) {
    pulse_get_rec_avals(pulsed);
  }
}


#if !HAVE_PA_STREAM_BEGIN_WRITE
static void pulse_buff_free(void *ptr) {
  lives_free(ptr);
}
#endif


static void sample_silence_pulse(pulse_driver_t *pdriver, size_t nbytes, size_t xbytes) {
  uint8_t *buff;
  int nsamples;

  if (xbytes <= 0) return;
  if (mainw->aplayer_broken) return;
  while (nbytes > 0) {
#if HAVE_PA_STREAM_BEGIN_WRITE
    xbytes = -1;
    // returns a buffer and size for us to write to
    pa_stream_begin_write(pdriver->pstream, (void **)&buff, &xbytes);
#endif
    if (nbytes < xbytes) xbytes = nbytes;
#if !HAVE_PA_STREAM_BEGIN_WRITE
    buff = (uint8_t *)lives_try_malloc0(xbytes);
#endif
    if (!buff) {
#if HAVE_PA_STREAM_BEGIN_WRITE
      pa_stream_cancel_write(pdriver->pstream);
#endif
      return;
    }
#if HAVE_PA_STREAM_BEGIN_WRITE
    memset(buff, 0, xbytes);
#endif
    if (pdriver->astream_fd != -1) audio_stream(buff, xbytes, pdriver->astream_fd); // old streaming API

    nsamples = xbytes / pdriver->out_achans / (pdriver->out_asamps >> 3);

    // new streaming API
    if (mainw->ext_audio && mainw->vpp != NULL && mainw->vpp->render_audio_frame_float != NULL && pdriver->playing_file != -1
        && pdriver->playing_file != mainw->ascrap_file) {
      sample_silence_stream(pdriver->out_achans, nsamples);
    }

    pthread_mutex_lock(&mainw->abuf_frame_mutex);
    if (mainw->audio_frame_buffer != NULL && prefs->audio_src != AUDIO_SRC_EXT) {
      // buffer audio for any generators
      // interlaced, so we paste all to channel 0
      append_to_audio_buffer16(buff, nsamples * pdriver->out_achans, 0);
      mainw->audio_frame_buffer->samples_filled += nsamples * pdriver->out_achans;
    }
    pthread_mutex_unlock(&mainw->abuf_frame_mutex);
#if !HAVE_PA_STREAM_BEGIN_WRITE
    pa_stream_write(pdriver->pstream, buff, xbytes, pulse_buff_free, 0, PA_SEEK_RELATIVE);
#else
    pa_stream_write(pdriver->pstream, buff, xbytes, NULL, 0, PA_SEEK_RELATIVE);
#endif
    nbytes -= xbytes;
  }
}


static short *shortbuffer = NULL;

static void pulse_audio_write_process(pa_stream *pstream, size_t nbytes, void *arg) {
  // PULSE AUDIO calls this periodically to get the next audio buffer
  // note the buffer size can, and does, change on each call, making it inefficient to use ringbuffers
  pulse_driver_t *pulsed = (pulse_driver_t *)arg;

  uint64_t nsamples = nbytes / pulsed->out_achans / (pulsed->out_asamps >> 3);

  aserver_message_t *msg;

  int64_t seek, xseek;
  int new_file;
  char *filename;
  boolean from_memory = FALSE;

  uint8_t *buffer;
  size_t xbytes = pa_stream_writable_size(pstream);

  boolean needs_free = FALSE;

  size_t offs = 0;

  pa_volume_t pavol;

  pulsed->real_seek_pos = pulsed->seek_pos;

  pulsed->pstream = pstream;

  if (xbytes > nbytes) xbytes = nbytes;

  if (!mainw->is_ready || pulsed == NULL || (mainw->playing_file == -1 && pulsed->msgq == NULL) || nbytes > 1000000) {
    sample_silence_pulse(pulsed, nsamples * pulsed->out_achans * (pulsed->out_asamps >> 3), xbytes);
    //g_print("pt a1 %ld %d %p %d %p %ld\n",nsamples, mainw->is_ready, pulsed, mainw->playing_file, pulsed->msgq, nbytes);
    return;
  }

  while ((msg = (aserver_message_t *)pulsed->msgq) != NULL) {
    switch (msg->command) {
    case ASERVER_CMD_FILE_OPEN:
      new_file = atoi((char *)msg->data);
      if (pulsed->playing_file != new_file) {
        filename = lives_get_audio_file_name(new_file);
        pulsed->fd = lives_open2(filename, O_RDONLY);
        if (pulsed->fd == -1) {
          // dont show gui errors - we are running in realtime thread
          LIVES_ERROR("pulsed: error opening");
          LIVES_ERROR(filename);
          pulsed->playing_file = -1;
        } else {
#ifdef HAVE_POSIX_FADVISE
          if (new_file == mainw->ascrap_file) {
            posix_fadvise(pulsed->fd, 0, 0, POSIX_FADV_SEQUENTIAL);
          }
#endif
          pulsed->real_seek_pos = pulsed->seek_pos = 0;
          pulsed->playing_file = new_file;
          pa_stream_trigger(pulsed->pstream, NULL, NULL);
        }
        lives_free(filename);
      }
      break;
    case ASERVER_CMD_FILE_CLOSE:
      //if (mainw->playing_file == -1) pulse_driver_cork(pulsed);
      if (pulsed->fd >= 0) close(pulsed->fd);
      if (pulsed->sound_buffer == pulsed->aPlayPtr->data) pulsed->sound_buffer = NULL;
      lives_freep((void **)&pulsed->aPlayPtr->data);
      pulsed->aPlayPtr->max_size = 0;
      pulsed->aPlayPtr->size = 0;
      pulsed->fd = -1;
      pulsed->playing_file = -1;
      break;
    case ASERVER_CMD_FILE_SEEK:
      if (pulsed->fd < 0) break;
      xseek = seek = atol((char *)msg->data);
      if (seek < 0.) xseek = 0.;
      if (mainw->agen_key == 0 && !mainw->agen_needs_reinit) {
        lseek(pulsed->fd, xseek, SEEK_SET);
      }
      pulsed->real_seek_pos = pulsed->seek_pos = seek;
      pa_stream_trigger(pulsed->pstream, NULL, NULL);
      break;
    default:
      pulsed->msgq = NULL;
      msg->data = NULL;
    }
    if (msg->data != NULL && msg->next != msg) {
      lives_free((char *)(msg->data));
      msg->data = NULL;
    }
    msg->command = ASERVER_CMD_PROCESSED;
    pulsed->msgq = msg->next;
    if (pulsed->msgq != NULL && pulsed->msgq->next == pulsed->msgq) pulsed->msgq->next = NULL;
  }

  if (pulsed->chunk_size != nbytes) pulsed->chunk_size = nbytes;

  pulsed->state = pa_stream_get_state(pulsed->pstream);

  if (pulsed->state == PA_STREAM_READY) {
    uint64_t pulseFramesAvailable = nsamples;
    uint64_t inputFramesAvailable;
    uint64_t numFramesToWrite;
    int64_t in_frames = 0;
    uint64_t in_bytes = 0, xin_bytes = 0;
    float shrink_factor = 1.f;
    int swap_sign;

    lives_clip_t *xfile = afile;

#ifdef DEBUG_PULSE
    lives_printerr("playing... pulseFramesAvailable = %ld\n", pulseFramesAvailable);
#endif

    pulsed->num_calls++;

    if (!pulsed->in_use || (((pulsed->fd < 0 || pulsed->seek_pos < 0.) && pulsed->read_abuf < 0) &&
                            ((mainw->agen_key == 0 && !mainw->agen_needs_reinit) || mainw->multitrack != NULL))
        || pulsed->is_paused || (mainw->pulsed_read != NULL && mainw->pulsed_read->playing_file != -1)) {
      sample_silence_pulse(pulsed, nsamples * pulsed->out_achans * (pulsed->out_asamps >> 3), xbytes);

      if (!pulsed->is_paused) pulsed->frames_written += nsamples;
      if (pulsed->seek_pos < 0. && pulsed->playing_file > -1 && afile != NULL) {
        pulsed->seek_pos += nsamples * afile->achans * afile->asampsize / 8;
        if (pulsed->seek_pos >= 0) pulse_audio_seek_bytes(pulsed, pulsed->seek_pos, afile);
      }
#ifdef DEBUG_PULSE
      g_print("pt a3 %d\n", pulsed->in_use);
#endif
      return;
    }

    if (LIVES_LIKELY(pulseFramesAvailable > 0 && (pulsed->read_abuf > -1 ||
                     (pulsed->aPlayPtr != NULL
                      && pulsed->in_achans > 0) ||
                     ((mainw->agen_key != 0 || mainw->agen_needs_reinit) && mainw->multitrack == NULL)
                                                 ))) {
      if (mainw->playing_file > -1 && pulsed->read_abuf > -1) {
        // playing back from memory buffers instead of from file
        // this is used in multitrack
        from_memory = TRUE;
        numFramesToWrite = pulseFramesAvailable;
        pulsed->frames_written += numFramesToWrite;
        pulseFramesAvailable -= numFramesToWrite;

      } else {
        if (LIVES_LIKELY(pulsed->fd >= 0)) {
          int playfile = mainw->playing_file;
          if ((pulsed->playing_file == mainw->ascrap_file && !mainw->preview) && playfile >= -1
              && mainw->files[playfile] != NULL && mainw->files[playfile]->achans > 0) {
            xfile = mainw->files[playfile];
          }
          pulsed->aPlayPtr->size = 0;
          in_bytes = ABS((in_frames = ((double)pulsed->in_arate / (double)pulsed->out_arate *
                                       (double)pulseFramesAvailable + ((double)fastrand() / (double)LIVES_MAXUINT32))))
                     * pulsed->in_achans * (pulsed->in_asamps >> 3);
#ifdef DEBUG_PULSE
          g_print("in bytes=%ld %ld %ld %ld %ld %ld\n", in_bytes, pulsed->in_arate, pulsed->out_arate, pulseFramesAvailable, pulsed->in_achans,
                  pulsed->in_asamps);
#endif
          if ((shrink_factor = (float)in_frames / (float)pulseFramesAvailable) < 0.f) {
            // reverse playback
            if ((pulsed->seek_pos -= in_bytes) < 0) {
              if (pulsed->loop == AUDIO_LOOP_NONE) {
                if (*pulsed->whentostop == STOP_ON_AUD_END) {
                  *pulsed->cancelled = CANCEL_AUD_END;
                }
                pulsed->in_use = FALSE;
              } else {
                if (pulsed->loop == AUDIO_LOOP_PINGPONG) {
                  pulsed->in_arate = -pulsed->in_arate;
                  shrink_factor = -shrink_factor;
                  pulsed->seek_pos = -pulsed->seek_pos;
                } else pulsed->seek_pos += pulsed->seek_end;
              }
              pulsed->real_seek_pos = pulsed->seek_pos;
              pulse_set_rec_avals(pulsed);
            }
            // rewind by in_bytes
            if ((mainw->agen_key == 0 && !mainw->agen_needs_reinit) || mainw->multitrack != NULL)
              lseek(pulsed->fd, pulsed->seek_pos, SEEK_SET);
          }
          if (LIVES_UNLIKELY((in_bytes > pulsed->aPlayPtr->max_size && !(*pulsed->cancelled) && ABS(shrink_factor) <= 100.f))) {
            boolean update_sbuffer = FALSE;
            if (pulsed->sound_buffer == pulsed->aPlayPtr->data) update_sbuffer = TRUE;
            pulsed->aPlayPtr->data = lives_try_realloc(pulsed->aPlayPtr->data, in_bytes);
            if (update_sbuffer) pulsed->sound_buffer = pulsed->aPlayPtr->data;
            if (pulsed->aPlayPtr->data != NULL) {
              memset(pulsed->aPlayPtr->data, 0, in_bytes);
              pulsed->aPlayPtr->max_size = in_bytes;
            } else {
              pulsed->aPlayPtr->max_size = 0;
            }
          }
          if (pulsed->mute || (pulsed->aPlayPtr->data == NULL && ((mainw->agen_key == 0 && !mainw->agen_needs_reinit) ||
                               mainw->multitrack != NULL))) {
            if (shrink_factor > 0.f) pulsed->seek_pos += in_bytes;
            if (pulsed->seek_pos >= pulsed->seek_end) {
              if (*pulsed->whentostop == STOP_ON_AUD_END) {
                *pulsed->cancelled = CANCEL_AUD_END;
              } else {
                if (pulsed->loop == AUDIO_LOOP_PINGPONG) {
                  pulsed->in_arate = -pulsed->in_arate;
                  pulsed->seek_pos -= in_bytes;
                } else {
                  pulsed->seek_pos = 0;
                }
              }
            }
            sample_silence_pulse(pulsed, nsamples * pulsed->out_achans *
                                 (pulsed->out_asamps >> 3), xbytes);
            pulsed->frames_written += nsamples;
#ifdef DEBUG_PULSE
            g_print("pt a4\n");
#endif
            return;
          } else {
            boolean loop_restart;
            do {
              loop_restart = FALSE;
              if (in_bytes > 0) {
                // playing from a file
                if (!(*pulsed->cancelled) && ABS(shrink_factor) <= 100.f) {
                  if (((((mainw->agen_key == 0 && !mainw->agen_needs_reinit) || mainw->multitrack != NULL) &&
                        (pulsed->aPlayPtr->size = read(pulsed->fd, pulsed->aPlayPtr->data, in_bytes)) == 0)) ||
                      (((mainw->agen_key != 0 || mainw->agen_needs_reinit) && mainw->multitrack == NULL &&
                        (xfile != NULL && pulsed->seek_pos + in_bytes >= xfile->afilesize)))) {
                    if (*pulsed->whentostop == STOP_ON_AUD_END) {
                      *pulsed->cancelled = CANCEL_AUD_END;
                    } else {
                      loop_restart = TRUE;
                      if (pulsed->loop == AUDIO_LOOP_PINGPONG) {
                        pulsed->in_arate = -pulsed->in_arate;
                        if ((mainw->agen_key == 0 && !mainw->agen_needs_reinit) || mainw->multitrack != NULL)
                          lseek(pulsed->fd, (pulsed->seek_pos -= in_bytes), SEEK_SET);
                      } else {
                        if (pulsed->loop != AUDIO_LOOP_NONE) {
                          seek = 0;
                          if ((mainw->agen_key == 0 && !mainw->agen_needs_reinit) || mainw->multitrack != NULL)
                            lseek(pulsed->fd, (pulsed->seek_pos = seek), SEEK_SET);
                          else pulsed->seek_pos = seek;
                        } else {
                          if (mainw->agen_key == 0 && !mainw->agen_needs_reinit) pulsed->in_use = FALSE;
                          loop_restart = FALSE;
                        }
                      }
                      pulsed->real_seek_pos = pulsed->seek_pos;
                      pulse_set_rec_avals(pulsed);
                    }
                  } else {
                    if (pulsed->aPlayPtr->size < 0 && ((mainw->agen_key == 0 && !mainw->agen_needs_reinit) ||
                                                       mainw->multitrack != NULL)) {
                      // read error...output silence
                      sample_silence_pulse(pulsed, nsamples * pulsed->out_achans *
                                           (pulsed->out_asamps >> 3), xbytes);
                      if (!pulsed->is_paused) pulsed->frames_written += nsamples;
#ifdef DEBUG_PULSE
                      g_print("pt X\n");
#endif
                      return;
                    }
                    if (shrink_factor < 0.f) {
                      // reverse play - rewind again by in_bytes
                      if ((mainw->agen_key == 0 && !mainw->agen_needs_reinit) || mainw->multitrack != NULL)
                        lseek(pulsed->fd, pulsed->seek_pos, SEEK_SET);
                    } else {
                      if ((mainw->agen_key == 0 && !mainw->agen_needs_reinit) || mainw->multitrack != NULL)
                        pulsed->seek_pos += pulsed->aPlayPtr->size;
                      else
                        pulsed->seek_pos += in_bytes;
                    }
                  }
                }
              }
            } while (loop_restart);
          }
          xin_bytes = in_bytes;
        }

        if (mainw->agen_key != 0 && mainw->multitrack == NULL) {
          in_bytes = pulseFramesAvailable * pulsed->out_achans * 2;
          if (xin_bytes == 0) xin_bytes = in_bytes;
        }

        if (!pulsed->in_use || in_bytes == 0 || pulsed->mute) {
          // reached end of audio with no looping
          sample_silence_pulse(pulsed, nsamples * pulsed->out_achans *
                               (pulsed->out_asamps >> 3), xbytes);
          if (!pulsed->is_paused) pulsed->frames_written += nsamples;
          if (pulsed->seek_pos < 0. && pulsed->playing_file > -1 && afile != NULL) {
            pulsed->seek_pos += nsamples * afile->achans * afile->asampsize / 8;
            if (pulsed->seek_pos >= 0) pulse_audio_seek_bytes(pulsed, pulsed->seek_pos, afile);
          }
#ifdef DEBUG_PULSE
          g_print("pt a5 %d %ld\n", pulsed->in_use, in_bytes);
#endif
          return;
        }

        if ((mainw->agen_key == 0 && !mainw->agen_needs_reinit) || mainw->multitrack != NULL) {
          swap_sign = afile->signed_endian & AFORM_UNSIGNED;

          inputFramesAvailable = pulsed->aPlayPtr->size / (pulsed->in_achans * (pulsed->in_asamps >> 3));
#ifdef DEBUG_PULSE
          lives_printerr("%ld inputFramesAvailable == %ld, %ld, %ld %ld,pulseFramesAvailable == %ld\n", pulsed->aPlayPtr->size, inputFramesAvailable,
                         in_frames, pulsed->in_arate, pulsed->out_arate, pulseFramesAvailable);
#endif
          buffer = (uint8_t *)pulsed->aPlayPtr->data;

          numFramesToWrite = MIN(pulseFramesAvailable, (inputFramesAvailable / ABS(shrink_factor) + .001));

#ifdef DEBUG_PULSE
          lives_printerr("inputFramesAvailable after conversion %ld\n", (uint64_t)((double)inputFramesAvailable / shrink_factor + .001));
          lives_printerr("nsamples == %ld, pulseFramesAvailable == %ld,\n\tpulsed->num_input_channels == %ld, pulsed->out_achans == %ld\n",  nsamples,
                         pulseFramesAvailable, pulsed->in_achans, pulsed->out_achans);
#endif

          if (pulsed->in_asamps == pulsed->out_asamps && shrink_factor == 1. && pulsed->in_achans == pulsed->out_achans &&
              !pulsed->reverse_endian && !swap_sign) {
            // no transformation needed
            pulsed->sound_buffer = buffer;
          } else {
            if (pulsed->sound_buffer != pulsed->aPlayPtr->data) lives_freep((void **)&pulsed->sound_buffer);
            pulsed->sound_buffer = (uint8_t *)lives_try_malloc0(pulsed->chunk_size);
            if (!pulsed->sound_buffer) {
              sample_silence_pulse(pulsed, nsamples * pulsed->out_achans *
                                   (pulsed->out_asamps >> 3), xbytes);
              if (!pulsed->is_paused) pulsed->frames_written += nsamples;
#ifdef DEBUG_PULSE
              g_print("pt X2\n");
#endif
              return;
            }

            if (pulsed->in_asamps == 8) {
              sample_move_d8_d16((short *)(pulsed->sound_buffer), (uint8_t *)buffer, numFramesToWrite, in_bytes,
                                 shrink_factor, pulsed->out_achans, pulsed->in_achans, swap_sign ? SWAP_U_TO_S : 0);
            } else {
              sample_move_d16_d16((short *)pulsed->sound_buffer, (short *)buffer, numFramesToWrite, in_bytes, shrink_factor,
                                  pulsed->out_achans, pulsed->in_achans, pulsed->reverse_endian ? SWAP_X_TO_L : 0,
                                  swap_sign ? SWAP_U_TO_S : 0);
            }
          }

          if ((has_audio_filters(AF_TYPE_ANY) || mainw->ext_audio) && (pulsed->playing_file != mainw->ascrap_file)) {
            boolean memok = TRUE;
            float **fltbuf = (float **)lives_malloc(pulsed->out_achans * sizeof(float *));
            register int i;

            // we have audio filters; convert to float, pass through any audio filters, then back to s16
            for (i = 0; i < pulsed->out_achans; i++) {
              // convert s16 to non-interleaved float
              fltbuf[i] = (float *)lives_try_malloc(numFramesToWrite * sizeof(float));
              if (fltbuf[i] == NULL) {
                memok = FALSE;
                for (--i; i >= 0; i--) {
                  lives_freep((void **)&fltbuf[i]);
                }
                break;
              }

              pulsed->abs_maxvol_heard = sample_move_d16_float(fltbuf[i], (short *)pulsed->sound_buffer + i, numFramesToWrite, pulsed->out_achans, FALSE,
                                         FALSE, 1.0);
            }

            if (memok) {
              int64_t tc = mainw->currticks;
              // apply any audio effects with in_channels

              if (has_audio_filters(AF_TYPE_ANY)) weed_apply_audio_effects_rt(fltbuf, pulsed->out_achans, numFramesToWrite, pulsed->out_arate, tc, FALSE);

              // new streaming API
              pthread_mutex_lock(&mainw->vpp_stream_mutex);
              if (mainw->ext_audio && mainw->vpp != NULL && mainw->vpp->render_audio_frame_float != NULL) {
                (*mainw->vpp->render_audio_frame_float)(fltbuf, numFramesToWrite);
              }
              pthread_mutex_unlock(&mainw->vpp_stream_mutex);

              // convert float audio back to s16
              sample_move_float_int(pulsed->sound_buffer, fltbuf, numFramesToWrite, 1.0, pulsed->out_achans, PA_SAMPSIZE, 0,
                                    (capable->byte_order == LIVES_LITTLE_ENDIAN), FALSE, 1.0);

              for (i = 0; i < pulsed->out_achans; i++) {
                lives_free(fltbuf[i]);
              }
            }

            lives_free(fltbuf);
          }
        } else {
          // audio generator
          // get float audio from gen, convert it to S16
          float *fbuffer = NULL;
          boolean pl_error = FALSE;
          xbytes = nbytes;
          numFramesToWrite = pulseFramesAvailable;

          if (mainw->agen_needs_reinit) pl_error = TRUE;
          else {
            fbuffer = (float *)lives_malloc(numFramesToWrite * pulsed->out_achans * sizeof(float));
            if (!get_audio_from_plugin(fbuffer, pulsed->out_achans, pulsed->out_arate, numFramesToWrite)) {
              pl_error = TRUE;
            }
          }

          if (!pl_error) {
            if (LIVES_UNLIKELY(nbytes > pulsed->aPlayPtr->max_size)) {
              boolean update_sbuffer = FALSE;
              if (pulsed->sound_buffer == pulsed->aPlayPtr->data) update_sbuffer = TRUE;
              pulsed->aPlayPtr->data = lives_try_realloc(pulsed->aPlayPtr->data, nbytes);
              if (update_sbuffer) pulsed->sound_buffer = pulsed->aPlayPtr->data;
              if (pulsed->aPlayPtr->data != NULL) {
                memset(pulsed->aPlayPtr->data, 0, in_bytes);
                pulsed->aPlayPtr->max_size = nbytes;
              } else {
                pulsed->aPlayPtr->max_size = 0;
                pl_error = TRUE;
              }
            }
            pulsed->aPlayPtr->size = nbytes;
          }

          // get back non-interleaved float fbuffer; rate and channels should match
          if (pl_error) nbytes = 0;
          else {
            register int i;
            boolean memok = FALSE;
            float **fp = (float **)lives_malloc(pulsed->out_achans * sizeof(float *));
            void *buf;
            pulsed->sound_buffer = (uint8_t *)pulsed->aPlayPtr->data;
            buf = (void *)pulsed->sound_buffer;

            if (has_audio_filters(AF_TYPE_ANY) || mainw->ext_audio) {
              register int i;

              memok = TRUE;

              // we have audio filters; convert to float, pass through any audio filters, then back to s16
              for (i = 0; i < pulsed->out_achans; i++) {
                // convert s16 to non-interleaved float
                fp[i] = (float *)lives_try_malloc(numFramesToWrite * sizeof(float));
                if (fp[i] == NULL) {
                  memok = FALSE;
                  for (--i; i >= 0; i--) {
                    lives_free(fp[i]);
                  }
                  break;
                }
                lives_memcpy(fp[i], &fbuffer[i * numFramesToWrite], numFramesToWrite * sizeof(float));
              }

              if (memok) {
                int64_t tc = mainw->currticks;
                // apply any audio effects with in_channels

                if (has_audio_filters(AF_TYPE_ANY)) weed_apply_audio_effects_rt(fp, pulsed->out_achans, numFramesToWrite, pulsed->out_arate, tc, FALSE);

                // new streaming API
                pthread_mutex_lock(&mainw->vpp_stream_mutex);
                if (mainw->ext_audio && mainw->vpp != NULL && mainw->vpp->render_audio_frame_float != NULL) {
                  (*mainw->vpp->render_audio_frame_float)(fp, numFramesToWrite);
                }
                pthread_mutex_unlock(&mainw->vpp_stream_mutex);

                // convert float audio to s16
                sample_move_float_int(buf, fp, numFramesToWrite, 1.0, pulsed->out_achans, PA_SAMPSIZE, FALSE,
                                      (capable->byte_order == LIVES_LITTLE_ENDIAN), FALSE, 1.0);

                for (i = 0; i < pulsed->out_achans; i++) {
                  lives_free(fp[i]);
                }
              }
            }

            if (!memok) {
              // no audio effects; or memory allocation error
              for (i = 0; i < pulsed->out_achans; i++) {
                fp[i] = fbuffer + (i * numFramesToWrite);
              }
              sample_move_float_int(buf, fp, numFramesToWrite, 1.0,
                                    pulsed->out_achans, PA_SAMPSIZE, 0, (capable->byte_order == LIVES_LITTLE_ENDIAN), FALSE, 1.0);
            }

            lives_freep((void **)&fbuffer);
            free(fp);

            if (mainw->record && !mainw->record_paused && mainw->ascrap_file != -1 && mainw->playing_file > 0) {
              // write generated audio to ascrap_file
              size_t rbytes = numFramesToWrite * mainw->files[mainw->ascrap_file]->achans *
                              mainw->files[mainw->ascrap_file]->asampsize >> 3;
              pulse_flush_read_data(pulsed, mainw->ascrap_file, nbytes, mainw->files[mainw->ascrap_file]->signed_endian & AFORM_BIG_ENDIAN, buf);
              mainw->files[mainw->ascrap_file]->aseek_pos += rbytes;
            }
          }
        }

        pulsed->frames_written += numFramesToWrite;
        pulseFramesAvailable -= numFramesToWrite;

#ifdef DEBUG_PULSE
        lives_printerr("pulseFramesAvailable == %ld\n", pulseFramesAvailable);
#endif
      }

      // playback from memory or file

      if (mainw->volume != pulsed->volume_linear) {
        pa_operation *paop;
        pavol = pa_sw_volume_from_linear(mainw->volume);
        pa_cvolume_set(&pulsed->volume, pulsed->out_achans, pavol);
        paop = pa_context_set_sink_input_volume(pulsed->con, pa_stream_get_index(pulsed->pstream), &pulsed->volume, NULL, NULL);
        pa_operation_unref(paop);
        pulsed->volume_linear = mainw->volume;
      }

      while (nbytes > 0) {
        if (nbytes < xbytes) xbytes = nbytes;

        if (!from_memory) {
          if (xbytes / pulsed->out_achans / (pulsed->out_asamps >> 3) <= numFramesToWrite && offs == 0) {
            buffer = pulsed->sound_buffer;
          } else {
#if HAVE_PA_STREAM_BEGIN_WRITE
            xbytes = -1;
            // returns a buffer and a max size fo us to write to
            pa_stream_begin_write(pulsed->pstream, (void **)&buffer, &xbytes);
            if (nbytes < xbytes) xbytes = nbytes;
#else
            buffer = (uint8_t *)lives_try_malloc(nbytes);
#endif
            if (!buffer || !pulsed->sound_buffer) {
#if HAVE_PA_STREAM_BEGIN_WRITE
              pa_stream_cancel_write(pulsed->pstream);
#endif
              sample_silence_pulse(pulsed, nsamples * pulsed->out_achans *
                                   (pulsed->out_asamps >> 3), nbytes);
              if (!pulsed->is_paused) pulsed->frames_written += nsamples;
#ifdef DEBUG_PULSE
              g_print("pt X3\n");
#endif
              return;
            }
            lives_memcpy(buffer, pulsed->sound_buffer + offs, xbytes);
            offs += xbytes;
            needs_free = TRUE;
          }
          if (pulsed->astream_fd != -1) audio_stream(buffer, xbytes, pulsed->astream_fd);
          pthread_mutex_lock(&mainw->abuf_frame_mutex);
          if (mainw->audio_frame_buffer != NULL && prefs->audio_src != AUDIO_SRC_EXT) {
            append_to_audio_buffer16(buffer, xbytes / 2, 0);
            mainw->audio_frame_buffer->samples_filled += xbytes / 2;
          }
          pthread_mutex_unlock(&mainw->abuf_frame_mutex);
#if !HAVE_PA_STREAM_BEGIN_WRITE
          pa_stream_write(pulsed->pstream, buffer, xbytes, buffer == pulsed->aPlayPtr->data ? NULL :
                          pulse_buff_free, 0, PA_SEEK_RELATIVE);
#else
          pa_stream_write(pulsed->pstream, buffer, xbytes, NULL, 0, PA_SEEK_RELATIVE);
#endif
        } else {
          if (pulsed->read_abuf > -1 && !pulsed->mute) {
#if HAVE_PA_STREAM_BEGIN_WRITE
            xbytes = -1;
            pa_stream_begin_write(pulsed->pstream, (void **)&shortbuffer, &xbytes);
#endif
            if (nbytes < xbytes) xbytes = nbytes;
#if !HAVE_PA_STREAM_BEGIN_WRITE
            shortbuffer = (short *)lives_try_malloc0(xbytes);
#endif
            if (!shortbuffer) {
#if HAVE_PA_STREAM_BEGIN_WRITE
              pa_stream_cancel_write(pulsed->pstream);
#endif
              sample_silence_pulse(pulsed, nsamples * pulsed->out_achans *
                                   (pulsed->out_asamps >> 3), nbytes);
              if (!pulsed->is_paused) pulsed->frames_written += nsamples;
#ifdef DEBUG_PULSE
              g_print("pt X4\n");
#endif
              return;
            }
            sample_move_abuf_int16(shortbuffer, pulsed->out_achans, (xbytes >> 1) / pulsed->out_achans, pulsed->out_arate);
            if (pulsed->astream_fd != -1) audio_stream(shortbuffer, xbytes, pulsed->astream_fd);
            pthread_mutex_lock(&mainw->abuf_frame_mutex);
            if (mainw->audio_frame_buffer != NULL && prefs->audio_src != AUDIO_SRC_EXT) {
              append_to_audio_buffer16(shortbuffer, xbytes / 2, 0);
              mainw->audio_frame_buffer->samples_filled += xbytes / 2;
            }
            pthread_mutex_unlock(&mainw->abuf_frame_mutex);
#if !HAVE_PA_STREAM_BEGIN_WRITE
            pa_stream_write(pulsed->pstream, shortbuffer, xbytes, pulse_buff_free, 0, PA_SEEK_RELATIVE);
#else
            pa_stream_write(pulsed->pstream, shortbuffer, xbytes, NULL, 0, PA_SEEK_RELATIVE);
#endif
          } else {
            sample_silence_pulse(pulsed, xbytes, xbytes);
            if (!pulsed->is_paused) pulsed->frames_written += xbytes / pulsed->out_achans / (pulsed->out_asamps >> 3);
          }
        }
        nbytes -= xbytes;
      }

      if (needs_free && pulsed->sound_buffer != pulsed->aPlayPtr->data && pulsed->sound_buffer != NULL) {
        lives_freep((void **)&pulsed->sound_buffer);
      }
    }

    if (pulseFramesAvailable) {
#ifdef DEBUG_PULSE
      lives_printerr("buffer underrun of %ld frames\n", pulseFramesAvailable);
#endif
      xbytes = pa_stream_writable_size(pstream);
      sample_silence_pulse(pulsed, pulseFramesAvailable * pulsed->out_achans
                           * (pulsed->out_asamps >> 3), xbytes);
      if (!pulsed->is_paused) pulsed->frames_written += xbytes / pulsed->out_achans / (pulsed->out_asamps >> 3);
    }
  } else {
#ifdef DEBUG_PULSE
    if (pulsed->state == PA_STREAM_UNCONNECTED || pulsed->state == PA_STREAM_CREATING)
      LIVES_INFO("pulseaudio stream UNCONNECTED or CREATING");
    else
      LIVES_WARN("pulseaudio stream FAILED or TERMINATED");
#endif
  }

#ifdef DEBUG_PULSE
  lives_printerr("done\n");
#endif
}


size_t pulse_flush_read_data(pulse_driver_t *pulsed, int fileno, size_t rbytes, boolean rev_endian, void *data) {
  // prb is how many bytes to write, with rbytes as the latest addition

  short *gbuf;
  size_t bytes_out, frames_out, bytes = 0;
  void *holding_buff;

  float out_scale;
  int swap_sign;

  lives_clip_t *ofile;

  if (data == NULL) data = prbuf;

  if (mainw->agen_key == 0 && !mainw->agen_needs_reinit) {
    if (prb == 0 || mainw->rec_samples == 0) return 0;
    if (prb <= PULSE_READ_BYTES * 2) {
      gbuf = (short *)data;
    } else {
      gbuf = (short *)lives_try_malloc(prb);
      if (!gbuf) return 0;
      if (prb > rbytes) lives_memcpy((void *)gbuf, prbuf, prb - rbytes);
      lives_memcpy((void *)gbuf + (prb - rbytes > 0 ? prb - rbytes : 0), data, rbytes);
    }
    ofile = afile;
  } else {
    if (rbytes == 0) return 0;
    if (fileno == -1) return 0;
    gbuf = (short *)data;
    prb = rbytes;
    ofile = mainw->files[fileno];
  }

  out_scale = (float)pulsed->in_arate / (float)ofile->arate;
  swap_sign = ofile->signed_endian & AFORM_UNSIGNED;

  frames_out = (size_t)((double)((prb / (ofile->asampsize >> 3) / ofile->achans)) / out_scale);

  if (mainw->agen_key == 0 && !mainw->agen_needs_reinit) {
    if (frames_out != pulsed->chunk_size) pulsed->chunk_size = frames_out;
  }

  bytes_out = frames_out * ofile->achans * (ofile->asampsize >> 3);

  holding_buff = lives_try_malloc(bytes_out);

  if (!holding_buff) {
    if (gbuf != (short *)data) lives_free(gbuf);
    prb = 0;
    return 0;
  }

  if (ofile->asampsize == 16) {
    sample_move_d16_d16((short *)holding_buff, gbuf, frames_out, prb, out_scale, ofile->achans, pulsed->in_achans,
                        pulsed->reverse_endian ? SWAP_L_TO_X : 0, swap_sign ? SWAP_S_TO_U : 0);
  } else {
    sample_move_d16_d8((uint8_t *)holding_buff, gbuf, frames_out, prb, out_scale, ofile->achans, pulsed->in_achans,
                       swap_sign ? SWAP_S_TO_U : 0);
  }

  if (gbuf != (short *)data) lives_free(gbuf);

  prb = 0;

  if (mainw->rec_samples > 0) {
    if (frames_out > mainw->rec_samples) frames_out = mainw->rec_samples;
    mainw->rec_samples -= frames_out;
  }

  if (mainw->bad_aud_file == NULL) {
    size_t target = frames_out * (ofile->asampsize / 8) * ofile->achans, bytes;
    // use write not lives_write - because of potential threading issues
    bytes = write(mainw->aud_rec_fd, holding_buff, target);
    if (bytes > 0) {
      mainw->aud_data_written += bytes;
      if (mainw->ascrap_file != -1 && mainw->files[mainw->ascrap_file] != NULL && mainw->aud_rec_fd == mainw->files[mainw->ascrap_file]->cb_src)
        add_to_ascrap_mb(bytes);
      if (mainw->aud_data_written > AUD_WRITTEN_CHECK) {
        mainw->aud_data_written = 0;
        check_for_disk_space();
      }
    }
    if (bytes < target) mainw->bad_aud_file = filename_from_fd(NULL, mainw->aud_rec_fd);
  }

  lives_free(holding_buff);

  return bytes;
}


static void pulse_audio_read_process(pa_stream *pstream, size_t nbytes, void *arg) {
  // read nsamples from pulse buffer, and then possibly write to mainw->aud_rec_fd

  // this is the callback from pulse when we are recording or playing external audio

  pulse_driver_t *pulsed = (pulse_driver_t *)arg;
  float out_scale;
  size_t frames_out, nsamples;
  void *data;
  size_t rbytes = nbytes, zbytes;

  pulsed->pstream = pstream;

  if (pulsed->is_corked) return;

  if (!pulsed->in_use || (mainw->playing_file < 0 && prefs->audio_src == AUDIO_SRC_EXT) || mainw->effects_paused) {
    pa_stream_peek(pulsed->pstream, (const void **)&data, &rbytes);
    if (rbytes > 0) pa_stream_drop(pulsed->pstream);
    prb = 0;
    return;
  }

  zbytes = pa_stream_readable_size(pulsed->pstream);

  if (zbytes == 0) {
    //g_print("nothing to read from PA\n");
    return;
  }

  if (pa_stream_peek(pulsed->pstream, (const void **)&data, &rbytes)) {
    return;
  }

  if (data == NULL) {
    if (rbytes > 0) {
      pa_stream_drop(pulsed->pstream);
    }
    return;
  }

  pthread_mutex_lock(&mainw->audio_filewriteend_mutex);

  if (pulsed->playing_file == -1) {
    out_scale = 1.0; // just listening, no recording
  } else {
    out_scale = (float)afile->arate / (float)pulsed->in_arate; // recording to ascrap_file
  }

  prb += rbytes;

  if (pulsed->playing_file == -1 || (mainw->record && mainw->record_paused)) prb = 0;

  frames_out = (size_t)((double)((prb / (pulsed->in_asamps >> 3) / pulsed->in_achans)) / out_scale + .5);

  nsamples = (size_t)((double)((rbytes / (pulsed->in_asamps >> 3) / pulsed->in_achans)) / out_scale + .5);

  // should really be frames_read here
  if (!pulsed->is_paused) {
    pulsed->frames_written += nsamples;
  }

  if (prefs->audio_src == AUDIO_SRC_EXT && (pulsed->playing_file == -1 || pulsed->playing_file == mainw->ascrap_file)) {
    // - (do not call this when recording ext window or voiceover)

    // in this case we read external audio, but maybe not record it
    // we may wish to analyse the audio for example, or push it to a video generator
    // or stream it to the video playback plugin

    if ((!mainw->video_seek_ready && prefs->ahold_threshold > pulsed->abs_maxvol_heard)
        || has_audio_filters(AF_TYPE_A) || mainw->ext_audio) {
      // convert to float, apply any analysers
      boolean memok = TRUE;
      float **fltbuf = (float **)lives_malloc(pulsed->in_achans * sizeof(float *));
      register int i;

      size_t xnsamples = (size_t)(rbytes / (pulsed->in_asamps >> 3) / pulsed->in_achans);

      if (fltbuf == NULL) {
        pthread_mutex_unlock(&mainw->audio_filewriteend_mutex);
        pa_stream_drop(pulsed->pstream);
        return;
      }

      for (i = 0; i < pulsed->in_achans; i++) {
        // convert s16 to non-interleaved float
        fltbuf[i] = (float *)lives_try_malloc(xnsamples * sizeof(float));
        if (fltbuf[i] == NULL) {
          memok = FALSE;
          for (--i; i >= 0; i--) {
            lives_free(fltbuf[i]);
          }
          break;
        }

        pulsed->abs_maxvol_heard = sample_move_d16_float(fltbuf[i], (short *)(data) + i, xnsamples, pulsed->in_achans, FALSE, FALSE, 1.0);

        pthread_mutex_lock(&mainw->abuf_frame_mutex);
        if (mainw->audio_frame_buffer != NULL && prefs->audio_src == AUDIO_SRC_EXT) {
          // if we have audio triggered gens., push audio to it
          append_to_audio_bufferf(fltbuf[i], xnsamples, i);
          if (i == pulsed->in_achans - 1) mainw->audio_frame_buffer->samples_filled += xnsamples;
        }
        pthread_mutex_unlock(&mainw->abuf_frame_mutex);
      }

      if (memok) {
        int64_t tc = mainw->currticks;
        // apply any audio effects with in channels but no out channels

        if (has_audio_filters(AF_TYPE_A)) weed_apply_audio_effects_rt(fltbuf, pulsed->in_achans, xnsamples, pulsed->in_arate, tc, TRUE);

        // new streaming API
        pthread_mutex_lock(&mainw->vpp_stream_mutex);
        if (mainw->ext_audio && mainw->vpp != NULL && mainw->vpp->render_audio_frame_float != NULL) {
          (*mainw->vpp->render_audio_frame_float)(fltbuf, xnsamples);
        }
        pthread_mutex_unlock(&mainw->vpp_stream_mutex);
        for (i = 0; i < pulsed->in_achans; i++) {
          lives_free(fltbuf[i]);
        }
      }

      lives_freep((void **)&fltbuf);
    }
  }

  if (pulsed->playing_file == -1 || (mainw->record && mainw->record_paused) || pulsed->is_paused) {
    pa_stream_drop(pulsed->pstream);
    if (pulsed->is_paused) {
      // This is NECESSARY to reduce / eliminate huge latencies.
      pa_operation *paop = pa_stream_flush(pulsed->pstream, NULL, NULL); // if not recording, flush the rest of audio (to reduce latency)
      pa_operation_unref(paop);
    }
    pthread_mutex_unlock(&mainw->audio_filewriteend_mutex);
    return;
  }

  if (mainw->playing_file != mainw->ascrap_file && IS_VALID_CLIP(mainw->playing_file))
    mainw->files[mainw->playing_file]->aseek_pos += rbytes;
  if (mainw->ascrap_file != -1 && !mainw->record_paused) mainw->files[mainw->ascrap_file]->aseek_pos += rbytes;

  pulsed->seek_pos += rbytes;

  if (prb < PULSE_READ_BYTES && (mainw->rec_samples == -1 || frames_out < mainw->rec_samples)) {
    // buffer until we have enough
    lives_memcpy(&prbuf[prb - rbytes], data, rbytes);
  } else {
    if (prb <= PULSE_READ_BYTES * 2) {
      lives_memcpy(&prbuf[prb - rbytes], data, rbytes);
      pulse_flush_read_data(pulsed, pulsed->playing_file, prb, pulsed->reverse_endian, prbuf);
    } else {
      pulse_flush_read_data(pulsed, pulsed->playing_file, rbytes, pulsed->reverse_endian, data);
    }
  }

  pa_stream_drop(pulsed->pstream);
  pthread_mutex_unlock(&mainw->audio_filewriteend_mutex);

  if (mainw->rec_samples == 0 && mainw->cancelled == CANCEL_NONE) {
    mainw->cancelled = CANCEL_KEEP; // we wrote the required #
  }
}


void pulse_shutdown(void) {
  //g_print("pa shutdown\n");
  if (pcon != NULL) {
    //g_print("pa shutdown2\n");
    pa_context_disconnect(pcon);
    pa_context_unref(pcon);
  }
  if (pa_mloop != NULL) {
    pa_threaded_mainloop_stop(pa_mloop);
    pa_threaded_mainloop_free(pa_mloop);
  }
  pcon = NULL;
  pa_mloop = NULL;
}


void pulse_close_client(pulse_driver_t *pdriver) {
  if (pdriver->pstream != NULL) {
    pa_threaded_mainloop_lock(pa_mloop);
    pa_stream_disconnect(pdriver->pstream);
    pa_stream_set_write_callback(pdriver->pstream, NULL, NULL);
    pa_stream_set_read_callback(pdriver->pstream, NULL, NULL);
    pa_stream_set_underflow_callback(pdriver->pstream, NULL, NULL);
    pa_stream_set_overflow_callback(pdriver->pstream, NULL, NULL);
    pa_stream_unref(pdriver->pstream);
    pa_threaded_mainloop_unlock(pa_mloop);
  }
  if (pdriver->pa_props != NULL) pa_proplist_free(pdriver->pa_props);
  pdriver->pa_props = NULL;
  pdriver->pstream = NULL;
}


int pulse_audio_init(void) {
  // initialise variables
#if PA_SW_CONNECTION
  int j;
#endif

  pulsed.in_use = FALSE;
  pulsed.mloop = pa_mloop;
  pulsed.con = pcon;

  //for (j = 0; j < PULSE_MAX_OUTPUT_CHANS; j++) pulsed.volume.values[j] = pa_sw_volume_from_linear(mainw->volume);
  pulsed.volume_linear = mainw->volume;
  pulsed.state = (pa_stream_state_t)PA_STREAM_UNCONNECTED;
  pulsed.in_arate = 44100;
  pulsed.fd = -1;
  pulsed.seek_pos = pulsed.seek_end = pulsed.real_seek_pos = 0;
  pulsed.msgq = NULL;
  pulsed.num_calls = 0;
  pulsed.chunk_size = 0;
  pulsed.astream_fd = -1;
  pulsed.abs_maxvol_heard = 0.;
  pulsed.pulsed_died = FALSE;
  pulsed.aPlayPtr = (audio_buffer_t *)lives_malloc(sizeof(audio_buffer_t));
  pulsed.aPlayPtr->data = NULL;
  pulsed.aPlayPtr->size = 0;
  pulsed.aPlayPtr->max_size = 0;
  pulsed.in_achans = PA_ACHANS;
  pulsed.out_achans = PA_ACHANS;
  pulsed.out_asamps = PA_SAMPSIZE;
  pulsed.mute = FALSE;
  pulsed.out_chans_available = PULSE_MAX_OUTPUT_CHANS;
  pulsed.is_output = TRUE;
  pulsed.read_abuf = -1;
  pulsed.is_paused = FALSE;
  pulsed.pstream = NULL;
  pulsed.pa_props = NULL;
  pulsed.playing_file = -1;
  pulsed.sound_buffer = NULL;
  return 0;
}


int pulse_audio_read_init(void) {
  // initialise variables
#if PA_SW_CONNECTION
  int j;
#endif

  pulsed_reader.in_use = FALSE;
  pulsed_reader.mloop = pa_mloop;
  pulsed_reader.con = pcon;

  //for (j = 0; j < PULSE_MAX_OUTPUT_CHANS; j++) pulsed_reader.volume.values[j] = pa_sw_volume_from_linear(mainw->volume);
  pulsed_reader.state = (pa_stream_state_t)PA_STREAM_UNCONNECTED;
  pulsed_reader.fd = -1;
  pulsed_reader.seek_pos = pulsed_reader.seek_end = 0;
  pulsed_reader.msgq = NULL;
  pulsed_reader.num_calls = 0;
  pulsed_reader.chunk_size = 0;
  pulsed_reader.astream_fd = -1;
  pulsed_reader.abs_maxvol_heard = 0.;
  pulsed_reader.pulsed_died = FALSE;
  pulsed_reader.in_achans = PA_ACHANS;
  pulsed_reader.in_asamps = PA_SAMPSIZE;
  pulsed_reader.mute = FALSE;
  pulsed_reader.is_output = FALSE;
  pulsed_reader.is_paused = FALSE;
  pulsed_reader.pstream = NULL;
  pulsed_reader.pa_props = NULL;
  pulsed_reader.sound_buffer = NULL;
  return 0;
}


#if PA_SW_CONNECTION
static void info_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata) {
  // would be great if this worked, but apparently it always returns NULL in i
  // for a hardware connection

  // TODO: get volume_writeable (pa 1.0+)
  pulse_driver_t *pdriver = (pulse_driver_t *)userdata;
  if (i == NULL) return;

  pdrive->volume = i->volume;
  pdriver->volume_linear = pa_sw_volume_to_linear(i->volume.values[0]);
  lives_scale_button_set_value(LIVES_SCALE_BUTTON(mainw->volume_scale), pdriver->volume_linear);
  if (i->mute != mainw->mute) on_mute_activate(NULL, NULL);
}
#endif


int pulse_driver_activate(pulse_driver_t *pdriver) {
  // create a new client and connect it to pulse server
  char *pa_clientname;
  char *mypid;

  pa_sample_spec pa_spec;
  pa_channel_map pa_map;
  pa_buffer_attr pa_battr;

  pa_operation *pa_op;

  if (pdriver->pstream != NULL) return 0;

  if (mainw->aplayer_broken) return 2;

  if (pdriver->is_output) {
    pa_clientname = "LiVES_audio_out";
  } else {
    pa_clientname = "LiVES_audio_in";
  }

  mypid = lives_strdup_printf("%d", capable->mainpid);

  pdriver->pa_props = pa_proplist_new();

  pa_proplist_sets(pdriver->pa_props, PA_PROP_APPLICATION_ICON_NAME, lives_get_application_name());
  pa_proplist_sets(pdriver->pa_props, PA_PROP_APPLICATION_ID, lives_get_application_name());
  pa_proplist_sets(pdriver->pa_props, PA_PROP_APPLICATION_NAME, lives_get_application_name());

  pa_proplist_sets(pdriver->pa_props, PA_PROP_APPLICATION_PROCESS_BINARY, capable->myname);
  pa_proplist_sets(pdriver->pa_props, PA_PROP_APPLICATION_PROCESS_ID, mypid);
  pa_proplist_sets(pdriver->pa_props, PA_PROP_APPLICATION_VERSION, LiVES_VERSION);

  lives_free(mypid);

#ifdef GUI_GTK
  pa_proplist_sets(pdriver->pa_props, PA_PROP_APPLICATION_LANGUAGE, pango_language_to_string(gtk_get_default_language()));
#endif

#ifdef GUI_QT
  QLocale ql;
  pa_proplist_sets(pdriver->pa_props, PA_PROP_APPLICATION_LANGUAGE, (QLocale::languageToString(ql.language())).toLocal8Bit().constData());
#endif

  pa_channel_map_init_stereo(&pa_map);

  pa_spec.format = PA_SAMPLE_S16NE;

  pa_spec.channels = pdriver->out_achans = pdriver->in_achans;

  pdriver->in_asamps = pdriver->out_asamps = PA_SAMPSIZE;
  pdriver->out_signed = AFORM_SIGNED;

  if (capable->byte_order == LIVES_BIG_ENDIAN) {
    pdriver->out_endian = AFORM_BIG_ENDIAN;
    pa_spec.format = PA_SAMPLE_S16BE;
  } else {
    pdriver->out_endian = AFORM_LITTLE_ENDIAN;
    pa_spec.format = PA_SAMPLE_S16LE;
  }

  if (pdriver->is_output) {
    pa_battr.maxlength = LIVES_PA_BUFF_MAXLEN;
    pa_battr.tlength = LIVES_PA_BUFF_TARGET;
  } else {
    pa_battr.maxlength = LIVES_PA_BUFF_MAXLEN * 2;
    pa_battr.fragsize = LIVES_PA_BUFF_FRAGSIZE * 4;
  }

  pa_battr.minreq = (uint32_t) - 1;
  pa_battr.prebuf = -1;

  if (pulse_server_rate == 0) {
    pa_op = pa_context_get_server_info(pdriver->con, pulse_server_cb, NULL);

    while (pa_operation_get_state(pa_op) == PA_OPERATION_RUNNING) {
      sched_yield();
      lives_usleep(prefs->sleep_time);
    }
    pa_operation_unref(pa_op);
  }

  if (pulse_server_rate == 0) {
    LIVES_WARN("Problem getting pulseaudio rate...expect more problems ahead.");
    return 1;
  }

  pa_spec.rate = pdriver->out_arate = pdriver->in_arate = pulse_server_rate;

  pa_threaded_mainloop_lock(pa_mloop);
  pdriver->pstream = pa_stream_new_with_proplist(pdriver->con, pa_clientname, &pa_spec, &pa_map, pdriver->pa_props);

  if (pdriver->is_output) {
    pa_volume_t pavol;
    pdriver->is_corked = TRUE;

#if PA_SW_CONNECTION
    pa_stream_connect_playback(pdriver->pstream, NULL, &pa_battr, (pa_stream_flags_t)(PA_STREAM_ADJUST_LATENCY |
                               PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_START_CORKED |
                               PA_STREAM_AUTO_TIMING_UPDATE),
                               NULL, NULL);
#else
    pdriver->volume_linear = mainw->volume;
    pavol = pa_sw_volume_from_linear(pdriver->volume_linear);
    pa_cvolume_set(&pdriver->volume, pdriver->out_achans, pavol);

    pa_stream_connect_playback(pdriver->pstream, NULL, &pa_battr, (pa_stream_flags_t)(PA_STREAM_ADJUST_LATENCY |
                               PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_START_CORKED | PA_STREAM_NOT_MONOTONIC |
                               PA_STREAM_AUTO_TIMING_UPDATE),
                               &pdriver->volume, NULL);
#endif
    pa_threaded_mainloop_unlock(pa_mloop);

    while (pa_stream_get_state(pdriver->pstream) != PA_STREAM_READY) {
      sched_yield();
      lives_usleep(prefs->sleep_time);
    }

    pa_threaded_mainloop_lock(pa_mloop);
    // set write callback
    pa_stream_set_write_callback(pdriver->pstream, pulse_audio_write_process, pdriver);

    pdriver->volume_linear = -1;

#if PA_SW_CONNECTION
    // get the volume from the server
    pa_op = pa_context_get_sink_info(pdriver->con, info_cb, &pdriver);

    while (pa_operation_get_state(pa_op) == PA_OPERATION_RUNNING) {
      sched_yield();
      lives_usleep(prefs->sleep_time);
    }
    pa_operation_unref(pa_op);
#endif

  } else {
    // set read callback
    pdriver->frames_written = 0;
    pdriver->usec_start = 0;
    pdriver->in_use = FALSE;
    pdriver->abs_maxvol_heard = 0.;
    pdriver->is_corked = TRUE;
    prb = 0;

    pa_stream_set_underflow_callback(pdriver->pstream, stream_underflow_callback, NULL);
    pa_stream_set_overflow_callback(pdriver->pstream, stream_overflow_callback, NULL);

    pa_stream_connect_record(pdriver->pstream, NULL, &pa_battr,
                             (pa_stream_flags_t)(PA_STREAM_START_CORKED | PA_STREAM_ADJUST_LATENCY | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE |
                                 PA_STREAM_NOT_MONOTONIC));
    pa_threaded_mainloop_unlock(pa_mloop);

    while (pa_stream_get_state(pdriver->pstream) != PA_STREAM_READY) {
      sched_yield();
      lives_usleep(prefs->sleep_time);
    }

    pa_threaded_mainloop_lock(pa_mloop);
    pa_stream_set_read_callback(pdriver->pstream, pulse_audio_read_process, pdriver);
  }

  pa_threaded_mainloop_unlock(pa_mloop);
  return 0;
}


//#define DEBUG_PULSE_CORK
static void uncorked_cb(pa_stream *s, int success, void *userdata) {
  pulse_driver_t *pdriver = (pulse_driver_t *)userdata;
#ifdef DEBUG_PULSE_CORK
  g_print("uncorked %p\n", pdriver);
#endif
  pdriver->is_corked = FALSE;
  prefs->force_system_clock = FALSE;
}


static void corked_cb(pa_stream *s, int success, void *userdata) {
  pulse_driver_t *pdriver = (pulse_driver_t *)userdata;
#ifdef DEBUG_PULSE_CORK
  g_print("corked %p\n", pdriver);
#endif
  pdriver->is_corked = TRUE;
  prefs->force_system_clock = TRUE;
}


void pulse_driver_uncork(pulse_driver_t *pdriver) {
#if 0
  // do we need to flush the read buffer ? before or after uncorking it ?
  int alarm_handle;
#endif
  pa_operation *paop;

  pdriver->abs_maxvol_heard = 0.;

  if (!pdriver->is_corked) return;

#if 0
  if (!pdriver->is_output) {
    // flush the stream if we are reading from it
    alarm_handle = lives_alarm_set(LIVES_SHORTEST_TIMEOUT);

    pa_threaded_mainloop_lock(pa_mloop);
    paop = pa_stream_flush(pdriver->pstream, NULL, NULL);
    pa_threaded_mainloop_unlock(pa_mloop);

    while (pa_operation_get_state(paop) == PA_OPERATION_RUNNING && !lives_alarm_get(alarm_handle)) {
      sched_yield();
      lives_usleep(prefs->sleep_time);
    }

    lives_alarm_clear(alarm_handle);
    pa_operation_unref(paop);
  }
#endif

  pa_threaded_mainloop_lock(pa_mloop);
  paop = pa_stream_cork(pdriver->pstream, 0, uncorked_cb, pdriver);
  pa_threaded_mainloop_unlock(pa_mloop);

  if (pdriver->is_output) {
    pa_operation_unref(paop);
    return; // let it uncork in its own time...
  }

#if 0
  alarm_handle = lives_alarm_set(LIVES_SHORTEST_TIMEOUT);

  while (pa_operation_get_state(paop) == PA_OPERATION_RUNNING && !lives_alarm_get(alarm_handle)) {
    sched_yield();
    lives_usleep(prefs->sleep_time);
  }

  lives_alarm_clear(alarm_handle);

  alarm_handle = lives_alarm_set(LIVES_SHORTEST_TIMEOUT);

  // flush the stream again if we are reading from it
  pa_threaded_mainloop_lock(pa_mloop);
  paop = pa_stream_flush(pdriver->pstream, NULL, NULL);
  pa_threaded_mainloop_unlock(pa_mloop);

  while (pa_operation_get_state(paop) == PA_OPERATION_RUNNING && !lives_alarm_get(alarm_handle)) {
    sched_yield();
    lives_usleep(prefs->sleep_time);
  }

  lives_alarm_clear(alarm_handle);

#endif

  pa_operation_unref(paop);
}


void pulse_driver_cork(pulse_driver_t *pdriver) {
  pa_operation *paop;

  if (pdriver->is_corked) {
    //g_print("IS CORKED\n");
    return;
  }

  pa_threaded_mainloop_lock(pa_mloop);
  paop = pa_stream_cork(pdriver->pstream, 1, corked_cb, pdriver);
  pa_operation_unref(paop);

  paop = pa_stream_flush(pdriver->pstream, NULL, NULL);
  pa_threaded_mainloop_unlock(pa_mloop);
  pa_operation_unref(paop);
}


///////////////////////////////////////////////////////////////

pulse_driver_t *pulse_get_driver(boolean is_output) {
  if (is_output) return &pulsed;
  return &pulsed_reader;
}


volatile aserver_message_t *pulse_get_msgq(pulse_driver_t *pulsed) {
  if (pulsed->pulsed_died || mainw->aplayer_broken) return NULL;
  return pulsed->msgq;
}


void pa_time_reset(pulse_driver_t *pulsed, int64_t offset) {
  pa_usec_t usec;
  pa_threaded_mainloop_lock(pa_mloop);
  pa_stream_get_time(pulsed->pstream, &usec);
  pa_threaded_mainloop_unlock(pa_mloop);
  pulsed->usec_start = usec + offset / USEC_TO_TICKS;
  pulsed->frames_written = 0;
  mainw->currticks = offset;
  mainw->deltaticks = mainw->startticks = 0;
}


uint64_t lives_pulse_get_time(pulse_driver_t *pulsed) {
  // get the time in ticks since either playback started
  volatile aserver_message_t *msg = pulsed->msgq;
  pa_usec_t usec;
  int err;
  if (msg != NULL && (msg->command == ASERVER_CMD_FILE_SEEK || msg->command == ASERVER_CMD_FILE_OPEN)) {
    boolean timeout;
    int alarm_handle = lives_alarm_set(LIVES_DEFAULT_TIMEOUT);
    while (!(timeout = lives_alarm_get(alarm_handle)) && pulse_get_msgq(pulsed) != NULL) {
      sched_yield(); // wait for seek
      lives_usleep(prefs->sleep_time);
    }
    lives_alarm_clear(alarm_handle);
    if (timeout) return -1;
  }

  do {
    pa_threaded_mainloop_lock(pa_mloop);
    err = pa_stream_get_time(pulsed->pstream, &usec);
    pa_threaded_mainloop_unlock(pa_mloop);
    sched_yield();
    lives_usleep(prefs->sleep_time);
  } while (usec == 0 && err == 0);
#ifdef DEBUG_PA_TIME
  g_print("gettime3 %d %ld %ld %ld %f\n", err, usec, pulsed->usec_start, (usec - pulsed->usec_start) * USEC_TO_TICKS,
          (usec - pulsed->usec_start) * USEC_TO_TICKS / 100000000.);
#endif
  return (uint64_t)((usec - pulsed->usec_start) * USEC_TO_TICKS);
}


double lives_pulse_get_pos(pulse_driver_t *pulsed) {
  // get current time position (seconds) in audio file
  return pulsed->real_seek_pos / (double)(afile->arps * afile->achans * afile->asampsize / 8);
}


boolean pulse_audio_seek_frame(pulse_driver_t *pulsed, int frame) {
  // seek to frame "frame" in current audio file
  // position will be adjusted to (floor) nearest sample
  int64_t seekstart;
  volatile aserver_message_t *pmsg;
  int alarm_handle = lives_alarm_set(LIVES_DEFAULT_TIMEOUT);
  boolean timeout;

  if (frame < 1) frame = 1;

  do {
    pmsg = pulse_get_msgq(pulsed);
  } while (!(timeout = lives_alarm_get(alarm_handle)) && pmsg != NULL && pmsg->command != ASERVER_CMD_FILE_SEEK);
  if (timeout || pulsed->playing_file == -1) {
    if (timeout) LIVES_WARN("PA connect timed out");
    lives_alarm_clear(alarm_handle);
    return FALSE;
  }
  lives_alarm_clear(alarm_handle);
  if (frame > afile->frames) frame = afile->frames;
  seekstart = (int64_t)((double)(frame - 1.) / afile->fps * afile->arps) * afile->achans * (afile->asampsize / 8);
  pulse_audio_seek_bytes(pulsed, seekstart, afile);
  return TRUE;
}


int64_t pulse_audio_seek_bytes(pulse_driver_t *pulsed, int64_t bytes, lives_clip_t *sfile) {
  // seek to position "bytes" in current audio file
  // position will be adjusted to (floor) nearest sample

  // if the position is > size of file, we will seek to the end of the file
  volatile aserver_message_t *pmsg;
  int64_t seekstart;

  if (!pulsed->is_corked) {
    boolean timeout;
    int alarm_handle = lives_alarm_set(LIVES_DEFAULT_TIMEOUT);

    do {
      pmsg = pulse_get_msgq(pulsed);
    } while (!(timeout = lives_alarm_get(alarm_handle)) && pmsg != NULL && pmsg->command != ASERVER_CMD_FILE_SEEK);

    if (timeout || pulsed->playing_file == -1) {
      lives_alarm_clear(alarm_handle);
      if (timeout) LIVES_WARN("PA connect timed out");
      return 0;
    }
    lives_alarm_clear(alarm_handle);
  }

  seekstart = ((int64_t)(bytes / sfile->achans / (sfile->asampsize / 8))) * sfile->achans * (sfile->asampsize / 8);

  if (seekstart < 0) seekstart = 0;
  if (seekstart > sfile->afilesize) seekstart = sfile->afilesize;

  pulse_message2.command = ASERVER_CMD_FILE_SEEK;
  pulse_message2.next = NULL;
  pulse_message2.data = lives_strdup_printf("%"PRId64, seekstart);
  if (pulsed->msgq == NULL) pulsed->msgq = &pulse_message2;
  else pulsed->msgq->next = &pulse_message2;
  return seekstart;
}


boolean pulse_try_reconnect(void) {
  boolean timeout;
  int alarm_handle;
  do_threaded_dialog(_("Resetting pulseaudio connection..."), FALSE);

  pulse_shutdown();
  mainw->pulsed = NULL;

  lives_system("pulseaudio -k", TRUE);
  alarm_handle = lives_alarm_set(LIVES_SHORTEST_TIMEOUT);
  while (!(timeout = lives_alarm_get(alarm_handle))) {
    sched_yield();
    lives_usleep(prefs->sleep_time);
    threaded_dialog_spin(0.);
  }
  lives_alarm_clear(alarm_handle);

  if (!lives_pulse_init(9999)) {
    end_threaded_dialog();
    goto err123; // init server failed
  }
  pulse_audio_init(); // reset vars
  pulse_audio_read_init(); // reset vars
  mainw->pulsed = pulse_get_driver(TRUE);
  if (pulse_driver_activate(mainw->pulsed)) { // activate driver
    goto err123;
  }
  if (prefs->perm_audio_reader && prefs->audio_src == AUDIO_SRC_EXT) {
    // create reader connection now, if permanent
    pulse_rec_audio_to_clip(-1, -1, RECA_EXTERNAL);
  }
  end_threaded_dialog();
  d_print(_("\nConnection to pulseaudio was reset.\n"));
  return TRUE;

err123:
  mainw->aplayer_broken = TRUE;
  mainw->pulsed = NULL;
  do_pulse_lost_conn_error();
  return FALSE;
}


void pulse_aud_pb_ready(int fileno) {
  // TODO - can we merge with switch_audio_clip() ?

  // prepare to play file fileno
  // - set loop mode
  // - check if we need to reconnect
  // - set vals
  char *tmpfilename = NULL;
  lives_clip_t *sfile = mainw->files[fileno];
  int asigned = !(sfile->signed_endian & AFORM_UNSIGNED);
  int aendian = !(sfile->signed_endian & AFORM_BIG_ENDIAN);

  pa_threaded_mainloop_lock(pa_mloop);
  if (mainw->pulsed != NULL) pulse_driver_uncork(mainw->pulsed);
  pa_threaded_mainloop_unlock(pa_mloop);

  // called at pb start and rec stop (after rec_ext_audio)
  if (mainw->pulsed != NULL && mainw->aud_rec_fd == -1) {
    mainw->pulsed->is_paused = FALSE;
    mainw->pulsed->mute = mainw->mute;
    if (mainw->loop_cont && !mainw->preview) {
      if (mainw->ping_pong && prefs->audio_opts & AUDIO_OPTS_FOLLOW_FPS && mainw->multitrack == NULL)
        mainw->pulsed->loop = AUDIO_LOOP_PINGPONG;
      else mainw->pulsed->loop = AUDIO_LOOP_FORWARD;
    } else mainw->pulsed->loop = AUDIO_LOOP_NONE;
    if (sfile->achans > 0 && (!mainw->preview || (mainw->preview && mainw->is_processing)) &&
        (sfile->laudio_time > 0. || sfile->opening ||
         (mainw->multitrack != NULL && mainw->multitrack->is_rendering &&
          lives_file_test((tmpfilename = lives_get_audio_file_name(fileno)), LIVES_FILE_TEST_EXISTS)))) {
      boolean timeout;
      int alarm_handle;

      lives_freep((void **)&tmpfilename);
      mainw->pulsed->in_achans = sfile->achans;
      mainw->pulsed->in_asamps = sfile->asampsize;
      mainw->pulsed->in_arate = sfile->arate;
      mainw->pulsed->usigned = !asigned;
      mainw->pulsed->seek_end = sfile->afilesize;

      if ((aendian && (capable->byte_order == LIVES_BIG_ENDIAN)) || (!aendian && (capable->byte_order == LIVES_LITTLE_ENDIAN)))
        mainw->pulsed->reverse_endian = TRUE;
      else mainw->pulsed->reverse_endian = FALSE;

      alarm_handle = lives_alarm_set(LIVES_DEFAULT_TIMEOUT);
      while (!(timeout = lives_alarm_get(alarm_handle)) && pulse_get_msgq(mainw->pulsed) != NULL) {
        sched_yield(); // wait for seek
        lives_usleep(prefs->sleep_time);
      }

      if (timeout) pulse_try_reconnect();

      lives_alarm_clear(alarm_handle);

      if ((mainw->multitrack == NULL || mainw->multitrack->is_rendering ||
           sfile->opening) && (mainw->event_list == NULL || mainw->record || (mainw->preview && mainw->is_processing))) {
        // tell pulse server to open audio file and start playing it
        pulse_message.command = ASERVER_CMD_FILE_OPEN;
        pulse_message.data = lives_strdup_printf("%d", fileno);
        pulse_message.next = NULL;
        mainw->pulsed->msgq = &pulse_message;
        pulse_audio_seek_bytes(mainw->pulsed, sfile->aseek_pos, sfile);
        if (seek_err) {
          if (pulse_try_reconnect()) pulse_audio_seek_bytes(mainw->pulsed, sfile->aseek_pos, sfile);
        }
        mainw->pulsed->in_use = TRUE;
        mainw->rec_aclip = fileno;
        mainw->rec_avel = sfile->pb_fps / sfile->fps;
        mainw->rec_aseek = (double)sfile->aseek_pos / (double)(sfile->arps * sfile->achans * (sfile->asampsize / 8));
      }
    }
    if (mainw->agen_key != 0 && mainw->multitrack == NULL) mainw->pulsed->in_use = TRUE; // audio generator is active
  }
}

#undef afile

#endif

