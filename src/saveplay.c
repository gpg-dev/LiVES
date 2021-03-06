// saveplay.c
// LiVES (lives-exe)
// (c) G. Finch 2003 - 2018 (salsaman+lives@gmail.com)
// released under the GNU GPL 3 or later
// see file ../COPYING or www.gnu.org for licensing details

#include <fcntl.h>
#include <glib.h>

#include "main.h"
#include "callbacks.h"
#include "resample.h"
#include "effects.h"
#include "audio.h"
#include "htmsocket.h"
#include "cvirtual.h"
#include "interface.h"

boolean _start_playback(livespointer data) {
  int new_file, old_file;
  int play_type = LIVES_POINTER_TO_INT(data);
  if (play_type != 8 && mainw->noswitch) return TRUE;
  switch (play_type) {
  case 8: case 6: case 0:
    /// normal play
    play_all(play_type == 8);
    if (play_type == 6) {
      /// triggered by generator
      // need to set this after playback ends; this stops the key from being activated (again) in effects.c
      // also stops the (now defunct instance being unreffed)
      mainw->gen_started_play = TRUE;
    }
    break;
  case 1:
    /// play selection
    if (!mainw->multitrack) play_sel();
    else multitrack_play_sel(NULL, mainw->multitrack);
    break;
  case 2:
    /// play stream
    mainw->play_start = 1;
    mainw->play_end = INT_MAX;
    play_file();
    break;
  case 3:
    /// osc playall
    mainw->osc_auto = 1; ///< request notifiction of success
    play_all(FALSE);
    mainw->osc_auto = 0;
    break;
  case 4:
    /// osc playsel
    mainw->osc_auto = 1; ///< request notifiction of success
    if (!mainw->multitrack) play_sel();
    else multitrack_play_sel(NULL, mainw->multitrack);
    mainw->osc_auto = 0;
    break;
  case 5:
    /// clipboard
    play_file();
    mainw->loop = mainw->oloop;
    mainw->loop_cont = mainw->oloop_cont;

    if (mainw->pre_play_file > 0) {
      switch_to_file(0, mainw->pre_play_file);
    } else {
      mainw->current_file = -1;
      close_current_file(0);
    }
    if (mainw->cancelled == CANCEL_AUDIO_ERROR) {
      handle_audio_timeout();
      mainw->cancelled = CANCEL_ERROR;
    }
    break;
  case 7:
    /// yuv4mpeg
    new_file = mainw->current_file;
    old_file = mainw->pre_play_file;
    play_file();
    if (mainw->current_file != old_file && mainw->current_file != new_file)
      old_file = mainw->current_file; // we could have rendered to a new file
    mainw->current_file = new_file;
    // close this temporary clip
    close_current_file(old_file);
    mainw->pre_play_file = -1;
    break;
  default:
    /// do nothing
    break;
  }
  return FALSE;
}

LIVES_GLOBAL_INLINE boolean start_playback(int type) {return  _start_playback(LIVES_INT_TO_POINTER(type));}

LIVES_GLOBAL_INLINE void start_playback_async(int type) {
  lives_idle_add(_start_playback, LIVES_INT_TO_POINTER(type));
  //lives_proc_thread_create(0, (lives_funcptr_t)_start_playback, 0, "i", type);
  //_start_playback(LIVES_INT_TO_POINTER(type));
}


boolean save_clip_values(int which) {
  lives_clip_t *sfile = mainw->files[which];
  char *lives_header_new;
  boolean all_ok = FALSE;
  int asigned, endian;
  int retval;

  if (which == 0 || which == mainw->scrap_file || which == mainw->ascrap_file) return TRUE;

  set_signal_handlers((SignalHandlerPointer)defer_sigint); // ignore ctrl-c

  asigned = !(sfile->signed_endian & AFORM_UNSIGNED);
  endian = sfile->signed_endian & AFORM_BIG_ENDIAN;
  if (which == mainw->ascrap_file)
    lives_header_new = lives_build_filename(prefs->workdir, sfile->handle, LIVES_ACLIP_HEADER_NEW, NULL);
  else
    lives_header_new = lives_build_filename(prefs->workdir, sfile->handle, LIVES_CLIP_HEADER_NEW, NULL);

  do {
    THREADVAR(com_failed) = THREADVAR(write_failed) = FALSE;
    mainw->clip_header = fopen(lives_header_new, "w");
    if (!mainw->clip_header) {
      retval = do_write_failed_error_s_with_retry(lives_header_new, lives_strerror(errno));
      if (retval == LIVES_RESPONSE_CANCEL) {
        set_signal_handlers((SignalHandlerPointer)catch_sigint);
        if (mainw->signal_caught) catch_sigint(mainw->signal_caught);
        lives_free(lives_header_new);
        return FALSE;
      }
    } else {
      sfile->header_version = LIVES_CLIP_HEADER_VERSION;
      do {
        retval = 0;
        if (!save_clip_value(which, CLIP_DETAILS_HEADER_VERSION, &sfile->header_version)) break;
        if (!save_clip_value(which, CLIP_DETAILS_BPP, &sfile->bpp)) break;
        if (sfile->clip_type == CLIP_TYPE_FILE && sfile->ext_src) {
          lives_clip_data_t *cdata = ((lives_decoder_t *)sfile->ext_src)->cdata;
          double dfps = (double)cdata->fps;
          if (!save_clip_value(which, CLIP_DETAILS_FPS, &dfps)) break;
          if (!save_clip_value(which, CLIP_DETAILS_PB_FPS, &sfile->fps)) break;
        } else {
          if (!save_clip_value(which, CLIP_DETAILS_FPS, &sfile->fps)) break;
          if (!save_clip_value(which, CLIP_DETAILS_PB_FPS, &sfile->pb_fps)) break;
        }
        if (!save_clip_value(which, CLIP_DETAILS_WIDTH, &sfile->hsize)) break;
        if (!save_clip_value(which, CLIP_DETAILS_HEIGHT, &sfile->vsize)) break;
        if (!save_clip_value(which, CLIP_DETAILS_INTERLACE, &sfile->interlace)) break;
        if (!save_clip_value(which, CLIP_DETAILS_UNIQUE_ID, &sfile->unique_id)) break;
        if (!save_clip_value(which, CLIP_DETAILS_ARATE, &sfile->arps)) break;
        if (!save_clip_value(which, CLIP_DETAILS_PB_ARATE, &sfile->arate)) break;
        if (!save_clip_value(which, CLIP_DETAILS_ACHANS, &sfile->achans)) break;
        if (sfile->achans > 0) {
          if (!save_clip_value(which, CLIP_DETAILS_ASIGNED, &asigned)) break;
          if (!save_clip_value(which, CLIP_DETAILS_AENDIAN, &endian)) break;
        }
        if (!save_clip_value(which, CLIP_DETAILS_ASAMPS, &sfile->asampsize)) break;
        if (!save_clip_value(which, CLIP_DETAILS_FRAMES, &sfile->frames)) break;
        if (!save_clip_value(which, CLIP_DETAILS_GAMMA_TYPE, &sfile->gamma_type)) break;
        if (!save_clip_value(which, CLIP_DETAILS_TITLE, sfile->title)) break;
        if (!save_clip_value(which, CLIP_DETAILS_AUTHOR, sfile->author)) break;
        if (!save_clip_value(which, CLIP_DETAILS_COMMENT, sfile->comment)) break;
        if (!save_clip_value(which, CLIP_DETAILS_PB_FRAMENO, &sfile->frameno)) break;
        if (!save_clip_value(which, CLIP_DETAILS_CLIPNAME, sfile->name)) break;
        if (!save_clip_value(which, CLIP_DETAILS_FILENAME, sfile->file_name)) break;
        if (!save_clip_value(which, CLIP_DETAILS_KEYWORDS, sfile->keywords)) break;
        if (sfile->clip_type == CLIP_TYPE_FILE && sfile->ext_src) {
          lives_decoder_t *dplug = (lives_decoder_t *)sfile->ext_src;
          if (!save_clip_value(which, CLIP_DETAILS_DECODER_NAME, (void *)dplug->decoder->name)) break;
        }
        all_ok = TRUE;
      } while (FALSE);

      fclose(mainw->clip_header);

      if (!all_ok) {
        retval = do_write_failed_error_s_with_retry(lives_header_new, NULL);
      } else {
        char *lives_header;
        if (which == mainw->ascrap_file)
          lives_header = lives_build_filename(prefs->workdir, sfile->handle, LIVES_ACLIP_HEADER, NULL);
        else
          lives_header = lives_build_filename(prefs->workdir, sfile->handle, LIVES_CLIP_HEADER, NULL);
        // TODO - check the sizes before and after
        lives_cp(lives_header_new, lives_header);
        lives_free(lives_header);
        if (THREADVAR(com_failed) || THREADVAR(write_failed)) {
          retval = do_write_failed_error_s_with_retry(lives_header_new, NULL);
        } else lives_rm(lives_header_new);
      }
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  if (mainw->signal_caught) catch_sigint(mainw->signal_caught);
  set_signal_handlers((SignalHandlerPointer)catch_sigint);

  lives_free(lives_header_new);
  mainw->clip_header = NULL;

  if (retval == LIVES_RESPONSE_CANCEL) return FALSE;

  return TRUE;
}


boolean read_file_details(const char *file_name, boolean is_audio, boolean is_img) {
  // get preliminary details

  // is_audio set to TRUE prevents us from checking for images, and deleting the (existing) first frame
  // therefore it is IMPORTANT to set it when loading new audio for an existing clip !

  // is_img will force unpacking of img into frames and return the count

  char *tmp, *com = lives_strdup_printf("%s get_details \"%s\" \"%s\" \"%s\" %d", prefs->backend_sync, cfile->handle,
                                        (tmp = lives_filename_from_utf8(file_name, -1, NULL, NULL, NULL)),
                                        get_image_ext_for_type(IMG_TYPE_BEST), mainw->opening_loc ? 3 :
                                        is_audio ? 2 : is_img ? 4 : 0);
  lives_free(tmp);
  lives_popen(com, FALSE, mainw->msg, MAINW_MSG_SIZE);
  lives_free(com);
  if (THREADVAR(com_failed)) {
    THREADVAR(com_failed) = FALSE;
    return FALSE;
  }
  return TRUE;
}


const char *get_deinterlace_string(void) {
  if (mainw->open_deint) {
    if (USE_MPV) return "--deinterlace=yes";
    return "-vf pp=ci";
  } else return "";
}


ulong deduce_file(const char *file_name, double start, int end) {
  // this is a utility function to deduce whether we are dealing with a file,
  // a selection, a backup, or a location
  char short_file_name[PATH_MAX];
  ulong uid;
  mainw->img_concat_clip = -1;

  if (lives_strrstr(file_name, "://") && strncmp(file_name, "dvd://", 6)) {
    mainw->opening_loc = TRUE;
    uid = open_file(file_name);
    mainw->opening_loc = FALSE;
  } else {
    lives_snprintf(short_file_name, PATH_MAX, "%s", file_name);
    if (!(strcmp(file_name + strlen(file_name) - 4, "."LIVES_FILE_EXT_BACKUP))) {
      uid = restore_file(file_name);
    } else {
      uid = open_file_sel(file_name, start, end);
    }
  }
  return uid;
}


ulong open_file(const char *file_name) {
  // this function should be called to open a whole file
  return open_file_sel(file_name, 0., 0);
}


static boolean rip_audio_cancelled(int old_file, weed_plant_t *mt_pb_start_event,
                                   boolean mt_has_audio_file) {

  if (mainw->cancelled == CANCEL_KEEP) {
    // user clicked "enough"
    mainw->cancelled = CANCEL_NONE;
    return TRUE;
  }

  end_threaded_dialog();

  d_print("\n");
  d_print_cancelled();
  close_current_file(old_file);

  if (mainw->multitrack) {
    mainw->multitrack->pb_start_event = mt_pb_start_event;
    mainw->multitrack->has_audio_file = mt_has_audio_file;
  }

  lives_freep((void **)&mainw->file_open_params);
  lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
  return FALSE;
}


void pad_init_silence(void) {
  cfile->undo1_dbl = 0.;
  cfile->undo2_dbl = CLIP_TOTAL_TIME(mainw->current_file) - cfile->laudio_time;
  cfile->undo_arate = cfile->arate;
  cfile->undo_signed_endian = cfile->signed_endian;
  cfile->undo_achans = cfile->achans;
  cfile->undo_asampsize = cfile->asampsize;
  cfile->undo_arps = cfile->arps;
  d_print(_("Auto padding with %.4f seconds of silence at start..."), cfile->undo2_dbl);
  if (on_ins_silence_activate(NULL, NULL)) d_print_done();
  else d_print("\n");
}


#define AUDIO_FRAMES_TO_READ 100

ulong open_file_sel(const char *file_name, double start, int frames) {
  LiVESResponseType response;
  char msg[256], loc[PATH_MAX];
  char *tmp = NULL;
  char *isubfname = NULL;
  char *fname = lives_strdup(file_name), *msgstr;
  char *com, *what;

  int withsound = 1;
  int old_file = mainw->current_file;
  int new_file = old_file;

  int achans, arate, arps, asampsize;
  int current_file;
  int extra_frames = 0;
  int probed_achans = 0;

  boolean mt_has_audio_file = TRUE;

  const lives_clip_data_t *cdata;

  weed_plant_t *mt_pb_start_event = NULL;

  if (!lives_file_test(fname, LIVES_FILE_TEST_EXISTS)) {
    do_no_loadfile_error(fname);
    lives_free(fname);
    return 0;
  }

  if (old_file == -1 || !CURRENT_CLIP_IS_VALID || !cfile->opening) {
    new_file = mainw->first_free_file;

    if (!get_new_handle(new_file, fname)) {
      lives_free(fname);
      return 0;
    }
    lives_free(fname);

    lives_set_cursor_style(LIVES_CURSOR_BUSY, NULL);
    lives_widget_context_update();

    if (frames == 0) {
      com = lives_strdup_printf(_("Opening %s"), file_name);
    } else {
      com = lives_strdup_printf(_("Opening %s start time %.2f sec. frames %d"), file_name, start, frames);
    }
    d_print(""); // exhaust "switch" message

    d_print(com);
    lives_free(com);

    if (!mainw->save_with_sound) {
      d_print(_(" without sound"));
      withsound = 0;
    }

    mainw->current_file = new_file;

    /// probe the file to see what it might be...
    read_file_details(file_name, FALSE, FALSE);
    lives_rm(cfile->info_file);
    if (THREADVAR(com_failed)) return 0;

    if (*mainw->msg) add_file_info(cfile->handle, FALSE);

    if (mainw->multitrack) {
      // set up for opening preview
      mt_pb_start_event = mainw->multitrack->pb_start_event;
      mt_has_audio_file = mainw->multitrack->has_audio_file;
      mainw->multitrack->pb_start_event = NULL;
      mainw->multitrack->has_audio_file = TRUE;
    }

    cfile->img_type = lives_image_ext_to_img_type(prefs->image_ext);
    if ((!strcmp(cfile->type, LIVES_IMAGE_TYPE_JPEG) || !strcmp(cfile->type, LIVES_IMAGE_TYPE_PNG))) {
      read_file_details(file_name, FALSE, TRUE);
      add_file_info(cfile->handle, FALSE);
      if (cfile->frames == 0) {
        d_print_failed();
        close_current_file(old_file);
        if (mainw->multitrack) {
          mainw->multitrack->pb_start_event = mt_pb_start_event;
          mainw->multitrack->has_audio_file = mt_has_audio_file;
        }
        lives_freep((void **)&mainw->file_open_params);
        lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
        return 0;
      }
      goto img_load;
    }

    if (prefs->instant_open && !mainw->opening_loc) {
      // cd to clip directory - so decoder plugins can write temp files
      char *ppath = lives_build_filename(prefs->workdir, cfile->handle, NULL);
      char *cwd = lives_get_current_dir();

      if (!mainw->decoders_loaded) {
        mainw->decoder_list = load_decoders();
        mainw->decoders_loaded = TRUE;
      }

      lives_chdir(ppath, FALSE);
      lives_free(ppath);

      cdata = get_decoder_cdata(mainw->current_file, prefs->disabled_decoders, NULL);

      lives_chdir(cwd, FALSE);
      lives_free(cwd);

      if (cfile->ext_src) {
        lives_decoder_t *dplug = (lives_decoder_t *)cfile->ext_src;
        cfile->opening = TRUE;
        cfile->clip_type = CLIP_TYPE_FILE;
        cfile->img_type = IMG_TYPE_BEST; // override the pref

        if (cdata->frame_width > 0) {
          cfile->hsize = cdata->frame_width;
          cfile->vsize = cdata->frame_height;
        } else {
          cfile->hsize = cdata->width;
          cfile->vsize = cdata->height;
        }

        if (cfile->frames > cdata->nframes && cfile->frames != 123456789) {
          extra_frames = cfile->frames - cdata->nframes;
        }
        cfile->frames = cdata->nframes;
        if (!*cfile->author)
          lives_snprintf(cfile->author, 1024, "%s", cdata->author);
        if (!*cfile->title)
          lives_snprintf(cfile->title, 1024, "%s", cdata->title);
        if (!*cfile->comment)
          lives_snprintf(cfile->comment, 1024, "%s", cdata->comment);

        if (frames > 0 && cfile->frames > frames) {
          cfile->frames = frames;
          extra_frames = 0;
        }

        cfile->start = 1;
        cfile->end = cfile->frames;

        what = (_("creating the frame index for the clip"));

        do {
          response = LIVES_RESPONSE_OK;
          create_frame_index(mainw->current_file, TRUE, cfile->fps * (start == 0 ? 0 : start - 1),
                             frames == 0 ? cfile->frames : frames);
          if (!cfile->frame_index) {
            response = do_memory_error_dialog(what, (frames == 0 ? cfile->frames : frames) * 4);
          }
        } while (response == LIVES_RESPONSE_RETRY);
        lives_free(what);
        if (response == LIVES_RESPONSE_CANCEL) {
          return 0;
        }
        probed_achans = cfile->achans;
        cfile->arate = cfile->arps = cdata->arate;
        cfile->achans = cdata->achans;
        cfile->asampsize = cdata->asamps;

        cfile->signed_endian = get_signed_endian(cdata->asigned, capable->byte_order == LIVES_LITTLE_ENDIAN);

        if (cfile->achans > 0 && (dplug->decoder->rip_audio) && withsound == 1) {
          // call rip_audio() in the decoder plugin
          // the plugin gets a chance to do any internal cleanup in rip_audio_cleanup()

          int64_t stframe = cfile->fps * start + .5;
          int64_t maxframe = (stframe + (frames == 0)) ? cfile->frames : frames;
          int64_t nframes = AUDIO_FRAMES_TO_READ;
          char *afile = get_audio_file_name(mainw->current_file, TRUE);

          msgstr = lives_strdup_printf(_("Opening audio for %s"), file_name);

          if (!LIVES_IS_PLAYING) resize(1);

          mainw->cancelled = CANCEL_NONE;

          if (!LIVES_IS_PLAYING) {
            mainw->cancel_type = CANCEL_SOFT;
            do_threaded_dialog(msgstr, TRUE);
            mainw->cancel_type = CANCEL_KILL;
          }

          do {
            if (stframe + nframes > maxframe) nframes = maxframe - stframe;
            if (nframes <= 0) break;
            (dplug->decoder->rip_audio)(cdata, afile, stframe, nframes, NULL);
            threaded_dialog_spin(0.);
            stframe += nframes;
          } while (mainw->cancelled == CANCEL_NONE);

          if (dplug->decoder->rip_audio_cleanup) {
            (dplug->decoder->rip_audio_cleanup)(cdata);
          }

          if (mainw->cancelled != CANCEL_NONE) {
            if (!rip_audio_cancelled(old_file, mt_pb_start_event, mt_has_audio_file)) {
              lives_free(afile);
              return 0;
            }
          }
          end_threaded_dialog();
          lives_free(msgstr);
          lives_free(afile);
        } else {
          cfile->arate = 0.;
          cfile->achans = cfile->asampsize = 0;
        }

        cfile->fps = cfile->pb_fps = cdata->fps;
        d_print("\n");

        if (cfile->achans == 0 && probed_achans > 0 && withsound == 1) {
          // plugin returned no audio, try with mplayer / mpv
          if (probed_achans > MAX_ACHANS) {
            probed_achans = MAX_ACHANS;
            d_print(_("Forcing audio channels to %d\n"), MAX_ACHANS);
          }

          if (!mainw->file_open_params) {
#if 1
            mainw->file_open_params = lives_strdup("alang eng");
#else
            mainw->file_open_params = lives_strdup("");
#endif
          }
          com = lives_strdup_printf("%s open \"%s\" \"%s\" %d %s:%s %.2f %d %d \"%s\"", prefs->backend, cfile->handle,
                                    (tmp = lives_filename_from_utf8(file_name, -1, NULL, NULL, NULL)), -1,
                                    prefs->image_ext, get_image_ext_for_type(IMG_TYPE_BEST), start, frames, probed_achans,
                                    mainw->file_open_params);

          lives_free(tmp);

          lives_rm(cfile->info_file);
          lives_system(com, FALSE);
          lives_free(com);

          // if we have a quick-opening file, display the first and last frames now
          // for some codecs this can be helpful since we can locate the last frame while audio is loading
          if (cfile->clip_type == CLIP_TYPE_FILE && !LIVES_IS_PLAYING) resize(1);

          mainw->effects_paused = FALSE; // set to TRUE if user clicks "Enough"

          msgstr = lives_strdup_printf(_("Opening audio"), file_name);
          if (!do_progress_dialog(TRUE, TRUE, msgstr)) {
            // error or user cancelled or switched to another clip
            lives_free(msgstr);

            cfile->opening_frames = -1;

            if (mainw->multitrack) {
              mainw->multitrack->pb_start_event = mt_pb_start_event;
              mainw->multitrack->has_audio_file = mt_has_audio_file;
            }

            if (mainw->cancelled == CANCEL_NO_PROPOGATE) {
              lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
              mainw->cancelled = CANCEL_NONE;
              return 0;
            }

            // cancelled
            if (mainw->cancelled != CANCEL_ERROR) {
              lives_kill_subprocesses(cfile->handle, TRUE);
            }

            lives_freep((void **)&mainw->file_open_params);
            close_current_file(old_file);
            lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
            if (mainw->error) {
              do_error_dialog(mainw->msg);
              mainw->error = 0;
              clear_mainw_msg();
            }
            sensitize();
            return 0;
          }
          lives_free(msgstr);

          cfile->opening = FALSE;

          wait_for_bg_audio_sync(mainw->current_file);
          if (mainw->error == 0) add_file_info(cfile->handle, TRUE);
          mainw->error = 0;
          get_total_time(cfile);

          if (prefs->auto_trim_audio) {
            if ((cdata->sync_hint & SYNC_HINT_VIDEO_PAD_START) && cdata->video_start_time <= 1.) {
              // pad with blank frames at start
              int st_extra_frames = cdata->video_start_time * cfile->fps;
              insert_blank_frames(mainw->current_file, st_extra_frames, 0, WEED_PALETTE_RGB24);
              cfile->video_time += st_extra_frames / cfile->fps;
              extra_frames -= st_extra_frames;
              showclipimgs();
              if (!mainw->multitrack)
                redraw_timeline(mainw->current_file);
            }

            if ((cfile->frames + extra_frames) / cfile->fps > cfile->laudio_time) {
              extra_frames = (cfile->laudio_time - (double)cfile->frames / cfile->fps) * cfile->fps;
            }

            if (extra_frames > 0 || ((cdata->sync_hint & SYNC_HINT_VIDEO_PAD_END)
                                     && (double)cfile->frames / cfile->fps < cfile->laudio_time)) {
              // pad with blank frames at end
              if (cdata->sync_hint & SYNC_HINT_VIDEO_PAD_END) {
                int xextra_frames = (cfile->laudio_time - (double)cfile->frames / cfile->fps) * cfile->fps;
                if (xextra_frames > extra_frames) extra_frames = xextra_frames;
              }
              insert_blank_frames(mainw->current_file, extra_frames, cfile->frames, WEED_PALETTE_RGB24);
              cfile->video_time += extra_frames / cfile->fps;
              load_end_image(cfile->end);
            }
            if (cfile->laudio_time > cfile->video_time + AV_TRACK_MIN_DIFF && cfile->frames > 0) {
              if (cdata->sync_hint & SYNC_HINT_AUDIO_TRIM_START) {
                cfile->undo1_dbl = 0.;
                cfile->undo2_dbl = cfile->laudio_time - cfile->video_time;
                d_print(_("Auto trimming %.4f seconds of audio at start..."), cfile->undo2_dbl);
                if (on_del_audio_activate(NULL, NULL)) d_print_done();
                else d_print("\n");
                cfile->changed = FALSE;
              }
            }
            if (cfile->laudio_time > cfile->video_time + AV_TRACK_MIN_DIFF && cfile->frames > 0) {
              if (cdata->sync_hint & SYNC_HINT_AUDIO_TRIM_END) {
                cfile->end = cfile->frames;
                d_print(_("Auto trimming %.4f seconds of audio at end..."), cfile->laudio_time - cfile->video_time);
                if (on_trim_audio_activate(NULL, LIVES_INT_TO_POINTER(0))) d_print_done();
                else d_print("\n");
                cfile->changed = FALSE;
              }
            }
            if (!mainw->effects_paused && cfile->afilesize > 0 && cfile->achans > 0
                && CLIP_TOTAL_TIME(mainw->current_file) > cfile->laudio_time + AV_TRACK_MIN_DIFF) {
              if (cdata->sync_hint & SYNC_HINT_AUDIO_PAD_START) {
                pad_init_silence();
                cfile->changed = FALSE;
              }
              if (cdata->sync_hint & SYNC_HINT_AUDIO_PAD_END) {
                cfile->undo1_dbl = cfile->laudio_time;
                cfile->undo2_dbl = CLIP_TOTAL_TIME(mainw->current_file) - cfile->laudio_time;
                cfile->undo_arate = cfile->arate;
                cfile->undo_signed_endian = cfile->signed_endian;
                cfile->undo_achans = cfile->achans;
                cfile->undo_asampsize = cfile->asampsize;
                cfile->undo_arps = cfile->arps;
                d_print(_("Auto padding with %.4f seconds of silence at end..."), cfile->undo2_dbl);
                if (on_ins_silence_activate(NULL, NULL)) d_print_done();
                else d_print("\n");
                cfile->changed = FALSE;
		// *INDENT-OFF*
              }}}}
	// *INDENT-ON*

        get_mime_type(cfile->type, 40, cdata);
        save_frame_index(mainw->current_file);
      }
    }

    if (cfile->ext_src) {
      if (mainw->open_deint) {
        // override what the plugin says
        cfile->deinterlace = TRUE;
        cfile->interlace = LIVES_INTERLACE_TOP_FIRST; // guessing
        save_clip_value(mainw->current_file, CLIP_DETAILS_INTERLACE, &cfile->interlace);
        if (THREADVAR(com_failed) || THREADVAR(write_failed)) do_header_write_error(mainw->current_file);
      }
    } else {
      // be careful, here we switch from mainw->opening_loc to cfile->opening_loc
      if (mainw->opening_loc) {
        cfile->opening_loc = TRUE;
        mainw->opening_loc = FALSE;
      } else {
        if (cfile->f_size > prefs->warn_file_size * 1000000. && mainw->is_ready && frames == 0) {
          char *fsize_ds = lives_format_storage_space_string((uint64_t)cfile->f_size);
          char *warn = lives_strdup_printf(
                         _("\nLiVES cannot Instant Open this file, it may take some time to load.\n"
                           "Are you sure you wish to continue ?"),
                         fsize_ds);
          lives_free(fsize_ds);
          if (!do_warning_dialog_with_check(warn, WARN_MASK_FSIZE)) {
            lives_free(warn);
            close_current_file(old_file);
            if (mainw->multitrack) {
              mainw->multitrack->pb_start_event = mt_pb_start_event;
              mainw->multitrack->has_audio_file = mt_has_audio_file;
            }
            lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
            return 0;
          }
          lives_free(warn);
          d_print(_(" - please be patient."));

        }
        d_print("\n");
#if defined DEBUG
        g_print("open_file: dpd in\n");
#endif
      }
    }

    // set undo_start and undo_end for preview
    cfile->undo_start = 1;
    cfile->undo_end = cfile->frames;

    if (cfile->achans > 0) {
      cfile->opening_audio = TRUE;
    }

    // these will get reset as we have no audio file yet, so preserve them
    achans = cfile->achans;
    arate = cfile->arate;
    arps = cfile->arps;
    asampsize = cfile->asampsize;
    cfile->old_frames = cfile->frames;
    cfile->frames = 0;

    // we need this FALSE here, otherwise we will switch straight back here...
    cfile->opening = FALSE;

    // force a resize
    current_file = mainw->current_file;

    cfile->opening = TRUE;
    cfile->achans = achans;
    cfile->arate = arate;
    cfile->arps = arps;
    cfile->asampsize = asampsize;
    cfile->frames = cfile->old_frames;

    if (cfile->frames <= 0) {
      cfile->undo_end = cfile->frames = 123456789;
    }
    if (cfile->hsize * cfile->vsize == 0) {
      cfile->frames = 0;
    }

    if (!mainw->multitrack) get_play_times();

    add_to_clipmenu();
    set_main_title(cfile->file_name, 0);

    mainw->effects_paused = FALSE;

    if (!cfile->ext_src) {
      if (!mainw->file_open_params) mainw->file_open_params = lives_strdup("");

      tmp = lives_strconcat(mainw->file_open_params, get_deinterlace_string(), NULL);
      lives_free(mainw->file_open_params);
      mainw->file_open_params = tmp;

      if (cfile->achans > MAX_ACHANS) {
        cfile->achans = MAX_ACHANS;
        d_print(_("Forcing audio channels to %d\n"), MAX_ACHANS);
      }

      com = lives_strdup_printf("%s open \"%s\" \"%s\" %d %s:%s %.2f %d %d \"%s\"", prefs->backend, cfile->handle,
                                (tmp = lives_filename_from_utf8(file_name, -1, NULL, NULL, NULL)), withsound,
                                prefs->image_ext, get_image_ext_for_type(IMG_TYPE_BEST), start, frames, cfile->achans,
                                mainw->file_open_params);

      lives_rm(cfile->info_file);
      lives_system(com, FALSE);
      lives_free(com);
      lives_free(tmp);

      if (mainw->toy_type == LIVES_TOY_TV) {
        // for LiVES TV we do an auto-preview
        mainw->play_start = cfile->start = cfile->undo_start;
        mainw->play_end = cfile->end = cfile->undo_end;
        mainw->preview = TRUE;
        do {
          desensitize();
          procw_desensitize();
          on_playsel_activate(NULL, NULL);
        } while (mainw->cancelled == CANCEL_KEEP_LOOPING);
        mainw->preview = FALSE;
        on_toy_activate(NULL, LIVES_INT_TO_POINTER(LIVES_TOY_NONE));
        lives_freep((void **)&mainw->file_open_params);
        mainw->cancelled = CANCEL_NONE;
        lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
        return 0;
      }
    }

    //  loading:

    // 'entry point' when we switch back

    // spin until loading is complete
    // afterwards, mainw->msg will contain file details
    cfile->progress_start = cfile->progress_end = 0;

    // (also check for cancel)
    msgstr = lives_strdup_printf(_("Opening %s"), file_name);

    if (!cfile->ext_src && mainw->toy_type != LIVES_TOY_TV) {
      mainw->cs_permitted = TRUE;
      mainw->disk_mon = MONITOR_QUOTA;
      if (!do_progress_dialog(TRUE, TRUE, msgstr)) {
        // user cancelled or switched to another clip
        mainw->cs_permitted = FALSE;
        mainw->disk_mon = 0;

        lives_free(msgstr);
        mainw->effects_paused = FALSE;

        if (mainw->cancelled == CANCEL_NO_PROPOGATE) {
          mainw->cancelled = CANCEL_NONE;
          lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
          return 0;
        }

        // cancelled
        // clean up our temp files
        if (IS_VALID_CLIP(current_file)) mainw->current_file = current_file;
        lives_kill_subprocesses(cfile->handle, TRUE);
        lives_freep((void **)&mainw->file_open_params);
        close_current_file(old_file);
        if (mainw->multitrack) {
          mainw->multitrack->pb_start_event = mt_pb_start_event;
          mainw->multitrack->has_audio_file = mt_has_audio_file;
        }
        lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);

        // mainw->error is TRUE if we could not open the file
        if (mainw->error) {
          d_print_failed();
          do_error_dialog(mainw->msg);
        }
        if (!mainw->multitrack)
          redraw_timeline(mainw->current_file);
        showclipimgs();
        return 0;
      }
      mainw->cs_permitted = FALSE;
      mainw->disk_mon = 0;
    }
    lives_free(msgstr);
  }

  if (cfile->ext_src && cfile->achans > 0) {
    char *afile = get_audio_file_name(mainw->current_file, TRUE);
    char *ofile = get_audio_file_name(mainw->current_file, FALSE);
    rename(afile, ofile);
    lives_free(afile);
    lives_free(ofile);
  }

  cfile->opening = cfile->opening_audio = cfile->opening_only_audio = FALSE;
  cfile->opening_frames = -1;
  mainw->effects_paused = FALSE;

#if defined DEBUG
  g_print("Out of dpd\n");
#endif

  if (mainw->multitrack) {
    mainw->multitrack->pb_start_event = mt_pb_start_event;
    mainw->multitrack->has_audio_file = mt_has_audio_file;
  }

  // mainw->error is TRUE if we could not open the file
  if (mainw->error) {
    do_error_dialog(mainw->msg);
    d_print_failed();
    close_current_file(old_file);
    lives_freep((void **)&mainw->file_open_params);
    lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
    return 0;
  }

  if (cfile->opening_loc) {
    cfile->changed = TRUE;
    cfile->opening_loc = FALSE;
  } else {
    if (prefs->autoload_subs) {
      char filename[512];
      char *subfname;
      lives_subtitle_type_t subtype = SUBTITLE_TYPE_NONE;

      lives_snprintf(filename, 512, "%s", file_name);
      get_filename(filename, FALSE); // strip extension
      isubfname = lives_strdup_printf("%s.%s", filename, LIVES_FILE_EXT_SRT);
      if (lives_file_test(isubfname, LIVES_FILE_TEST_EXISTS)) {
        subfname = lives_build_filename(prefs->workdir, cfile->handle, SUBS_FILENAME "." LIVES_FILE_EXT_SRT, NULL);
        subtype = SUBTITLE_TYPE_SRT;
      } else {
        lives_free(isubfname);
        isubfname = lives_strdup_printf("%s.%s", filename, LIVES_FILE_EXT_SUB);
        if (lives_file_test(isubfname, LIVES_FILE_TEST_EXISTS)) {
          subfname = lives_build_filename(prefs->workdir, cfile->handle, SUBS_FILENAME "." LIVES_FILE_EXT_SUB, NULL);
          subtype = SUBTITLE_TYPE_SUB;
        }
      }
      if (subtype != SUBTITLE_TYPE_NONE) {
        lives_cp(isubfname, subfname);
        if (!THREADVAR(com_failed))
          subtitles_init(cfile, subfname, subtype);
        lives_free(subfname);
      } else {
        lives_freep((void **)&isubfname);
      }
    }
  }

  // now file should be loaded...get full details
  if (!cfile->ext_src) add_file_info(cfile->handle, FALSE);
  cfile->is_loaded = TRUE;

  if (cfile->frames <= 0) {
    if (cfile->afilesize == 0l) {
      // we got neither video nor audio...
      lives_snprintf(msg, 256, "%s", _
                     ("\n\nLiVES was unable to extract either video or audio.\n"
                      "Please check the terminal window for more details.\n"));

      if (!capable->has_mplayer && !capable->has_mplayer2 && !capable->has_mpv) {
        lives_strappend(msg, 256, _("\n\nYou may need to install mplayer, mplayer2 or mpv to open this file.\n"));
      } else {
        if (capable->has_mplayer) {
          get_location(EXEC_MPLAYER, loc, PATH_MAX);
        } else if (capable->has_mplayer2) {
          get_location(EXEC_MPLAYER2, loc, PATH_MAX);
        } else if (capable->has_mpv) {
          get_location(EXEC_MPV, loc, PATH_MAX);
        }

        if (strcmp(prefs->video_open_command, loc) && strncmp(prefs->video_open_command + 1, loc, strlen(loc))) {
          lives_strappend(msg, 256, _("\n\nPlease check the setting of Video Open Command in\nTools|Preferences|Decoding\n"));
        }
      }
      widget_opts.non_modal = TRUE;
      do_error_dialog(msg);
      widget_opts.non_modal = FALSE;
      d_print_failed();
      close_current_file(old_file);
      if (mainw->multitrack) {
        mainw->multitrack->pb_start_event = mt_pb_start_event;
        mainw->multitrack->has_audio_file = mt_has_audio_file;
      }
      lives_freep((void **)&mainw->file_open_params);
      lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
      return 0;
    }
    cfile->frames = 0;
  }

  if (!cfile->ext_src) {
    extra_frames = cfile->frames;
    add_file_info(cfile->handle, FALSE);
    extra_frames -= cfile->frames;
    cfile->end = cfile->frames;
    cfile->video_time = cfile->frames / cfile->fps;
  } else {
    add_file_info(NULL, FALSE);
    if (cfile->f_size == 0) {
      off_t fsize = sget_file_size((char *)file_name);
      if (fsize < 0) fsize = 0;
      cfile->f_size = (size_t)fsize;
    }
  }

  if (!cfile->ext_src) {
    reget_afilesize(mainw->current_file);
    if (prefs->auto_trim_audio || prefs->keep_all_audio) {
      if (cfile->laudio_time > cfile->video_time && cfile->frames > 0) {
        if (!prefs->keep_all_audio || start != 0. || extra_frames <= 0) {
          d_print(_("Auto trimming %.2f seconds of audio at end..."), cfile->laudio_time - cfile->video_time);
          if (on_trim_audio_activate(NULL, LIVES_INT_TO_POINTER(0))) d_print_done();
          else d_print("\n");
          cfile->changed = FALSE;
        } else {
          /// insert blank frames
          if (prefs->keep_all_audio && (cfile->laudio_time - cfile->video_time) * cfile->fps > extra_frames)
            extra_frames = (cfile->laudio_time - cfile->video_time) * cfile->fps;
          insert_blank_frames(mainw->current_file, extra_frames, cfile->frames, WEED_PALETTE_RGB24);
          cfile->video_time += extra_frames / cfile->fps;
          cfile->end = cfile->frames;
          showclipimgs();
          if (!mainw->multitrack)
            redraw_timeline(mainw->current_file);
        }
      }
      if (cfile->laudio_time < cfile->video_time && cfile->achans > 0) {
        cfile->undo1_dbl = cfile->laudio_time;
        cfile->undo2_dbl = CLIP_TOTAL_TIME(mainw->current_file) - cfile->laudio_time;
        cfile->undo_arate = cfile->arate;
        cfile->undo_signed_endian = cfile->signed_endian;
        cfile->undo_achans = cfile->achans;
        cfile->undo_asampsize = cfile->asampsize;
        cfile->undo_arps = cfile->arps;
        d_print(_("Auto padding with %.2f seconds of silence at end..."), cfile->undo2_dbl);
        if (on_ins_silence_activate(NULL, NULL)) d_print_done();
        else d_print("\n");
        cfile->changed = FALSE;
      }
    }
  }

  if (isubfname) {
    d_print(_("Loaded subtitle file: %s\n"), isubfname);
    lives_free(isubfname);
  }

img_load:
  current_file = mainw->current_file;

#ifdef GET_MD5
  g_print("md5sum is %s\n", get_md5sum(file_name));
#endif

  // TODO - prompt for copy to origs (unless it is already there)

  lives_notify(LIVES_OSC_NOTIFY_CLIP_OPENED, "");

  if (prefs->show_recent && !mainw->is_generating) {
    add_to_recent(file_name, start, frames, mainw->file_open_params);
  }
  lives_freep((void **)&mainw->file_open_params);

  if (!strcmp(cfile->type, "Frames") || !strcmp(cfile->type, LIVES_IMAGE_TYPE_JPEG) ||
      !strcmp(cfile->type, LIVES_IMAGE_TYPE_PNG) ||
      !strcmp(cfile->type, "Audio")) {
    cfile->is_untitled = TRUE;
  }

  if ((!strcmp(cfile->type, LIVES_IMAGE_TYPE_JPEG) || !strcmp(cfile->type, LIVES_IMAGE_TYPE_PNG))) {
    if (mainw->img_concat_clip == -1) {
      cfile->img_type = lives_image_type_to_img_type(cfile->type);
      mainw->img_concat_clip = mainw->current_file;
      add_to_clipmenu();
      set_main_title(cfile->file_name, 0);
      cfile->opening = cfile->opening_audio = cfile->opening_only_audio = FALSE;
      cfile->opening_frames = -1;
      mainw->effects_paused = FALSE;
      cfile->is_loaded = TRUE;
    } else if (prefs->concat_images) {
      // insert this image into our image clip, close this file

      com = lives_strdup_printf("%s insert \"%s\" \"%s\" %d 1 1 \"%s\" 0 %d %d %d", prefs->backend,
                                mainw->files[mainw->img_concat_clip]->handle,
                                get_image_ext_for_type(mainw->files[mainw->img_concat_clip]->img_type),
                                mainw->files[mainw->img_concat_clip]->frames,
                                cfile->handle, mainw->files[mainw->img_concat_clip]->frames,
                                mainw->files[mainw->img_concat_clip]->hsize, mainw->files[mainw->img_concat_clip]->vsize);

      mainw->current_file = mainw->img_concat_clip;

      lives_rm(cfile->info_file);

      mainw->cancelled = CANCEL_NONE;
      mainw->error = FALSE;
      lives_system(com, FALSE);
      lives_free(com);

      do_auto_dialog(_("Adding image..."), 2);

      if (current_file != mainw->img_concat_clip) {
        mainw->current_file = current_file;
        close_current_file(mainw->img_concat_clip);
      }

      if (mainw->cancelled || mainw->error) {
        goto load_done;
      }

      cfile->frames++;
      cfile->end++;

      lives_signal_handler_block(mainw->spinbutton_end, mainw->spin_end_func);
      lives_spin_button_set_range(LIVES_SPIN_BUTTON(mainw->spinbutton_end), cfile->frames == 0 ? 0 : 1, cfile->frames);
      lives_spin_button_set_value(LIVES_SPIN_BUTTON(mainw->spinbutton_end), cfile->end);
      lives_signal_handler_unblock(mainw->spinbutton_end, mainw->spin_end_func);

      lives_signal_handler_block(mainw->spinbutton_start, mainw->spin_start_func);
      lives_spin_button_set_range(LIVES_SPIN_BUTTON(mainw->spinbutton_start), cfile->frames == 0 ? 0 : 1, cfile->frames);
      lives_spin_button_set_value(LIVES_SPIN_BUTTON(mainw->spinbutton_start), cfile->start);
      lives_signal_handler_unblock(mainw->spinbutton_start, mainw->spin_start_func);
      lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
      return 0;
    }
  }

  // set new style file details
  if (!save_clip_values(current_file)) {
    close_current_file(old_file);
    return 0;
  }

  if (prefs->crash_recovery) add_to_recovery_file(cfile->handle);

load_done:
  if (!mainw->multitrack) {
    // update widgets
    switch_to_file((mainw->current_file = 0), current_file);
    lives_widget_queue_draw(LIVES_MAIN_WINDOW_WIDGET);
  } else {
    lives_mt *multi = mainw->multitrack;
    mainw->multitrack = NULL; // allow getting of afilesize
    current_file = mainw->current_file;
    mainw->current_file = -1; // stop framebars from being drawn
    reget_afilesize(current_file);
    mainw->current_file = current_file;
    mainw->multitrack = multi;
    get_total_time(cfile);
    if (!mainw->is_generating) mainw->current_file = mainw->multitrack->render_file;
    mt_init_clips(mainw->multitrack, current_file, TRUE);
    lives_widget_process_updates(LIVES_MAIN_WINDOW_WIDGET);
    mt_clip_select(mainw->multitrack, TRUE);
  }
  lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
  check_storage_space(-1, FALSE);
  return cfile->unique_id;
}


static void save_subs_to_file(lives_clip_t *sfile, char *fname) {
  char *ext;
  lives_subtitle_type_t otype, itype;

  if (!sfile->subt) return;

  itype = sfile->subt->type;

  ext = get_extension(fname);

  if (!strcmp(ext, LIVES_FILE_EXT_SUB)) otype = SUBTITLE_TYPE_SUB;
  else if (!strcmp(ext, LIVES_FILE_EXT_SRT)) otype = SUBTITLE_TYPE_SRT;
  else otype = itype;

  lives_free(ext);

  // TODO - use sfile->subt->save_fn
  switch (otype) {
  case SUBTITLE_TYPE_SUB:
    save_sub_subtitles(sfile, (double)(sfile->start - 1) / sfile->fps, (double)sfile->end / sfile->fps,
                       (double)(sfile->start - 1) / sfile->fps, fname);
    break;

  case SUBTITLE_TYPE_SRT:
    save_srt_subtitles(sfile, (double)(sfile->start - 1) / sfile->fps, (double)sfile->end / sfile->fps,
                       (double)(sfile->start - 1) / sfile->fps, fname);
    break;

  default:
    return;
  }

  d_print(_("Subtitles were saved as %s\n"), mainw->subt_save_file);
}


boolean get_handle_from_info_file(int index) {
  // called from get_new_handle to get the 'real' file handle
  // because until we know the handle we can't use the normal info file yet
  char *com = lives_strdup_printf("%s new", prefs->backend_sync);

  lives_popen(com, FALSE, mainw->msg, MAINW_MSG_SIZE);
  lives_free(com);

  if (!strncmp(mainw->msg, "error|", 6)) {
    handle_backend_errors(FALSE);
    return FALSE;
  }

  if (!mainw->files[index]) {
    mainw->files[index] = (lives_clip_t *)(lives_calloc(1, sizeof(lives_clip_t)));
    mainw->files[index]->clip_type = CLIP_TYPE_DISK; // the default
  }
  lives_snprintf(mainw->files[index]->handle, 256, "%s", mainw->msg);

  return TRUE;
}


void save_frame(LiVESMenuItem * menuitem, livespointer user_data) {
  int frame;
  // save a single frame from a clip
  char *filt[2];
  char *ttl;
  char *filename, *defname;

  filt[0] = lives_strdup_printf("*.%s", get_image_ext_for_type(cfile->img_type));
  filt[1] = NULL;

  frame = LIVES_POINTER_TO_INT(user_data);

  if (frame > 0)
    ttl = lives_strdup_printf(_("Save Frame %d"), frame);

  else
    ttl = (_("Save Frame"));

  defname = lives_strdup_printf("frame%08d.%s", frame, get_image_ext_for_type(cfile->img_type));

  filename = choose_file(*mainw->image_dir ? mainw->image_dir : NULL, defname,
                         filt, LIVES_FILE_CHOOSER_ACTION_SAVE, ttl, NULL);

  lives_free(defname); lives_free(filt[0]); lives_free(ttl);

  if (!filename) return;
  if (!*filename) {
    lives_free(filename);
    return;
  }

  if (!save_frame_inner(mainw->current_file, frame, filename, -1, -1, FALSE)) {
    lives_free(filename);
    return;
  }

  lives_snprintf(mainw->image_dir, PATH_MAX, "%s", filename);
  lives_free(filename);
  get_dirname(mainw->image_dir);
  if (prefs->save_directories) {
    set_utf8_pref(PREF_IMAGE_DIR, mainw->image_dir);
  }
}


static void save_log_file(const char *prefix) {
  int logfd;

  // save the logfile in workdir
#ifndef IS_MINGW
  char *logfile = lives_strdup_printf("%s/%s_%d_%d.txt", prefs->workdir, prefix, lives_getuid(), lives_getgid());
  if ((logfd = creat(logfile, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) != -1) {
#else
  char *logfile = lives_strdup_printf("%s\\%s_%d_%d.txt", prefs->workdir, prefix, lives_getuid(), lives_getgid());
  if ((logfd = creat(logfile, S_IRUSR | S_IWUSR)) != -1) {
#endif
    char *btext = lives_text_view_get_text(mainw->optextview);
    lives_write(logfd, btext, strlen(btext), TRUE); // not really important if it fails
    lives_free(btext);
    close(logfd);
  }
  lives_free(logfile);
}


LIVES_GLOBAL_INLINE void set_default_comment(lives_clip_t *sfile, const char *extrat) {
  if (!*sfile->comment)
    lives_snprintf(sfile->comment, 1024, "Created with LiVES version %s.\nSee: %s\n%s",
                   LiVES_VERSION, LIVES_WEBSITE, extrat);
  if (!*sfile->author && *prefs->def_author)
    lives_snprintf(sfile->author, 1024, "%s", prefs->def_author);
}


void save_file(int clip, int start, int end, const char *filename) {
  // save clip from frame start to frame end
  lives_clip_t *sfile = mainw->files[clip], *nfile = NULL;
  double aud_start = 0., aud_end = 0.;

  char *n_file_name = NULL;
  char *fps_string;
  char *extra_params = NULL;
  char *redir = lives_strdup("1>&2 2>"LIVES_DEVNULL);
  char *new_stderr_name = NULL;
  char *mesg, *bit, *tmp;
  char *com, *msg;
  char *full_file_name = NULL;
  char *enc_exec_name = NULL;
  char *clipdir;
  char *cwd;

  boolean recheck_name = FALSE;

  int new_stderr = -1;
  int retval;
  int startframe = 1;
  int current_file = mainw->current_file;
  int asigned = !(sfile->signed_endian & AFORM_UNSIGNED); // 1 is signed (in backend)
  int aendian = (sfile->signed_endian & AFORM_BIG_ENDIAN); // 2 is bigend
  int arate;
  int new_file = -1;

#ifdef GUI_GTK
  GError *gerr = NULL;
#endif

  struct stat filestat;

  off_t fsize;

  LiVESWidget *hbox;
  frames_t res;

  boolean safe_symlinks = prefs->safe_symlinks;
  boolean not_cancelled = FALSE;
  boolean output_exists = FALSE;
  boolean save_all = FALSE;
  boolean debug_mode = FALSE;

  if (!check_storage_space(mainw->current_file, FALSE)) return;

  lives_set_cursor_style(LIVES_CURSOR_BUSY, NULL);
  lives_widget_context_update();

  if (start == 1 && end == sfile->frames) save_all = TRUE;

  // new handling for save selection:
  // symlink images 1 - n to the encoded frames
  // symlinks are now created in /tmp (for dynebolic)
  // then encode the symlinked frames

  if (!filename) {
    // prompt for encoder type/output format
    if (prefs->show_rdet) {
      int response;
      rdet = create_render_details(1); // WARNING !! - rdet is global in events.h

      while (1) {
        response = lives_dialog_run(LIVES_DIALOG(rdet->dialog));
        lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
        lives_widget_hide(rdet->dialog);

        if (response == LIVES_RESPONSE_CANCEL) {
          lives_widget_destroy(rdet->dialog);
          lives_free(rdet->encoder_name);
          lives_freep((void **)&rdet);
          lives_freep((void **)&resaudw);
          return;
        }

        clear_mainw_msg();
        // initialise new plugin

        if (enc_exec_name) lives_free(enc_exec_name);
        enc_exec_name = lives_build_filename(prefs->lib_dir, PLUGIN_EXEC_DIR, PLUGIN_ENCODERS, prefs->encoder.name, NULL);

        com = lives_strdup_printf("\"%s\" init", enc_exec_name);
        lives_popen(com, TRUE, mainw->msg, MAINW_MSG_SIZE);
        lives_free(com);

        if (strcmp(mainw->msg, "initialised\n")) {
          if (*mainw->msg) {
            msg = lives_strdup_printf(_("\n\nThe '%s' plugin reports:\n%s\n"), prefs->encoder.name, mainw->msg);
          } else {
            msg = lives_strdup_printf
                  (_("\n\nUnable to find the 'init' method in the %s plugin.\n"
                     "The plugin may be broken or not installed correctly."), prefs->encoder.name);
          }
          do_error_dialog(msg);
          lives_free(msg);
        } else break;
      }
      if (rdet->debug && lives_toggle_button_get_active(LIVES_TOGGLE_BUTTON(rdet->debug)))
        debug_mode = TRUE;
    }
  }

  if (!enc_exec_name)
    enc_exec_name = lives_build_filename(prefs->lib_dir, PLUGIN_EXEC_DIR, PLUGIN_ENCODERS, prefs->encoder.name, NULL);

  // get file extension
  check_encoder_restrictions(TRUE, FALSE, save_all);

  hbox = lives_hbox_new(FALSE, 0);
  mainw->fx1_bool = TRUE;
  add_suffix_check(LIVES_BOX(hbox), prefs->encoder.of_def_ext);
  lives_widget_show_all(hbox);

  if (palette->style & STYLE_1) {
    lives_widget_set_fg_color(hbox, LIVES_WIDGET_STATE_NORMAL, &palette->normal_fore);
    lives_widget_set_bg_color(hbox, LIVES_WIDGET_STATE_NORMAL, &palette->normal_back);
  }

  if (!filename) {
    char *ttl = (_("Save Clip"));
    do {
      lives_freep((void **)&n_file_name);
      n_file_name = choose_file_bg(mainw->vid_save_dir, NULL, NULL, LIVES_FILE_CHOOSER_ACTION_SAVE, ttl, hbox);
      if (mainw->fc_buttonresponse == LIVES_RESPONSE_CANCEL) return;
    } while (!*n_file_name);
    lives_snprintf(mainw->vid_save_dir, PATH_MAX, "%s", n_file_name);
    get_dirname(mainw->vid_save_dir);
    if (prefs->save_directories) {
      set_utf8_pref(PREF_VID_SAVE_DIR, mainw->vid_save_dir);
    }
    lives_free(ttl);
  } else n_file_name = lives_strdup(filename);

  //append default extension (if necessary)
  if (!*prefs->encoder.of_def_ext) {
    // encoder adds its own extension
    get_filename(n_file_name, FALSE);
  } else {
    if (mainw->fx1_bool && (strlen(n_file_name) <= strlen(prefs->encoder.of_def_ext) ||
                            strncmp(n_file_name + strlen(n_file_name) - strlen(prefs->encoder.of_def_ext) - 1, ".", 1) ||
                            strcmp(n_file_name + strlen(n_file_name) - strlen(prefs->encoder.of_def_ext),
                                   prefs->encoder.of_def_ext))) {
      full_file_name = lives_strconcat(n_file_name, ".", prefs->encoder.of_def_ext, NULL);
      recheck_name = TRUE;
    }
  }

  if (!full_file_name) {
    full_file_name = lives_strdup(n_file_name);
  }

  if (!filename && recheck_name) {
    if (!check_file(full_file_name, strcmp(full_file_name, n_file_name))) {
      lives_free(full_file_name);
      lives_free(n_file_name);
      if (rdet) {
        lives_widget_destroy(rdet->dialog);
        lives_free(rdet->encoder_name);
        lives_freep((void **)&rdet);
        lives_freep((void **)&resaudw);
      }
      return;
    }
    sfile->orig_file_name = FALSE;
  }

  if (!*sfile->comment) set_default_comment(sfile, NULL);

  if (!do_comments_dialog(clip, full_file_name)) {
    lives_free(full_file_name);
    if (rdet) {
      lives_widget_destroy(rdet->dialog);
      lives_free(rdet->encoder_name);
      lives_freep((void **)&rdet);
      lives_freep((void **)&resaudw);
    }
    lives_freep((void **)&mainw->subt_save_file);
    return;
  }

  if (rdet) {
    lives_widget_destroy(rdet->dialog);
    lives_freep((void **)&rdet->encoder_name);
    lives_freep((void **)&rdet);
    lives_freep((void **)&resaudw);
  }

  if (sfile->arate * sfile->achans != 0) {
    aud_start = calc_time_from_frame(clip, start) * sfile->arps / sfile->arate;
    aud_end = calc_time_from_frame(clip, end + 1) * sfile->arps / sfile->arate;
  }

  // get extra params for encoder

  if (!sfile->ratio_fps) {
    fps_string = lives_strdup_printf("%.3f", sfile->fps);
  } else {
    fps_string = lives_strdup_printf("%.8f", sfile->fps);
  }

  arate = sfile->arate;

  if (!mainw->save_with_sound || prefs->encoder.of_allowed_acodecs == 0) {
    arate = 0;
  }

  /// get extra parameters for saving
  if (prefs->encoder.capabilities & HAS_RFX) {
    char buff[65536];

    com = lives_strdup_printf("\"%s\" get_rfx %s %d %d %d", enc_exec_name, prefs->encoder.of_name,
                              prefs->encoder.audio_codec, cfile->hsize, cfile->vsize);
    if (debug_mode) {
      fprintf(stderr, "Running command: %s\n", com);
    }
    lives_popen(com, TRUE, buff, 65536);
    lives_free(com);

    if (!THREADVAR(com_failed)) {
      extra_params = plugin_run_param_window(buff, NULL, NULL);
    }
    if (!extra_params) {
      lives_free(fps_string);
      if (!mainw->multitrack) {
        switch_to_file(mainw->current_file, current_file);
      }
      lives_freep((void **)&mainw->subt_save_file);
      return;
    }
  }

  if (!save_all && !safe_symlinks) {
    // we are saving a selection - make symlinks from a temporary clip

    if ((new_file = mainw->first_free_file) == ALL_USED) {
      too_many_files();
      lives_freep((void **)&mainw->subt_save_file);
      return;
    }

    // create new clip
    if (!get_new_handle(new_file, (_("selection")))) {
      lives_freep((void **)&mainw->subt_save_file);
      return;
    }

    if (sfile->clip_type == CLIP_TYPE_FILE) {
      mainw->cancelled = CANCEL_NONE;
      cfile->progress_start = 1;
      cfile->progress_end = count_virtual_frames(sfile->frame_index, start, end);
      do_threaded_dialog(_("Pulling frames from clip..."), TRUE);
      res = virtual_to_images(clip, start, end, TRUE, NULL);
      end_threaded_dialog();

      if (mainw->cancelled != CANCEL_NONE || res < 0) {
        mainw->cancelled = CANCEL_USER;
        lives_freep((void **)&mainw->subt_save_file);
        if (res <= 0) d_print_file_error_failed();
        return;
      }
    }

    mainw->effects_paused = FALSE;

    nfile = mainw->files[new_file];
    nfile->hsize = sfile->hsize;
    nfile->vsize = sfile->vsize;
    cfile->progress_start = nfile->start = 1;
    cfile->progress_end = nfile->frames = nfile->end = end - start + 1;
    nfile->fps = sfile->fps;
    nfile->arps = sfile->arps;
    nfile->arate = sfile->arate;
    nfile->achans = sfile->achans;
    nfile->asampsize = sfile->asampsize;
    nfile->signed_endian = sfile->signed_endian;
    nfile->img_type = sfile->img_type;

    com = lives_strdup_printf("%s link_frames \"%s\" %d %d %.8f %.8f %d %d %d %d %d \"%s\"", prefs->backend, nfile->handle,
                              start, end, aud_start, aud_end, nfile->arate, nfile->achans, nfile->asampsize,
                              !(nfile->signed_endian & AFORM_UNSIGNED), !(nfile->signed_endian & AFORM_BIG_ENDIAN),
                              sfile->handle);

    lives_rm(nfile->info_file);
    lives_system(com, FALSE);
    lives_free(com);

    // TODO - eliminate this
    mainw->current_file = new_file;

    if (THREADVAR(com_failed)) {
      char *permitname = lives_build_filename(prefs->workdir, cfile->handle, TEMPFILE_MARKER "." LIVES_FILE_EXT_TMP, NULL);
#ifdef IS_MINGW
      // kill any active processes: for other OSes the backend does this
      lives_kill_subprocesses(cfile->handle, TRUE);
#endif
      lives_touch(permitname);
      lives_free(permitname);
      lives_system(lives_strdup_printf("%s close \"%s\"", prefs->backend, cfile->handle), TRUE);
      lives_freep((void **)&cfile);
      if (mainw->first_free_file == ALL_USED || mainw->first_free_file > new_file)
        mainw->first_free_file = new_file;
      if (!mainw->multitrack) {
        switch_to_file(mainw->current_file, current_file);
      }
      d_print_cancelled();
      lives_freep((void **)&mainw->subt_save_file);
      return;
    }

    cfile->nopreview = TRUE;
    if (!(do_progress_dialog(TRUE, TRUE, _("Linking selection")))) {
      char *permitname = lives_build_filename(prefs->workdir, cfile->handle, TEMPFILE_MARKER "." LIVES_FILE_EXT_TMP, NULL);
#ifdef IS_MINGW
      // kill any active processes: for other OSes the backend does this
      lives_kill_subprocesses(cfile->handle, TRUE);
#endif
      lives_touch(permitname);
      lives_free(permitname);
      lives_system((tmp = lives_strdup_printf("%s close \"%s\"", prefs->backend, cfile->handle)), TRUE);
      lives_free(tmp);
      lives_freep((void **)&cfile);
      if (mainw->first_free_file == ALL_USED || mainw->first_free_file > new_file)
        mainw->first_free_file = new_file;

      if (!mainw->multitrack) {
        switch_to_file(mainw->current_file, current_file);
      }
      if (mainw->error) d_print_failed();
      else d_print_cancelled();
      lives_freep((void **)&mainw->subt_save_file);
      return;
    }

    // cfile->arate, etc., would have been reset by calls to do_progress_dialog() which calls get_total_time() [since cfile->afilesize==0]
    // so we need to set these again now that link_frames has provided an actual audio clip

    nfile->arps = sfile->arps;
    nfile->arate = sfile->arate;
    nfile->achans = sfile->achans;
    nfile->asampsize = sfile->asampsize;
    nfile->signed_endian = sfile->signed_endian;

    reget_afilesize(new_file);

    aud_start = calc_time_from_frame(new_file, 1) * nfile->arps / nfile->arate;
    aud_end = calc_time_from_frame(new_file, nfile->frames + 1) * nfile->arps / nfile->arate;
    cfile->nopreview = FALSE;
  } else mainw->current_file = clip; // for encoder restns

  if (rdet) rdet->is_encoding = TRUE;

  if (!check_encoder_restrictions(FALSE, FALSE, save_all)) {
    if (!save_all && !safe_symlinks) {
      char *permitname = lives_build_filename(prefs->workdir, nfile->handle, TEMPFILE_MARKER "." LIVES_FILE_EXT_TMP, NULL);
#ifdef IS_MINGW
      lives_kill_subprocesses(nfile->handle, TRUE);
#endif
      lives_touch(permitname);
      lives_free(permitname);
      lives_system((com = lives_strdup_printf("%s close \"%s\"", prefs->backend, nfile->handle)), TRUE);
      lives_free(com);
      lives_free(nfile);
      mainw->files[new_file] = NULL;
      if (mainw->first_free_file == ALL_USED || new_file) mainw->first_free_file = new_file;
    }
    if (!mainw->multitrack) {
      switch_to_file(mainw->current_file, current_file);
    }
    d_print_cancelled();
    lives_freep((void **)&mainw->subt_save_file);
    return;
  }

  if (!save_all && safe_symlinks) {
    int xarps, xarate, xachans, xasamps, xasigned_endian;
    // we are saving a selection - make symlinks in /tmp

    startframe = -1;

    if (sfile->clip_type == CLIP_TYPE_FILE) {
      mainw->cancelled = CANCEL_NONE;
      cfile->progress_start = 1;
      cfile->progress_end = count_virtual_frames(sfile->frame_index, start, end);
      do_threaded_dialog(_("Pulling frames from clip..."), TRUE);
      res = virtual_to_images(clip, start, end, TRUE, NULL);
      end_threaded_dialog();

      if (mainw->cancelled != CANCEL_NONE || res <= 0) {
        if (mainw->cancelled != CANCEL_NONE) mainw->cancelled = CANCEL_USER;
        lives_freep((void **)&mainw->subt_save_file);
        if (res <= 0) d_print_file_error_failed();
        return;
      }
    }

    com = lives_strdup_printf("%s link_frames \"%s\" %d %d %.8f %.8f %d %d %d %d %d", prefs->backend, sfile->handle,
                              start, end, aud_start, aud_end, sfile->arate, sfile->achans, sfile->asampsize,
                              !(sfile->signed_endian & AFORM_UNSIGNED), !(sfile->signed_endian & AFORM_BIG_ENDIAN));

    lives_rm(sfile->info_file);
    lives_system(com, FALSE);
    lives_free(com);

    mainw->current_file = clip;

    xarps = sfile->arps;
    xarate = sfile->arate;
    xachans = sfile->achans;
    xasamps = sfile->asampsize;
    xasigned_endian = sfile->signed_endian;

    if (THREADVAR(com_failed)) {
      com = lives_strdup_printf("%s clear_symlinks \"%s\"", prefs->backend_sync, cfile->handle);
      lives_system(com, TRUE);
      lives_free(com);
      cfile->nopreview = FALSE;
      if (!mainw->multitrack) {
        switch_to_file(mainw->current_file, current_file);
      }
      d_print_cancelled();
      lives_freep((void **)&mainw->subt_save_file);
      return;
    }

    cfile->nopreview = TRUE;
    if (!(do_progress_dialog(TRUE, TRUE, _("Linking selection")))) {
      com = lives_strdup_printf("%s clear_symlinks \"%s\"", prefs->backend_sync, cfile->handle);
      lives_system(com, TRUE);
      lives_free(com);
      cfile->nopreview = FALSE;
      if (!mainw->multitrack) {
        switch_to_file(mainw->current_file, current_file);
      }
      if (mainw->error) d_print_failed();
      else d_print_cancelled();
      lives_freep((void **)&mainw->subt_save_file);
      return;
    }

    // cfile->arate, etc., would have been reset by calls to do_progress_dialog() which calls get_total_time() [since cfile->afilesize==0]
    // so we need to set these again now that link_frames has provided an actual audio clip

    sfile->arps = xarps;
    sfile->arate = xarate;
    sfile->achans = xachans;
    sfile->asampsize = xasamps;
    sfile->signed_endian = xasigned_endian;

    reget_afilesize(clip);

    aud_start = calc_time_from_frame(clip, 1) * sfile->arps / sfile->arate;
    aud_end = calc_time_from_frame(clip, end - start + 1) * sfile->arps / sfile->arate;
    cfile->nopreview = FALSE;
  }

  if (save_all) {
    if (sfile->clip_type == CLIP_TYPE_FILE) {
      frames_t ret;
      char *msg = (_("Pulling frames from clip..."));
      if ((ret = realize_all_frames(clip, msg, FALSE)) < sfile->frames) {
        lives_free(msg);
        lives_freep((void **)&mainw->subt_save_file);
        if (ret > 0) d_print_cancelled();
        if (!mainw->multitrack) {
          switch_to_file(mainw->current_file, current_file);
        }
        return;
      }
      lives_free(msg);
    }
  }

  if (!mainw->save_with_sound || prefs->encoder.of_allowed_acodecs == 0) {
    bit = (_(" (with no sound)\n"));
  } else {
    bit = lives_strdup("\n");
  }

  if (!save_all) {
    mesg = lives_strdup_printf(_("Saving frames %d to %d%s as \"%s\" : encoder = %s : format = %s..."),
                               start, end, bit, full_file_name, prefs->encoder.name, prefs->encoder.of_desc);
  } // end selection
  else {
    mesg = lives_strdup_printf(_("Saving frames 1 to %d%s as \"%s\" : encoder %s : format = %s..."),
                               sfile->frames, bit, full_file_name, prefs->encoder.name, prefs->encoder.of_desc);
  }
  lives_free(bit);

  mainw->no_switch_dprint = TRUE;
  d_print(mesg);
  mainw->no_switch_dprint = FALSE;
  lives_free(mesg);

  if (prefs->show_gui && !debug_mode) {
    // open a file for stderr
    new_stderr_name = lives_build_filename(prefs->workdir, cfile->handle, LIVES_ENC_DEBUG_FILE_NAME, NULL);
    lives_free(redir);

    do {
      retval = 0;
      new_stderr = lives_open3(new_stderr_name, O_CREAT | O_RDONLY | O_TRUNC | O_SYNC, S_IRUSR | S_IWUSR);
      if (new_stderr < 0) {
        retval = do_write_failed_error_s_with_retry(new_stderr_name, lives_strerror(errno));
        if (retval == LIVES_RESPONSE_CANCEL) redir = lives_strdup("1>&2");
      } else {

#ifdef IS_MINGW

#ifdef GUI_GTK
        mainw->iochan = g_io_channel_win32_new_fd(new_stderr);
#endif
        redir = lives_strdup_printf("2>&1 >\"%s\"", new_stderr_name);
#else
#ifdef GUI_GTK
        mainw->iochan = g_io_channel_unix_new(new_stderr);
#endif
        redir = lives_strdup_printf("2>\"%s\"", new_stderr_name);
#endif

#ifdef GUI_QT
        mainw->iochan = new QFile;
        mainw->iochan->open(new_stderr, QIODevice::ReadOnly);
#endif

#ifdef GUI_GTK
        g_io_channel_set_encoding(mainw->iochan, NULL, NULL);
        g_io_channel_set_buffer_size(mainw->iochan, 0);
        g_io_channel_set_flags(mainw->iochan, G_IO_FLAG_NONBLOCK, &gerr);
        if (gerr) lives_error_free(gerr);
        gerr = NULL;
#endif
        mainw->optextview = create_output_textview();
      }
    } while (retval == LIVES_RESPONSE_RETRY);
  } else {
    lives_free(redir);
    redir = lives_strdup("1>&2");
  }

  if (lives_file_test((tmp = lives_filename_from_utf8(full_file_name, -1, NULL, NULL, NULL)), LIVES_FILE_TEST_EXISTS)) {
    lives_rm(tmp);
  }
  lives_free(tmp);

  /// re-read values in case they were resampled

  if (arate != 0) arate = cfile->arate;

  if (!cfile->ratio_fps) {
    fps_string = lives_strdup_printf("%.3f", cfile->fps);
  } else {
    fps_string = lives_strdup_printf("%.8f", cfile->fps);
  }

  // if startframe is -ve, we will use the links created for safe_symlinks - in /tmp
  // for non-safe symlinks, cfile will be our new links file
  // for save_all, cfile will be sfile

  if (prefs->encoder.capabilities & ENCODER_NON_NATIVE) {
    com = lives_strdup_printf("%s save \"%s\" \"%s\" \"%s\" \"%s\" %d %d %d %d %d %d %.4f %.4f %s %s", prefs->backend,
                              cfile->handle,
                              enc_exec_name, fps_string, (tmp = lives_filename_from_utf8(full_file_name, -1, NULL, NULL, NULL)),
                              startframe, cfile->frames, arate, cfile->achans, cfile->asampsize,
                              asigned | aendian, aud_start, aud_end, (extra_params == NULL) ? "" : extra_params, redir);
  } else {
    com = lives_strdup_printf("%s save \"%s\" \"native:%s\" \"%s\" \"%s\" %d %d %d %d %d %d %.4f %.4f %s %s", prefs->backend,
                              cfile->handle,
                              enc_exec_name, fps_string, (tmp = lives_filename_from_utf8(full_file_name, -1, NULL, NULL, NULL)),
                              startframe, cfile->frames, arate, cfile->achans, cfile->asampsize,
                              asigned | aendian, aud_start, aud_end, (extra_params == NULL) ? "" : extra_params, redir);

  }
  lives_free(tmp);
  lives_free(fps_string);

  lives_freep((void **)&extra_params);

  mainw->effects_paused = FALSE;
  cfile->nokeep = TRUE;

  lives_rm(cfile->info_file);
  THREADVAR(write_failed) = FALSE;
  save_file_comments(current_file);

  if (debug_mode) {
    fprintf(stderr, "Running command: %s\n", com);
  }

  lives_system(com, FALSE);
  lives_free(com);
  mainw->error = FALSE;

  if (THREADVAR(com_failed) || THREADVAR(write_failed)) {
    mainw->error = TRUE;
  }

  if (!mainw->error) {
    //char *pluginstr;

    cfile->progress_start = 1;
    cfile->progress_end = cfile->frames;

    not_cancelled = do_progress_dialog(TRUE, TRUE, _("Saving [can take a long time]"));

    if (mainw->iochan) {
      /// flush last of stdout/stderr from plugin

      lives_fsync(new_stderr);
      pump_io_chan(mainw->iochan);

#ifdef GUI_GTK
      g_io_channel_shutdown(mainw->iochan, FALSE, &gerr);
      g_io_channel_unref(mainw->iochan);
      if (gerr) lives_error_free(gerr);
#endif
#ifdef GUI_QT
      delete mainw->iochan;
#endif
      mainw->iochan = NULL;

      close(new_stderr);
      lives_rm(new_stderr_name);
      lives_free(new_stderr_name);
      lives_free(redir);
    }

    mainw->effects_paused = FALSE;
    cfile->nokeep = FALSE;
  } else {
    if (mainw->iochan) {
      /// flush last of stdout/stderr from plugin

      lives_fsync(new_stderr);
      pump_io_chan(mainw->iochan);

#ifdef GUI_GTK
      g_io_channel_shutdown(mainw->iochan, FALSE, &gerr);
      g_io_channel_unref(mainw->iochan);
      if (gerr) lives_error_free(gerr);
#endif
#ifdef GUI_QT
      delete mainw->iochan;
#endif
      mainw->iochan = NULL;
      close(new_stderr);
      lives_rm(new_stderr_name);
      lives_free(new_stderr_name);
      lives_free(redir);
    }
  }

  cwd = lives_get_current_dir();

  clipdir = lives_build_path(prefs->workdir, cfile->handle, NULL);
  lives_chdir(clipdir, FALSE);
  lives_free(clipdir);

  com = lives_strdup_printf("\"%s\" clear", enc_exec_name);

  if (debug_mode) {
    fprintf(stderr, "Running command: %s\n", com);
  }
  lives_system(com, FALSE);
  lives_free(com);

  lives_chdir(cwd, FALSE);
  lives_free(cwd);

  lives_free(enc_exec_name);

  if (not_cancelled || mainw->error) {
    if (mainw->error) {
      mainw->no_switch_dprint = TRUE;
      d_print_failed();
      mainw->no_switch_dprint = FALSE;
      lives_free(full_file_name);
      if (!save_all && !safe_symlinks) {
        char *permitname = lives_build_filename(prefs->workdir, cfile->handle, TEMPFILE_MARKER "." LIVES_FILE_EXT_TMP, NULL);
        lives_kill_subprocesses(cfile->handle, TRUE);
        lives_touch(permitname);
        lives_free(permitname);
        lives_system((com = lives_strdup_printf("%s close \"%s\"", prefs->backend, cfile->handle)), TRUE);
        lives_free(com);
        lives_freep((void **)&cfile);
        if (mainw->first_free_file == ALL_USED || mainw->first_free_file > mainw->current_file)
          mainw->first_free_file = mainw->current_file;
      } else if (!save_all && safe_symlinks) {
        com = lives_strdup_printf("%s clear_symlinks \"%s\"", prefs->backend_sync, cfile->handle);
        lives_system(com, TRUE);
        lives_free(com);
      }

      switch_to_file(mainw->current_file, current_file);

      lives_freep((void **)&mainw->subt_save_file);
      sensitize();
      return;
    }

    if (lives_file_test((tmp = lives_filename_from_utf8(full_file_name, -1, NULL, NULL, NULL)), LIVES_FILE_TEST_EXISTS)) {
      lives_free(tmp);
      stat((tmp = lives_filename_from_utf8(full_file_name, -1, NULL, NULL, NULL)), &filestat);
      if (filestat.st_size > 0) output_exists = TRUE;
    }
    if (!output_exists) {
      lives_free(tmp);

      mainw->no_switch_dprint = TRUE;
      d_print_failed();
      mainw->no_switch_dprint = FALSE;
      lives_free(full_file_name);
      if (!save_all && !safe_symlinks) {
        char *permitname = lives_build_filename(prefs->workdir, cfile->handle, TEMPFILE_MARKER "." LIVES_FILE_EXT_TMP, NULL);
        lives_kill_subprocesses(cfile->handle, TRUE);
        lives_touch(permitname);
        lives_free(permitname);
        lives_system((com = lives_strdup_printf("%s close \"%s\"", prefs->backend, cfile->handle)), TRUE);
        lives_free(com);
        lives_freep((void **)&cfile);
        if (mainw->first_free_file == ALL_USED || mainw->first_free_file > mainw->current_file)
          mainw->first_free_file = mainw->current_file;
      } else if (!save_all && safe_symlinks) {
        com = lives_strdup_printf("%s clear_symlinks \"%s\"", prefs->backend_sync, cfile->handle);
        lives_system(com, TRUE);
        lives_free(com);
      }

      if (!mainw->multitrack) {
        switch_to_file(mainw->current_file, current_file);
      }
      retval = do_error_dialog(_("\n\nEncoder error - output file was not created !\n"));

      if (retval == LIVES_RESPONSE_SHOW_DETAILS) {
        /// show iochan (encoder) details
        on_details_button_clicked();
      }

      if (mainw->iochan) {
        save_log_file("failed_encoder_log");
        mainw->iochan = NULL;
        lives_widget_object_unref(mainw->optextview);
      }

      lives_freep((void **)&mainw->subt_save_file);
      sensitize();
      if (mainw->error) d_print_failed();

      return;
    }
    lives_free(tmp);

    if (save_all) {
      if (prefs->enc_letterbox) {
        /// replace letterboxed frames with maxspect frames
        int iwidth = sfile->ohsize;
        int iheight = sfile->ovsize;
        boolean bad_header = FALSE;

        com = lives_strdup_printf("%s mv_mgk \"%s\" %d %d \"%s\" 1", prefs->backend, sfile->handle, 1, sfile->frames,
                                  get_image_ext_for_type(sfile->img_type));

        lives_rm(sfile->info_file);
        lives_system(com, FALSE);

        do_progress_dialog(TRUE, FALSE, _("Clearing letterbox"));

        if (mainw->error) {
          //	  cfile->may_be_damaged=TRUE;
          d_print_failed();
          return;
        }

        calc_maxspect(sfile->hsize, sfile->vsize, &iwidth, &iheight);

        sfile->hsize = iwidth;
        sfile->vsize = iheight;

        save_clip_value(clip, CLIP_DETAILS_WIDTH, &sfile->hsize);
        if (THREADVAR(com_failed) || THREADVAR(write_failed)) bad_header = TRUE;
        save_clip_value(clip, CLIP_DETAILS_HEIGHT, &sfile->vsize);
        if (THREADVAR(com_failed) || THREADVAR(write_failed)) bad_header = TRUE;
        if (bad_header) do_header_write_error(mainw->current_file);
      }

      lives_snprintf(sfile->save_file_name, PATH_MAX, "%s", full_file_name);
      sfile->changed = FALSE;

      /// save was successful
      /// TODO - check for size < 0 !!!
      fsize = sget_file_size(full_file_name);
      if (fsize < 0) fsize = 0;
      cfile->f_size = (size_t)fsize;

      if (sfile->is_untitled) {
        sfile->is_untitled = FALSE;
      }
      if (!sfile->was_renamed) {
        lives_menu_item_set_text(sfile->menuentry, full_file_name, FALSE);
        lives_snprintf(sfile->name, CLIP_NAME_MAXLEN, "%s", full_file_name);
      }
      set_main_title(cfile->name, 0);
      if (prefs->show_recent) {
        add_to_recent(full_file_name, 0., 0, NULL);
        global_recent_manager_add(full_file_name);
      }
    } else {
      if (!safe_symlinks) {
        char *permitname = lives_build_filename(prefs->workdir, nfile->handle, TEMPFILE_MARKER "." LIVES_FILE_EXT_TMP, NULL);
#ifdef IS_MINGW
        lives_kill_subprocesses(nfile->handle, TRUE);
#endif
        lives_touch(permitname);
        lives_free(permitname);
        lives_system((com = lives_strdup_printf("%s close \"%s\"", prefs->backend, nfile->handle)), TRUE);
        lives_free(com);
        lives_free(nfile);
        mainw->files[new_file] = NULL;
        if (mainw->first_free_file == ALL_USED || mainw->first_free_file > mainw->current_file)
          mainw->first_free_file = new_file;
      } else {
        com = lives_strdup_printf("%s clear_symlinks \"%s\"", prefs->backend_sync, cfile->handle);
        lives_system(com, TRUE);
        lives_free(com);
      }
    }
  }

  if (!mainw->multitrack) {
    switch_to_file(mainw->current_file, current_file);
  }
  if (mainw->iochan) {
    save_log_file("encoder_log");
    lives_widget_object_unref(mainw->optextview);
    mainw->iochan = NULL;
  }

  if (not_cancelled) {
    char *fsize_ds;
    mainw->no_switch_dprint = TRUE;
    d_print_done();

    /// get size of file and show it

    fsize = sget_file_size(full_file_name);
    if (fsize >= 0) {
      /// TODO - handle file errors !!!!!

      fsize_ds = lives_format_storage_space_string(fsize);
      d_print(_("File size was %s\n"), fsize_ds);
      lives_free(fsize_ds);

      if (mainw->subt_save_file) {
        save_subs_to_file(sfile, mainw->subt_save_file);
        lives_freep((void **)&mainw->subt_save_file);
      }
    }
    mainw->no_switch_dprint = FALSE;

    lives_notify(LIVES_OSC_NOTIFY_SUCCESS,
                 (mesg = lives_strdup_printf("encode %d \"%s\"", clip,
                         (tmp = lives_filename_from_utf8(full_file_name, -1, NULL, NULL, NULL)))));
    lives_free(tmp);
    lives_free(mesg);
  } else {
    lives_rm((tmp = lives_filename_from_utf8(full_file_name, -1, NULL, NULL, NULL)));
    lives_free(tmp);
  }

  lives_free(full_file_name);
}


char *prep_audio_player(char *com2, char *com3, frames_t audio_end, int arate, int asigned, int aendian) {
  char *stfile = NULL;
  char *stopcom = NULL, *com;
  short audio_player = prefs->audio_player;
  int loop = 0;

  if (cfile->achans > 0) {
    cfile->aseek_pos = (off64_t)(cfile->real_pointer_time * (double)cfile->arate) * cfile->achans * (cfile->asampsize / 8);
    if (mainw->playing_sel) {
      off64_t apos = (off64_t)((double)(mainw->play_start - 1.) / cfile->fps * (double)cfile->arate) * cfile->achans *
                     (cfile->asampsize / 8);
      if (apos > cfile->aseek_pos) cfile->aseek_pos = apos;
    }
    if (cfile->aseek_pos > cfile->afilesize) cfile->aseek_pos = 0.;
    if (mainw->current_file == 0 && cfile->arate < 0) cfile->aseek_pos = cfile->afilesize;
  }
  // start up our audio player (jack or pulse)
  if (audio_player == AUD_PLAYER_JACK) {
#ifdef ENABLE_JACK
    if (mainw->jackd) jack_aud_pb_ready(mainw->current_file);
    return NULL;
#endif
  } else if (audio_player == AUD_PLAYER_PULSE) {
#ifdef HAVE_PULSE_AUDIO
    if (mainw->pulsed) pulse_aud_pb_ready(mainw->current_file);
    return NULL;
#endif
  } else if (audio_player != AUD_PLAYER_NONE && cfile->achans > 0) {
    // sox or mplayer audio - run as background process
    if (com3) {
      if (mainw->loop_cont) {
        // tell audio to loop forever
        loop = -1;
      }

      stfile = lives_build_filename(prefs->workdir, cfile->handle, ".stoploop", NULL);
      lives_rm(stfile);

      if (cfile->achans > 0 || (!cfile->is_loaded && !mainw->is_generating)) {
        if (loop) {
          lives_free(com3);
          com3 = lives_strdup_printf("%s \"%s\" 2>\"%s\" 1>&2", capable->touch_cmd, stfile, prefs->cmd_log);
        }

        if (com2) {
          if (cfile->achans > 0) {
            com2 = lives_strdup_printf("%s stop_audio %s", prefs->backend_sync, cfile->handle);
          }
          stopcom = lives_strconcat(com3, com2, NULL);
        }
      }
    }

    lives_freep((void **)&stfile);

    stfile = lives_build_filename(prefs->workdir, cfile->handle, LIVES_STATUS_FILE_NAME".play", NULL);

    lives_snprintf(cfile->info_file, PATH_MAX, "%s", stfile);
    lives_free(stfile);
    if (cfile->clip_type == CLIP_TYPE_DISK) lives_rm(cfile->info_file);

    // PLAY

    if (cfile->clip_type == CLIP_TYPE_DISK && cfile->opening) {
      com = lives_strdup_printf("%s play_opening_preview \"%s\" %.3f %d %d %d %d %d %d %d %d", prefs->backend,
                                cfile->handle, cfile->fps, mainw->audio_start, audio_end, 0,
                                arate, cfile->achans, cfile->asampsize, asigned, aendian);
    } else {
      // this is only used now for sox or mplayer audio player
      com = lives_strdup_printf("%s play %s %.3f %d %d %d %d %d %d %d %d", prefs->backend, cfile->handle,
                                cfile->fps, mainw->audio_start, audio_end, loop,
                                arate, cfile->achans, cfile->asampsize, asigned, aendian);
    }
    if (!mainw->multitrack && com) lives_system(com, FALSE);
  }
  return stopcom;
}


/// play the current clip from 'mainw->play_start' to 'mainw->play_end'
void play_file(void) {
  LiVESWidgetClosure *freeze_closure, *bg_freeze_closure;
  LiVESList *cliplist;
  weed_plant_t *pb_start_event = NULL;

#ifdef GDK_WINDOWING_X11
  uint64_t awinid = -1;
#endif

  char *com, *com2 = lives_strdup(" "), *com3 = lives_strdup(" ");
  char *stopcom = NULL;
  char *stfile;
#ifdef GDK_WINDOWING_X11
  char *tmp;
#endif

  double fps_med = 0.;
  double pointer_time = cfile->pointer_time;
  double real_pointer_time = cfile->real_pointer_time;

  short audio_player = prefs->audio_player;

  boolean mute;
  boolean needsadone = FALSE;

#ifdef RT_AUDIO
  boolean exact_preview = FALSE;
#endif
  boolean has_audio_buffers = FALSE;

  int arate;

  int asigned = !(cfile->signed_endian & AFORM_UNSIGNED);
  int aendian = !(cfile->signed_endian & AFORM_BIG_ENDIAN);
  int current_file = mainw->current_file;
  int audio_end = 0;

  /// from now on we can only switch at the designated SWITCH POINT
  mainw->noswitch = TRUE;
  mainw->cancelled = CANCEL_NONE;

  asigned = !(cfile->signed_endian & AFORM_UNSIGNED);
  aendian = !(cfile->signed_endian & AFORM_BIG_ENDIAN);
  current_file = mainw->current_file;
  if (mainw->pre_play_file == -1) mainw->pre_play_file = current_file;

  if (!is_realtime_aplayer(audio_player)) mainw->aud_file_to_kill = mainw->current_file;
  else mainw->aud_file_to_kill = -1;

#ifdef ENABLE_JACK_TRANSPORT
  if (!mainw->preview && !mainw->foreign) {
    if (!mainw->multitrack)
      jack_pb_start(cfile->achans > 0 ? cfile->real_pointer_time : cfile->pointer_time);
    else
      jack_pb_start(mainw->multitrack->pb_start_time);
  }
#endif

  mainw->ext_playback = FALSE;

  mainw->rec_aclip = -1;

  init_conversions(LIVES_INTENTION_PLAY);

  if (mainw->pre_src_file == -2) mainw->pre_src_file = mainw->current_file;
  mainw->pre_src_audio_file = mainw->current_file;

  /// enable the freeze button
  lives_accel_group_connect(LIVES_ACCEL_GROUP(mainw->accel_group), LIVES_KEY_BackSpace,
                            (LiVESXModifierType)LIVES_CONTROL_MASK,
                            (LiVESAccelFlags)0, (freeze_closure = lives_cclosure_new(LIVES_GUI_CALLBACK(freeze_callback),
                                LIVES_INT_TO_POINTER(SCREEN_AREA_FOREGROUND), NULL)));
  lives_accel_group_connect(LIVES_ACCEL_GROUP(mainw->accel_group), LIVES_KEY_BackSpace,
                            (LiVESXModifierType)(LIVES_CONTROL_MASK | LIVES_ALT_MASK),
                            (LiVESAccelFlags)0, (bg_freeze_closure = lives_cclosure_new(LIVES_GUI_CALLBACK(freeze_callback),
                                LIVES_INT_TO_POINTER(SCREEN_AREA_BACKGROUND), NULL)));

  /// disable ctrl-q since it can be activated by user error
  lives_accel_path_disconnect(mainw->accel_group, LIVES_ACCEL_PATH_QUIT);

  if (mainw->multitrack) {
    mainw->event_list = mainw->multitrack->event_list;
    pb_start_event = mainw->multitrack->pb_start_event;
#ifdef RT_AUDIO
    exact_preview = mainw->multitrack->exact_preview;
#endif
  }

  // reinit all active effects
  if (!mainw->preview && !mainw->is_rendering && !mainw->foreign) weed_reinit_all();

  if (mainw->record) {
    if (mainw->preview) {
      mainw->record = FALSE;
      d_print(_("recording aborted by preview.\n"));
    } else if (mainw->current_file == 0) {
      mainw->record = FALSE;
      d_print(_("recording aborted by clipboard playback.\n"));
    } else {
      d_print(_("Recording performance..."));
      needsadone = TRUE;
      mainw->clip_switched = FALSE;
      // TODO
      if (mainw->current_file > 0 && (cfile->undo_action == UNDO_RESAMPLE || cfile->undo_action == UNDO_RENDER)) {
        lives_widget_set_sensitive(mainw->undo, FALSE);
        lives_widget_set_sensitive(mainw->redo, FALSE);
        cfile->undoable = cfile->redoable = FALSE;
      }
    }
  }
  /// set performance at right place
  else if (mainw->event_list) cfile->next_event = get_first_event(mainw->event_list);

  if (!mainw->multitrack && CURRENT_CLIP_HAS_VIDEO) {
    lives_widget_set_frozen(mainw->spinbutton_start, TRUE);
    lives_widget_set_frozen(mainw->spinbutton_end, TRUE);
    //lives_signal_handler_block(mainw->spinbutton_start, mainw->spin_start_func);
    //lives_signal_handler_block(mainw->spinbutton_end, mainw->spin_end_func);
  }

#ifdef ENABLE_JACK_TRANSPORT
  if (mainw->jack_can_stop && !mainw->event_list && !mainw->preview
      && (prefs->jack_opts & (JACK_OPTS_TIMEBASE_START | JACK_OPTS_TIMEBASE_CLIENT))) {
    // calculate the start position from jack transport
    double sttime = (double)jack_transport_get_current_ticks() / TICKS_PER_SECOND_DBL;
    cfile->pointer_time = cfile->real_pointer_time = sttime;
    if (cfile->real_pointer_time > CLIP_TOTAL_TIME(mainw->current_file))
      cfile->real_pointer_time = CLIP_TOTAL_TIME(mainw->current_file);
    if (cfile->pointer_time > cfile->video_time) cfile->pointer_time = 0.;
    mainw->play_start = calc_frame_from_time(mainw->current_file, cfile->pointer_time);
  }
#endif

  /// these values are only relevant for non-realtime audio players (e.g. sox)
  mainw->audio_start = mainw->audio_end = 0;

  if (cfile->achans > 0) {
    if (mainw->event_list &&
        !(mainw->preview && mainw->is_rendering) &&
        !(mainw->multitrack && mainw->preview && mainw->multitrack->is_rendering)) {
      /// play performance data
      if (event_list_get_end_secs(mainw->event_list) > cfile->frames / cfile->fps && !mainw->playing_sel) {
        mainw->audio_end = (event_list_get_end_secs(mainw->event_list) * cfile->fps + 1.) * cfile->arate / cfile->arps;
      }
    }

    if (mainw->audio_end == 0) {
      mainw->audio_start = calc_time_from_frame(mainw->current_file,
                           mainw->play_start) * cfile->fps + 1. * cfile->arate / cfile->arps;
      mainw->audio_end = calc_time_from_frame(mainw->current_file, mainw->play_end) * cfile->fps
                         + 1. * cfile->arate / cfile->arps;
      if (!mainw->playing_sel) {
        mainw->audio_end = 0;
      }
    }
  }

  if (!cfile->opening_audio && !mainw->loop) {
    /** if we are opening audio or looping we just play to the end of audio,
      otherwise...*/
    audio_end = mainw->audio_end;
  }

  if (!mainw->multitrack) {
    if (!mainw->preview) {
      lives_frame_set_label(LIVES_FRAME(mainw->playframe), _("Play"));
    } else {
      lives_frame_set_label(LIVES_FRAME(mainw->playframe), _("Preview"));
    }

    if (palette->style & STYLE_1) {
      lives_widget_set_fg_color(lives_frame_get_label_widget(LIVES_FRAME(mainw->playframe)),
                                LIVES_WIDGET_STATE_NORMAL, &palette->normal_fore);
    }

    if (mainw->foreign) {
      lives_widget_show_all(mainw->top_vbox);
      lives_widget_process_updates(LIVES_MAIN_WINDOW_WIDGET);
    }

    /// blank the background if asked to
    if ((mainw->faded || (prefs->show_playwin && !prefs->show_gui)
         || (mainw->fs && (!mainw->sep_win))) && (cfile->frames > 0 ||
             mainw->foreign)) {
      fade_background();
    }

    if ((!mainw->sep_win || (!mainw->faded && (prefs->sepwin_type != SEPWIN_TYPE_STICKY)))
        && (cfile->frames > 0 ||
            mainw->foreign)) {
      /// show the frame in the main window
      lives_widget_set_opacity(mainw->playframe, 1.);
      lives_widget_show_all(mainw->playframe);
    }

    /// plug the plug into the playframe socket if we need to
    add_to_playframe();
  }

  arate = cfile->arate;

  mute = mainw->mute;

  if (!is_realtime_aplayer(audio_player)) {
    if (cfile->achans == 0 || mainw->is_rendering) mainw->mute = TRUE;
    if (mainw->mute && !cfile->opening_only_audio) arate = arate ? -arate : -1;
  }

  cfile->frameno = mainw->play_start;
  cfile->pb_fps = cfile->fps;
  if (mainw->reverse_pb) {
    cfile->pb_fps = -cfile->pb_fps;
    cfile->frameno = mainw->play_end;
  }
  cfile->last_frameno = cfile->frameno;
  mainw->reverse_pb = FALSE;

  mainw->swapped_clip = -1;
  mainw->blend_palette = WEED_PALETTE_END;

  cfile->play_paused = FALSE;
  mainw->period = TICKS_PER_SECOND_DBL / cfile->pb_fps;

  if (audio_player == AUD_PLAYER_JACK
      || (mainw->event_list && (!mainw->is_rendering || !mainw->preview || mainw->preview_rendering)))
    audio_cache_init();

  if (mainw->blend_file != -1 && !IS_VALID_CLIP(mainw->blend_file)) mainw->blend_file = -1;

  lives_widget_set_sensitive(mainw->m_stopbutton, TRUE);
  mainw->playing_file = mainw->current_file;

  if (!mainw->preview || !cfile->opening) {
    enable_record();
    desensitize();
    lives_widget_set_sensitive(mainw->spinbutton_pb_fps, TRUE);
  }

  if (mainw->record) {
    if (mainw->event_list) event_list_free(mainw->event_list);
    mainw->record_starting = TRUE;
  }

  if (prefs->show_msg_area && mainw->double_size && !mainw->multitrack) {
    lives_widget_hide(mainw->message_box);
  }

  lives_widget_set_sensitive(mainw->stop, TRUE);

  if (!mainw->multitrack) lives_widget_set_sensitive(mainw->m_playbutton, FALSE);
  else if (!cfile->opening) {
    if (!mainw->is_processing) mt_swap_play_pause(mainw->multitrack, TRUE);
    else {
      lives_widget_set_sensitive(mainw->multitrack->playall, FALSE);
      lives_widget_set_sensitive(mainw->m_playbutton, FALSE);
    }
  }

  lives_table_set_column_homogeneous(LIVES_TABLE(mainw->pf_grid), !mainw->double_size);

  lives_widget_set_sensitive(mainw->m_playselbutton, FALSE);
  lives_widget_set_sensitive(mainw->m_rewindbutton, FALSE);
  lives_widget_set_sensitive(mainw->m_mutebutton, is_realtime_aplayer(audio_player) || mainw->multitrack);

  lives_widget_set_sensitive(mainw->m_loopbutton, (!cfile->achans || mainw->mute || mainw->multitrack ||
                             mainw->loop_cont || is_realtime_aplayer(audio_player))
                             && mainw->current_file > 0);
  lives_widget_set_sensitive(mainw->loop_continue, (!cfile->achans || mainw->mute || mainw->loop_cont ||
                             is_realtime_aplayer(audio_player))
                             && mainw->current_file > 0);

  if (cfile->frames == 0 && !mainw->multitrack) {
    if (mainw->preview_box && lives_widget_get_parent(mainw->preview_box)) {

      lives_container_remove(LIVES_CONTAINER(mainw->play_window), mainw->preview_box);

      mainw->pw_scroll_func = lives_signal_connect(LIVES_GUI_OBJECT(mainw->play_window), LIVES_WIDGET_SCROLL_EVENT,
                              LIVES_GUI_CALLBACK(on_mouse_scroll), NULL);
    }
  } else {
    if (mainw->sep_win) {
      /// create a separate window for the internal player if requested
      if (prefs->sepwin_type == SEPWIN_TYPE_NON_STICKY) {
        make_play_window();
      } else {
        if (!mainw->multitrack) {
          if (mainw->preview_controls) {
            lives_widget_hide(mainw->preview_controls);
            /* mainw->pw_scroll_func = lives_signal_connect(LIVES_GUI_OBJECT(mainw->play_window), LIVES_WIDGET_SCROLL_EVENT, */
            /*                         LIVES_GUI_CALLBACK(on_mouse_scroll), */
            /*                         NULL); */
          }
        }

        if (!mainw->multitrack || mainw->fs) {
          resize_play_window();
        }

        /// needed
        if (!mainw->multitrack) {
          lives_widget_process_updates(LIVES_MAIN_WINDOW_WIDGET);
        } else {
          /// this doesn't get called if we don't call resize_play_window()
          if (mainw->play_window) {
            if (prefs->show_playwin) {
              lives_window_present(LIVES_WINDOW(mainw->play_window));
              lives_xwindow_raise(lives_widget_get_xwindow(mainw->play_window));
	      // *INDENT-OFF*
	    }}}}}
    // *INDENT-ON*

    if (mainw->play_window) {
      hide_cursor(lives_widget_get_xwindow(mainw->play_window));
      lives_widget_set_app_paintable(mainw->play_window, TRUE);
      play_window_set_title();
    }

    if (!mainw->foreign && !mainw->sep_win) {
      hide_cursor(lives_widget_get_xwindow(mainw->playarea));
    }

    if (!mainw->sep_win && !mainw->foreign) {
      if (mainw->double_size) resize(2.);
      else resize(1);
    }

    /* if (mainw->vpp && mainw->vpp->fheight > -1 && mainw->vpp->fwidth > -1) { */
    /*   // fixed o/p size for stream */
    /*   if (mainw->vpp->fwidth * mainw->vpp->fheight == 0) { */
    /*     mainw->vpp->fwidth = DEF_VPP_HSIZE; */
    /*     mainw->vpp->fheight = DEF_VPP_VSIZE; */
    /*   } */
    /*   if (!(mainw->vpp->capabilities & VPP_CAN_RESIZE)) { */
    /*     mainw->pwidth = mainw->vpp->fwidth; */
    /*     mainw->pheight = mainw->vpp->fheight; */
    /*   } */
    /* } */

    if (mainw->fs && !mainw->sep_win && cfile->frames > 0) {
      fullscreen_internal();
    }
  }

  // moved down because xdg-screensaver requires a mapped windowID
  if (prefs->stop_screensaver) {
    lives_freep((void **)&com2);
#ifdef GDK_WINDOWING_X11
    if (!prefs->show_gui && prefs->show_playwin && mainw->play_window) {
      awinid = lives_widget_get_xwinid(mainw->play_window, NULL);
    } else if (prefs->show_gui) {
      awinid = lives_widget_get_xwinid(LIVES_MAIN_WINDOW_WIDGET, NULL);
    }

    com2 = lives_strdup("xset s off 2>/dev/null; xset -dpms 2>/dev/null ;");

    if (capable->has_gconftool_2) {
      char *xnew = lives_strdup(" gconftool-2 --set --type bool /apps/gnome-screensaver/idle_activation_enabled "
                                "false 2>/dev/null ;");
      tmp = lives_concat(com2, xnew);
      com2 = tmp;
    }
    if (capable->has_xdg_screensaver && awinid != -1) {
      char *xnew = lives_strdup_printf(" xdg-screensaver suspend %"PRIu64" 2>/dev/null ;", awinid);
      tmp = lives_concat(com2, xnew);
      com2 = tmp;
    }
#else
    if (capable->has_gconftool_2) {
      com2 = lives_strdup("gconftool-2 --set --type bool /apps/gnome-screensaver/idle_activation_enabled false 2>/dev/null ;");
    } else com2 = lives_strdup("");
#endif
    if (!com2) com2 = lives_strdup("");
  }

  if (!mainw->foreign && prefs->midisynch && !mainw->preview) {
    lives_free(com3);
    com3 = lives_strdup(EXEC_MIDISTART);
  }
  com = lives_strconcat(com2, com3, NULL);
  if (*com) {
    // allow this to fail - not all sub-commands may be present
    lives_system(com, TRUE);
  }
  lives_freep((void **)&com);
  lives_freep((void **)&com2);
  lives_free(com3);
  com3 = lives_strdup(" ");

  if (!mainw->multitrack) {
    lives_spin_button_set_value(LIVES_SPIN_BUTTON(mainw->spinbutton_pb_fps), cfile->pb_fps);

    mainw->last_blend_file = -1;

    // show the framebar
    if (!mainw->multitrack && !mainw->faded
        && (!prefs->hide_framebar &&
            (!mainw->fs || (widget_opts.monitor + 1 != prefs->play_monitor && prefs->play_monitor != 0
                            && capable->nmonitors > 1 &&
                            mainw->sep_win) ||
             (mainw->vpp && mainw->sep_win && !(mainw->vpp->capabilities & VPP_LOCAL_DISPLAY))) &&
            ((!mainw->preview && (cfile->frames > 0 || mainw->foreign)) || cfile->opening))) {
      lives_widget_show(mainw->framebar);
    }
  }

  cfile->play_paused = FALSE;
  mainw->actual_frame = 0;

  mainw->currticks = 0;
  mainw->effort = -EFFORT_RANGE_MAX;

  find_when_to_stop();

  // reinit all active effects
  if (!mainw->preview && !mainw->is_rendering && !mainw->foreign) weed_reinit_all();

  if (!mainw->foreign && (!(prefs->audio_src == AUDIO_SRC_EXT &&
                            (audio_player == AUD_PLAYER_JACK ||
                             audio_player == AUD_PLAYER_PULSE || audio_player == AUD_PLAYER_NONE)))) {
    stopcom = prep_audio_player(com2, com3, audio_end, arate, asigned, aendian);
  }

  lives_free(com3);

  // if recording, refrain from writing audio until we are ready
  if (mainw->record) mainw->record_paused = TRUE;

  // if recording, set up recorder (jack or pulse)
  if (!mainw->preview && (prefs->audio_src == AUDIO_SRC_EXT || (mainw->record && mainw->agen_key != 0))
      && (audio_player == AUD_PLAYER_JACK || audio_player == AUD_PLAYER_PULSE)) {
    mainw->rec_samples = -1; // record unlimited
    if (mainw->record) {
      // create temp clip
      open_ascrap_file();
      if (mainw->ascrap_file != -1) {
        mainw->rec_aclip = mainw->ascrap_file;
        mainw->rec_avel = 1.;
        mainw->rec_aseek = 0;
      }
    }
    if (audio_player == AUD_PLAYER_JACK) {
#ifdef ENABLE_JACK
      if ((prefs->audio_src == AUDIO_SRC_EXT || mainw->agen_key != 0 || mainw->agen_needs_reinit) && mainw->jackd) {
        if (mainw->agen_key != 0 || mainw->agen_needs_reinit) {
          mainw->jackd->playing_file = mainw->current_file;
          if (mainw->ascrap_file != -1 || !prefs->perm_audio_reader)
            jack_rec_audio_to_clip(mainw->ascrap_file, -1, RECA_GENERATED);
        } else {
          if (mainw->ascrap_file != -1 || !prefs->perm_audio_reader)
            jack_rec_audio_to_clip(mainw->ascrap_file, -1, RECA_EXTERNAL);
        }
        //mainw->jackd->in_use = TRUE;
      }
      if (prefs->audio_src == AUDIO_SRC_EXT && mainw->jackd_read) {
        mainw->jackd_read->num_input_channels = mainw->jackd_read->num_output_channels = 2;
        mainw->jackd_read->sample_in_rate = mainw->jackd_read->sample_out_rate;
        mainw->jackd_read->is_paused = TRUE;
        mainw->jackd_read->in_use = TRUE;
      }
#endif
    }
    if (audio_player == AUD_PLAYER_PULSE) {
#ifdef HAVE_PULSE_AUDIO
      if ((prefs->audio_src == AUDIO_SRC_EXT || mainw->agen_key != 0  || mainw->agen_needs_reinit) && mainw->pulsed) {
        if (mainw->agen_key != 0 || mainw->agen_needs_reinit) {
          mainw->pulsed->playing_file = mainw->current_file;
          if (mainw->ascrap_file != -1 || !prefs->perm_audio_reader)
            pulse_rec_audio_to_clip(mainw->ascrap_file, -1, RECA_GENERATED);
        } else {
          if (mainw->ascrap_file != -1 || !prefs->perm_audio_reader)
            pulse_rec_audio_to_clip(mainw->ascrap_file, -1, RECA_EXTERNAL);
        }
        //mainw->pulsed->in_use = TRUE;
      }
      if (prefs->audio_src == AUDIO_SRC_EXT && mainw->pulsed_read) {
        mainw->pulsed_read->in_achans = mainw->pulsed_read->out_achans = PA_ACHANS;
        mainw->pulsed_read->in_asamps = mainw->pulsed_read->out_asamps = PA_SAMPSIZE;
        mainw->pulsed_read->in_arate = mainw->pulsed_read->out_arate;
        mainw->pulsed_read->is_paused = TRUE;
        mainw->pulsed_read->in_use = TRUE;
      }
#endif
    }
  }

  // set in case audio lock gets actioned
  future_prefs->audio_opts = prefs->audio_opts;

  if (mainw->foreign || weed_playback_gen_start()) {
    if (mainw->osc_auto)
      lives_notify(LIVES_OSC_NOTIFY_SUCCESS, "");
    lives_notify(LIVES_OSC_NOTIFY_PLAYBACK_STARTED, "");

#ifdef ENABLE_JACK
    if (mainw->event_list && !mainw->record && audio_player == AUD_PLAYER_JACK && mainw->jackd &&
        !(mainw->preview && mainw->is_processing &&
          !(mainw->multitrack && mainw->preview && mainw->multitrack->is_rendering))) {
      // if playing an event list, we switch to audio memory buffer mode
      if (mainw->multitrack) init_jack_audio_buffers(cfile->achans, cfile->arate, exact_preview);
      else init_jack_audio_buffers(DEFAULT_AUDIO_CHANS, DEFAULT_AUDIO_RATE, FALSE);
      has_audio_buffers = TRUE;
    }
#endif
#ifdef HAVE_PULSE_AUDIO
    if (mainw->event_list && !mainw->record && audio_player == AUD_PLAYER_PULSE && mainw->pulsed &&
        !(mainw->preview && mainw->is_processing &&
          !(mainw->multitrack && mainw->preview && mainw->multitrack->is_rendering))) {
      // if playing an event list, we switch to audio memory buffer mode
      if (mainw->multitrack) init_pulse_audio_buffers(cfile->achans, cfile->arate, exact_preview);
      else init_pulse_audio_buffers(DEFAULT_AUDIO_CHANS, DEFAULT_AUDIO_RATE, FALSE);
      has_audio_buffers = TRUE;
    }
#endif

    mainw->abufs_to_fill = 0;
    //lives_widget_context_update();
    //play until stopped or a stream finishes
    do {
      mainw->cancelled = CANCEL_NONE;
      mainw->play_sequence++;
      mainw->fps_measure = 0;

      if (mainw->event_list && !mainw->record) {
        if (!pb_start_event) pb_start_event = get_first_event(mainw->event_list);

        if (!(mainw->preview && mainw->multitrack && mainw->multitrack->is_rendering))
          init_track_decoders();

        if (has_audio_buffers) {
#ifdef ENABLE_JACK
          if (audio_player == AUD_PLAYER_JACK) {
            int i;
            mainw->write_abuf = 0;

            // fill our audio buffers now
            // this will also get our effects state

            // reset because audio sync may have set it
            if (mainw->multitrack) mainw->jackd->abufs[0]->arate = cfile->arate;
            else mainw->jackd->abufs[0]->arate = mainw->jackd->sample_out_rate;
            fill_abuffer_from(mainw->jackd->abufs[0], mainw->event_list, pb_start_event, exact_preview);
            for (i = 1; i < prefs->num_rtaudiobufs; i++) {
              // reset because audio sync may have set it
              if (mainw->multitrack) mainw->jackd->abufs[i]->arate = cfile->arate;
              else mainw->jackd->abufs[i]->arate = mainw->jackd->sample_out_rate;
              fill_abuffer_from(mainw->jackd->abufs[i], mainw->event_list, NULL, FALSE);
            }

            pthread_mutex_lock(&mainw->abuf_mutex);
            mainw->jackd->read_abuf = 0;
            mainw->abufs_to_fill = 0;
            pthread_mutex_unlock(&mainw->abuf_mutex);
            if (mainw->event_list)
              mainw->jackd->in_use = TRUE;
          }
#endif
#ifdef HAVE_PULSE_AUDIO
          if (audio_player == AUD_PLAYER_PULSE) {
            int i;
            mainw->write_abuf = 0;

            /// fill our audio buffers now
            /// this will also get our effects state

            /// this is the IN rate, everything is resampled to this rate and then to output rate
            if (mainw->multitrack) mainw->pulsed->abufs[0]->arate = cfile->arate;
            else mainw->pulsed->abufs[0]->arate = mainw->pulsed->out_arate;

            /// need to set asamps, in case padding with silence is needed
            mainw->pulsed->abufs[0]->out_asamps = mainw->pulsed->out_asamps;

            fill_abuffer_from(mainw->pulsed->abufs[0], mainw->event_list, pb_start_event, exact_preview);
            for (i = 1; i < prefs->num_rtaudiobufs; i++) {
              if (mainw->multitrack) mainw->pulsed->abufs[i]->arate = cfile->arate;
              else mainw->pulsed->abufs[i]->arate = mainw->pulsed->out_arate;
              mainw->pulsed->abufs[i]->out_asamps = mainw->pulsed->out_asamps;
              fill_abuffer_from(mainw->pulsed->abufs[i], mainw->event_list, NULL, FALSE);
            }

            pthread_mutex_lock(&mainw->abuf_mutex);
            mainw->pulsed->read_abuf = 0;
            mainw->abufs_to_fill = 0;
            pthread_mutex_unlock(&mainw->abuf_mutex);
            if (mainw->event_list) {
              mainw->pulsed->in_use = TRUE;
            }
          }
#endif
        }
      }

      if (!mainw->foreign && !mainw->multitrack)
        mainw->video_seek_ready = mainw->audio_seek_ready = FALSE;
      else
        mainw->video_seek_ready = mainw->audio_seek_ready = TRUE;

      if (!mainw->multitrack || !mainw->multitrack->pb_start_event) {
        do_progress_dialog(FALSE, FALSE, NULL);

        // reset audio buffers
#ifdef ENABLE_JACK
        if (audio_player == AUD_PLAYER_JACK && mainw->jackd) {
          // must do this before deinit fx
          pthread_mutex_lock(&mainw->abuf_mutex);
          mainw->jackd->read_abuf = -1;
          mainw->jackd->in_use = FALSE;
          pthread_mutex_unlock(&mainw->abuf_mutex);
        }
#endif
#ifdef HAVE_PULSE_AUDIO
        if (audio_player == AUD_PLAYER_PULSE && mainw->pulsed) {
          // must do this before deinit fx
          pthread_mutex_lock(&mainw->abuf_mutex);
          mainw->pulsed->read_abuf = -1;
          mainw->pulsed->in_use = FALSE;
          pthread_mutex_unlock(&mainw->abuf_mutex);
        }
#endif
      } else {
        // play from middle of mt timeline
        cfile->next_event = mainw->multitrack->pb_start_event;

        if (!has_audio_buffers) {
          // no audio buffering
          // get just effects state
          get_audio_and_effects_state_at(mainw->multitrack->event_list, mainw->multitrack->pb_start_event, 0,
                                         LIVES_PREVIEW_TYPE_VIDEO_ONLY, mainw->multitrack->exact_preview);
        }

        do_progress_dialog(FALSE, FALSE, NULL);

        // reset audio read buffers
#ifdef ENABLE_JACK
        if (audio_player == AUD_PLAYER_JACK && mainw->jackd) {
          // must do this before deinit fx
          pthread_mutex_lock(&mainw->abuf_mutex);
          mainw->jackd->read_abuf = -1;
          mainw->jackd->in_use = FALSE;
          pthread_mutex_unlock(&mainw->abuf_mutex);
        }
#endif
#ifdef HAVE_PULSE_AUDIO
        if (audio_player == AUD_PLAYER_PULSE && mainw->pulsed) {
          // must do this before deinit fx
          pthread_mutex_lock(&mainw->abuf_mutex);
          mainw->pulsed->read_abuf = -1;
          mainw->pulsed->in_use = FALSE;
          pthread_mutex_unlock(&mainw->abuf_mutex);
        }
#endif
        // realtime effects off (for multitrack and event_list preview)
        deinit_render_effects();

        cfile->next_event = NULL;

        if (!(mainw->preview && mainw->multitrack && mainw->multitrack->is_rendering))
          free_track_decoders();

        // multitrack loop - go back to loop start position unless external transport moved us
        if (mainw->scratch == SCRATCH_NONE) {
          mainw->multitrack->pb_start_event = mainw->multitrack->pb_loop_event;
        }
      }
      mainw->effort = 0;
      if (mainw->multitrack) pb_start_event = mainw->multitrack->pb_start_event;
    } while (mainw->multitrack && (mainw->loop_cont || mainw->scratch != SCRATCH_NONE) &&
             (mainw->cancelled == CANCEL_NONE || mainw->cancelled == CANCEL_EVENT_LIST_END));
  }

  mainw->osc_block = TRUE;
  mainw->rte_textparm = NULL;
  mainw->playing_file = -1;
  mainw->abufs_to_fill = 0;

  if (!mainw->foreign) {
    /// deinit any active real time effects
    if (prefs->allow_easing && !mainw->multitrack) {
      // any effects which were "easing out" should be deinited now
      deinit_easing_effects();
    }
  }

  if (mainw->ext_playback) {
#ifndef IS_MINGW
    vid_playback_plugin_exit();
    if (mainw->play_window) lives_window_unfullscreen(LIVES_WINDOW(mainw->play_window));
#else
    if (mainw->play_window) lives_window_unfullscreen(LIVES_WINDOW(mainw->play_window));
    vid_playback_plugin_exit();
#endif
  }

  // play completed
  if (prefs->show_player_stats) {
    if (mainw->fps_measure > 0) {
      fps_med = (double)mainw->fps_measure / ((double)lives_get_relative_ticks(mainw->origsecs, mainw->orignsecs)
                                              / TICKS_PER_SECOND_DBL);
    }
  }
  mainw->video_seek_ready = mainw->audio_seek_ready = FALSE;
  mainw->osc_auto = 0;

  if (mainw->loop_locked) unlock_loop_lock();
  if (prefs->show_msg_area) {
    lives_widget_set_size_request(mainw->message_box, -1, MIN_MSGBAR_HEIGHT);
  }

  mainw->jack_can_stop = FALSE;
  if ((mainw->current_file == current_file) && CURRENT_CLIP_IS_VALID) {
    cfile->pointer_time = pointer_time;
    cfile->real_pointer_time = real_pointer_time;
  }

#ifdef ENABLE_JACK
  if (audio_player == AUD_PLAYER_JACK && (mainw->jackd || mainw->jackd_read)) {
    if (mainw->jackd_read || mainw->aud_rec_fd != -1)
      jack_rec_audio_end(!prefs->perm_audio_reader, TRUE);

    if (mainw->jackd_read) {
      mainw->jackd_read->in_use = FALSE;
    }

    // send jack transport stop
    if (!mainw->preview && !mainw->foreign) jack_pb_stop();

    // tell jack client to close audio file
    if (mainw->jackd && mainw->jackd->playing_file > 0) {
      ticks_t timeout = 0;
      if (mainw->cancelled != CANCEL_AUDIO_ERROR) {
        lives_alarm_t alarm_handle = lives_alarm_set(LIVES_DEFAULT_TIMEOUT);
        while ((timeout = lives_alarm_check(alarm_handle)) > 0 && jack_get_msgq(mainw->jackd)) {
          sched_yield(); // wait for seek
          lives_usleep(prefs->sleep_time);
        }
        lives_alarm_clear(alarm_handle);
      }
      if (mainw->cancelled == CANCEL_AUDIO_ERROR) mainw->cancelled = CANCEL_ERROR;
      jack_message.command = ASERVER_CMD_FILE_CLOSE;
      jack_message.data = NULL;
      jack_message.next = NULL;
      mainw->jackd->msgq = &jack_message;
      if (timeout == 0) handle_audio_timeout();
      else {
        while (mainw->jackd->playing_file > -1) {
          sched_yield();
          lives_usleep(prefs->sleep_time);
        }
      }
    }
  } else {
#endif
#ifdef HAVE_PULSE_AUDIO
    if (audio_player == AUD_PLAYER_PULSE && (mainw->pulsed || mainw->pulsed_read)) {
      if (mainw->pulsed_read || mainw->aud_rec_fd != -1)
        pulse_rec_audio_end(!prefs->perm_audio_reader, TRUE);

      if (mainw->pulsed_read) {
        mainw->pulsed_read->in_use = FALSE;
        pulse_driver_cork(mainw->pulsed_read);
      }

      // tell pulse client to close audio file
      if (mainw->pulsed) {
        if (mainw->pulsed->playing_file > 0 || mainw->pulsed->fd > 0) {
          ticks_t timeout = 0;
          if (mainw->cancelled != CANCEL_AUDIO_ERROR) {
            lives_alarm_t alarm_handle = lives_alarm_set(LIVES_DEFAULT_TIMEOUT);
            while ((timeout = lives_alarm_check(alarm_handle)) > 0 && pulse_get_msgq(mainw->pulsed)) {
              sched_yield(); // wait for seek
              lives_usleep(prefs->sleep_time);
            }
            lives_alarm_clear(alarm_handle);
          }
          if (mainw->cancelled == CANCEL_AUDIO_ERROR) mainw->cancelled = CANCEL_ERROR;
          pulse_message.command = ASERVER_CMD_FILE_CLOSE;
          pulse_message.data = NULL;
          pulse_message.next = NULL;
          mainw->pulsed->msgq = &pulse_message;
          if (timeout == 0)  {
            handle_audio_timeout();
            mainw->pulsed->playing_file = -1;
            mainw->pulsed->fd = -1;
          } else {
            while (mainw->pulsed->playing_file > -1 || mainw->pulsed->fd > 0) {
              sched_yield();
              lives_usleep(prefs->sleep_time);
            }
            pulse_driver_cork(mainw->pulsed);
          }
        } else {
          pulse_driver_cork(mainw->pulsed);
        }
      }
    } else {
#endif
      if (!is_realtime_aplayer(audio_player) && stopcom) {
        // kill sound (if still playing)
        lives_system(stopcom, TRUE);
        mainw->aud_file_to_kill = -1;
        lives_free(stopcom);
      }
#ifdef ENABLE_JACK
    }
#endif
#ifdef HAVE_PULSE_AUDIO
  }
#endif

  lives_freep((void **)&com);
  lives_freep((void **)&mainw->urgency_msg);
  mainw->actual_frame = 0;

  lives_notify(LIVES_OSC_NOTIFY_PLAYBACK_STOPPED, "");

  if (mainw->new_clip != -1) {
    mainw->current_file = mainw->new_clip;
    mainw->new_clip = -1;
  }

  // stop the audio players
#ifdef ENABLE_JACK
  if (audio_player == AUD_PLAYER_JACK && mainw->jackd) {
    mainw->jackd->in_use = FALSE;
  }
#endif
#ifdef HAVE_PULSE_AUDIO
  if (audio_player == AUD_PLAYER_PULSE && mainw->pulsed) {
    mainw->pulsed->in_use = FALSE;
  }
#endif

  /// stop the players before the cache thread, else the players may try to play from a non-existent file
  if (audio_player == AUD_PLAYER_JACK
      || (mainw->event_list && !mainw->record && (!mainw->is_rendering
          || !mainw->preview || mainw->preview_rendering)))
    audio_cache_end();

  // terminate autolives if running
  lives_check_menu_item_set_active(LIVES_CHECK_MENU_ITEM(mainw->autolives), FALSE);

  // PLAY FINISHED...

  // allow this to fail - not all sub-commands may be present
  if (prefs->stop_screensaver) {
#ifdef GDK_WINDOWING_X11
    com = lives_strdup("xset s on 2>/dev/null; xset +dpms 2>/dev/null ;");

    if (capable->has_gconftool_2) {
      char *xnew = lives_strdup(" gconftool-2 --set --type bool /apps/gnome-screensaver/idle_activation_enabled "
                                "true 2>/dev/null ;");
      tmp = lives_strconcat(com, xnew, NULL);
      lives_free(com);
      lives_free(xnew);
      com = tmp;
    }
    if (capable->has_xdg_screensaver && awinid != -1) {
      char *xnew = lives_strdup_printf(" xdg-screensaver resume %"PRIu64" 2>/dev/null ;", awinid);
      tmp = lives_strconcat(com, xnew, NULL);
      lives_free(com);
      lives_free(xnew);
      com = tmp;
    }
#else
    if (capable->has_gconftool_2) {
      com = lives_strdup("gconftool-2 --set --type bool /apps/gnome-screensaver/idle_activation_enabled true 2>/dev/null ;");
    } else com = lives_strdup("");
#endif

    if (com) {
      lives_system(com, TRUE);
      lives_free(com);
    }
  }

  if (!mainw->multitrack && mainw->ext_audio_mon)
    lives_toggle_tool_button_set_active(LIVES_TOGGLE_TOOL_BUTTON(mainw->ext_audio_mon), FALSE);

  // reset in case audio lock was actioned
  prefs->audio_opts = future_prefs->audio_opts;

  // TODO ***: use MIDI output port for this
  if (!mainw->foreign && prefs->midisynch) lives_system(EXEC_MIDISTOP, TRUE);

  // we could have started by playing a generator, which could've been closed
  if (!mainw->files[current_file]) current_file = mainw->current_file;

  if (!is_realtime_aplayer(audio_player)) {
    // wait for audio_ended...
    if (cfile->achans > 0 && com2) {
      wait_for_stop(com2);
      mainw->aud_file_to_kill = -1;
    }
    lives_freep((void **)&com2);
  }

  if (CURRENT_CLIP_IS_NORMAL) {
    cfile->last_play_sequence = mainw->play_sequence;
    stfile = lives_build_filename(prefs->workdir, cfile->handle, LIVES_STATUS_FILE_NAME, NULL);
    lives_snprintf(cfile->info_file, PATH_MAX, "%s", stfile);
    lives_free(stfile);
  }

  if (IS_VALID_CLIP(mainw->scrap_file) && mainw->files[mainw->scrap_file]->ext_src) {
    lives_close_buffered(LIVES_POINTER_TO_INT(mainw->files[mainw->scrap_file]->ext_src));
    mainw->files[mainw->scrap_file]->ext_src = NULL;
    mainw->files[mainw->scrap_file]->ext_src_type = LIVES_EXT_SRC_NONE;
  }

  if (mainw->foreign) {
    // recording from external window capture
    mainw->pwidth = lives_widget_get_allocation_width(mainw->playframe) - H_RESIZE_ADJUST;
    mainw->pheight = lives_widget_get_allocation_height(mainw->playframe) - V_RESIZE_ADJUST;

    cfile->hsize = mainw->pwidth;
    cfile->vsize = mainw->pheight;

    lives_xwindow_set_keep_above(mainw->foreign_window, FALSE);

    lives_widget_process_updates(LIVES_MAIN_WINDOW_WIDGET);

    return;
  }

  disable_record();
  prefs->pb_quality = future_prefs->pb_quality;
  mainw->lockstats = FALSE;
  mainw->blend_palette = WEED_PALETTE_END;
  mainw->audio_stretch = 1.;

  if (!mainw->multitrack) {
    if (mainw->faded || mainw->fs) {
      unfade_background();
    }

    if (mainw->sep_win) add_to_playframe();

    if (CURRENT_CLIP_HAS_VIDEO) {
      resize(1.);
      lives_widget_show_all(mainw->playframe);
      lives_frame_set_label(LIVES_FRAME(mainw->playframe), NULL);
    }

    if (palette->style & STYLE_1) {
      lives_widget_show(mainw->sep_image);
    }

    if (prefs->show_msg_area && !mainw->multitrack) {
      lives_widget_show(mainw->message_box);
      reset_message_area(); ///< necessary
    }

    lives_widget_show(mainw->frame1);
    lives_widget_show(mainw->frame2);
    lives_widget_show(mainw->eventbox3);
    lives_widget_show(mainw->eventbox4);
    lives_widget_show(mainw->sep_image);

    if (!prefs->hide_framebar && !prefs->hfbwnp) {
      lives_widget_show(mainw->framebar);
    }
  }

  if (!is_realtime_aplayer(audio_player)) mainw->mute = mute;

  /// kill the separate play window
  if (mainw->play_window) {
    if (mainw->fs) {
      mainw->ignore_screen_size = TRUE;
      if (prefs->show_desktop_panel && (capable->wm_caps.pan_annoy & ANNOY_DISPLAY)
          && (capable->wm_caps.pan_annoy & ANNOY_FS) && (capable->wm_caps.pan_res & RES_HIDE) &&
          capable->wm_caps.pan_res & RESTYPE_ACTION) {
        show_desktop_panel();
      }
      lives_window_unfullscreen(LIVES_WINDOW(mainw->play_window));
    }
    if (prefs->sepwin_type == SEPWIN_TYPE_NON_STICKY) {
      kill_play_window();
    } else {
      /// or resize it back to single size
      if (CURRENT_CLIP_IS_VALID && cfile->is_loaded && cfile->frames > 0 && !mainw->is_rendering &&
          (cfile->clip_type != CLIP_TYPE_GENERATOR)) {
        if (mainw->preview_controls) {
          /// create the preview in the sepwin
          if (prefs->show_gui) {
            lives_widget_set_no_show_all(mainw->preview_controls, FALSE);
            lives_widget_show_all(mainw->preview_box);
            lives_widget_show_now(mainw->preview_box);
            lives_widget_set_no_show_all(mainw->preview_controls, TRUE);
            //lives_widget_grab_focus(mainw->preview_spinbutton);
          }
        }
        if (mainw->current_file != current_file) {
          // now we have to guess how to center the play window
          mainw->opwx = mainw->opwy = -1;
          mainw->preview_frame = 0;
        }
      }

      if (!mainw->multitrack) {
        mainw->playing_file = -2;
        if (mainw->fs) mainw->ignore_screen_size = TRUE;
        resize_play_window();
        mainw->playing_file = -1;
        lives_widget_queue_draw(LIVES_MAIN_WINDOW_WIDGET);

        if (!mainw->preview_box) {
          // create the preview box that shows frames
          make_preview_box();
        }
        // and add it to the play window
        if (!lives_widget_get_parent(mainw->preview_box) && CURRENT_CLIP_IS_NORMAL && !mainw->is_rendering) {
          lives_widget_queue_draw(mainw->play_window);
          lives_container_add(LIVES_CONTAINER(mainw->play_window), mainw->preview_box);
          play_window_set_title();
        }

        if (mainw->play_window) {
          if (prefs->show_playwin) {
            lives_window_present(LIVES_WINDOW(mainw->play_window));
            lives_xwindow_raise(lives_widget_get_xwindow(mainw->play_window));
            unhide_cursor(lives_widget_get_xwindow(mainw->play_window));
            lives_widget_set_no_show_all(mainw->preview_controls, FALSE);
            lives_widget_show_all(mainw->preview_box);
            lives_widget_show_now(mainw->preview_box);
            lives_widget_grab_focus(mainw->preview_spinbutton);
            lives_widget_set_no_show_all(mainw->preview_controls, TRUE);
            lives_widget_process_updates(mainw->play_window);
            lives_window_center(LIVES_WINDOW(mainw->play_window));
            clear_widget_bg(mainw->play_image, mainw->play_surface);
            load_preview_image(FALSE);
	  // *INDENT-OFF*
	  }}}}}
  // *INDENT-ON*

  /// free the last frame image
  if (mainw->frame_layer) {
    weed_layer_free(mainw->frame_layer);
    mainw->frame_layer = NULL;
  }

  if (mainw->lazy) mainw->lazy = lives_idle_add_simple(lazy_startup_checks, NULL);

  cliplist = mainw->cliplist;
  while (cliplist) {
    int i = LIVES_POINTER_TO_INT(cliplist->data);
    if (IS_NORMAL_CLIP(i) && mainw->files[i]->clip_type == CLIP_TYPE_FILE)
      chill_decoder_plugin(i);
    mainw->files[i]->adirection = LIVES_DIRECTION_FORWARD;
    cliplist = cliplist->next;
  }

  if (!mainw->foreign) {
    unhide_cursor(lives_widget_get_xwindow(mainw->playarea));
  }

  if (CURRENT_CLIP_IS_VALID) cfile->play_paused = FALSE;

  if (mainw->blend_file != -1 && mainw->blend_file != mainw->current_file && mainw->files[mainw->blend_file] &&
      mainw->files[mainw->blend_file]->clip_type == CLIP_TYPE_GENERATOR) {
    int xcurrent_file = mainw->current_file;
    weed_bg_generator_end((weed_plant_t *)mainw->files[mainw->blend_file]->ext_src);
    mainw->current_file = xcurrent_file;
  }

  mainw->filter_map = mainw->afilter_map = mainw->audio_event = NULL;

  /// disable the freeze key
  lives_accel_group_disconnect(LIVES_ACCEL_GROUP(mainw->accel_group), freeze_closure);
  lives_accel_group_disconnect(LIVES_ACCEL_GROUP(mainw->accel_group), bg_freeze_closure);

  if (needsadone) d_print_done();

  /// free any pre-cached frame
  if (mainw->frame_layer_preload && mainw->pred_clip != -1) {
    check_layer_ready(mainw->frame_layer_preload);
    weed_layer_free(mainw->frame_layer_preload);
  }
  mainw->frame_layer_preload = NULL;

  if (!prefs->vj_mode) {
    /// pop up error dialog if badly sized frames were detected
    if (mainw->size_warn) {
      if (mainw->size_warn > 0 && mainw->files[mainw->size_warn]) {
        char *smsg = lives_strdup_printf(
                       _("\n\nSome frames in the clip\n%s\nare wrongly sized.\nYou should "
                         "click on Tools--->Resize All\n"
                         "and resize all frames to the current size.\n"),
                       mainw->files[mainw->size_warn]->name);
        widget_opts.non_modal = TRUE;
        do_error_dialog(smsg);
        widget_opts.non_modal = FALSE;
        lives_free(smsg);
      }
    }
  }
  mainw->size_warn = 0;

  // set processing state again if a previewe finished
  // CAUTION !!
  mainw->is_processing = mainw->preview;
  /////////////////

  if (prefs->volume != (double)future_prefs->volume) pref_factory_float(PREF_MASTER_VOLUME, future_prefs->volume, TRUE);

  // TODO - ????
  if (CURRENT_CLIP_IS_VALID && cfile->clip_type == CLIP_TYPE_DISK && cfile->frames == 0 && mainw->record_perf) {
    lives_signal_handler_block(mainw->record_perf, mainw->record_perf_func);
    lives_check_menu_item_set_active(LIVES_CHECK_MENU_ITEM(mainw->record_perf), FALSE);
    lives_signal_handler_unblock(mainw->record_perf, mainw->record_perf_func);
  }

  // TODO - can this be done earlier ?
  if (mainw->cancelled == CANCEL_APP_QUIT) on_quit_activate(NULL, NULL);

  /// end record performance

#ifdef ENABLE_JACK
  if (audio_player == AUD_PLAYER_JACK && mainw->jackd) {
    ticks_t timeout;
    lives_alarm_t alarm_handle = lives_alarm_set(LIVES_DEFAULT_TIMEOUT);
    while ((timeout = lives_alarm_check(alarm_handle)) > 0 && jack_get_msgq(mainw->jackd)) {
      sched_yield(); ///< wait for seek
      lives_usleep(prefs->sleep_time);
    }
    lives_alarm_clear(alarm_handle);
    if (timeout == 0)  {
      handle_audio_timeout();
    }
    if (has_audio_buffers) {
      free_jack_audio_buffers();
      audio_free_fnames();
    }
  }
#endif
#ifdef HAVE_PULSE_AUDIO
  if (audio_player == AUD_PLAYER_PULSE && mainw->pulsed) {
    ticks_t timeout;
    lives_alarm_t alarm_handle = lives_alarm_set(LIVES_DEFAULT_TIMEOUT);
    while ((timeout = lives_alarm_check(alarm_handle)) > 0 && pulse_get_msgq(mainw->pulsed)) {
      sched_yield(); ///< wait for seek
      lives_usleep(prefs->sleep_time);
    }
    lives_alarm_clear(alarm_handle);
    if (timeout == 0)  {
      handle_audio_timeout();
    }

    if (has_audio_buffers) {
      free_pulse_audio_buffers();
      audio_free_fnames();
    }
  }
#endif

  if (prefs->show_player_stats) {
    if (mainw->fps_measure > 0) {
      d_print(_("Average FPS was %.4f (%d frames in clock time of %f)\n"), fps_med, mainw->fps_measure,
              (double)lives_get_relative_ticks(mainw->origsecs, mainw->orignsecs) / TICKS_PER_SECOND_DBL);
    }
  }

  if (THREADVAR(bad_aud_file)) {
    /// we got an error recording audio
    do_write_failed_error_s(THREADVAR(bad_aud_file), NULL);
    lives_freep((void **)&THREADVAR(bad_aud_file));
  }

  if (mainw->new_vpp) {
    mainw->noswitch = FALSE;
    mainw->vpp = open_vid_playback_plugin(mainw->new_vpp, TRUE);
    mainw->new_vpp = NULL;
    mainw->noswitch = TRUE;
  }

  if (!mainw->multitrack && CURRENT_CLIP_HAS_VIDEO) {
    lives_widget_set_sensitive(mainw->spinbutton_start, TRUE);
    lives_widget_set_sensitive(mainw->spinbutton_end, TRUE);
  }

  if (!mainw->preview && CURRENT_CLIP_IS_VALID && cfile->clip_type == CLIP_TYPE_GENERATOR) {
    mainw->osc_block = TRUE;
    weed_generator_end((weed_plant_t *)cfile->ext_src);
    mainw->osc_block = FALSE;
  } else {
    if (mainw->current_file > -1) {
      if (mainw->toy_type == LIVES_TOY_MAD_FRAMES && !cfile->opening) {
        showclipimgs();
        if (!mainw->multitrack)
          redraw_timeline(mainw->current_file);
      }
    }
  }

  if (CURRENT_CLIP_IS_VALID) {
    if (!mainw->multitrack) {
      lives_ce_update_timeline(0, cfile->real_pointer_time);
      mainw->ptrtime = cfile->real_pointer_time;
      lives_widget_queue_draw(mainw->eventbox2);
      lives_widget_queue_draw_if_visible(mainw->framecounter);
    }
  }

  if (!mainw->multitrack) {
    //lives_table_set_column_homogeneous(LIVES_TABLE(mainw->pf_grid), FALSE);
  }

  if (prefs->show_gui && ((mainw->multitrack  && mainw->double_size) ||
                          (lives_widget_get_allocation_height(LIVES_MAIN_WINDOW_WIDGET) > GUI_SCREEN_HEIGHT ||
                           lives_widget_get_allocation_width(LIVES_MAIN_WINDOW_WIDGET) > GUI_SCREEN_WIDTH))) {
    //if (prefs->gui_monitor == 0) lives_window_move(LIVES_WINDOW(LIVES_MAIN_WINDOW_WIDGET), 0, 0);
    if (prefs->open_maximised)
      lives_window_maximize(LIVES_WINDOW(LIVES_MAIN_WINDOW_WIDGET));
    lives_widget_queue_draw(LIVES_MAIN_WINDOW_WIDGET);
  }

  if (!mainw->preview && (mainw->current_file == -1 || (CURRENT_CLIP_IS_VALID && !cfile->opening))) {
    sensitize();
  }

  if (CURRENT_CLIP_IS_VALID && cfile->opening) {
    lives_widget_set_sensitive(mainw->mute_audio, cfile->achans > 0);
    lives_widget_set_sensitive(mainw->loop_continue, TRUE);
    lives_widget_set_sensitive(mainw->loop_video, cfile->achans > 0 && cfile->frames > 0);
  }

  if (mainw->cancelled != CANCEL_USER_PAUSED) {
    lives_widget_set_sensitive(mainw->stop, FALSE);
    lives_widget_set_sensitive(mainw->m_stopbutton, FALSE);
  }

  lives_widget_set_sensitive(mainw->spinbutton_pb_fps, FALSE);

  if (!mainw->multitrack) {
    /// update screen for internal players
    if (prefs->hfbwnp) {
      lives_widget_hide(mainw->framebar);
    }
    set_drawing_area_from_pixbuf(mainw->play_image, NULL, mainw->play_surface);
    lives_widget_set_opacity(mainw->play_image, 0.);
  }

  if (!mainw->multitrack) mainw->osc_block = FALSE;

  reset_clipmenu();

  lives_menu_item_set_accel_path(LIVES_MENU_ITEM(mainw->quit), LIVES_ACCEL_PATH_QUIT);

  if (!mainw->multitrack && CURRENT_CLIP_IS_VALID)
    set_main_title(cfile->name, 0);

  if (!mainw->multitrack && !mainw->foreign && CURRENT_CLIP_IS_VALID && (!cfile->opening ||
      cfile->clip_type == CLIP_TYPE_FILE)) {
    showclipimgs();
    redraw_timeline(mainw->current_file);
  }

  if (prefs->show_msg_area) {
    if (mainw->idlemax == 0) {
      lives_idle_add_simple(resize_message_area, NULL);
    }
    mainw->idlemax = DEF_IDLE_MAX;
  }

  lives_widget_queue_draw(LIVES_MAIN_WINDOW_WIDGET);

  /// need to do this here, in case we want to preview with only a generator and no other clips (which will close to -1)
  if (mainw->record) {
    lives_idle_add_simple(render_choice_idle, LIVES_INT_TO_POINTER(FALSE));
  }

  mainw->record_paused = mainw->record_starting = mainw->record = FALSE;

  mainw->ignore_screen_size = FALSE;

  /// re-enable generic clip switching
  mainw->noswitch = FALSE;
}


/**
   @brief close cfile and switch to new clip (may be -1)

   note this only closes the disk and basic resources, it does not affect the interface
   (c.f. close_current_file())
   returns new_clip */
int close_temp_handle(int new_clip) {
  char *com, *permitname;
  int clipno = mainw->current_file;

  if (!IS_VALID_CLIP(new_clip)) new_clip = -1;
  if (!IS_VALID_CLIP(clipno)) {
    mainw->current_file = new_clip;
    return new_clip;
  }
  if (cfile->clip_type != CLIP_TYPE_TEMP
      && mainw->current_file != mainw->scrap_file && mainw->current_file != mainw->ascrap_file) {
    close_current_file(new_clip);
  }

  // as a safety feature we create a special file which allows the back end to delete the directory
  permitname = lives_build_filename(prefs->workdir, cfile->handle, TEMPFILE_MARKER "." LIVES_FILE_EXT_TMP, NULL);
  lives_touch(permitname);
  lives_free(permitname);

  com = lives_strdup_printf("%s close \"%s\"", prefs->backend, cfile->handle);
  lives_system(com, TRUE);
  lives_free(com);
  lives_freep((void **)&mainw->files[clipno]);

  mainw->current_file = new_clip;

  if (mainw->first_free_file == ALL_USED || mainw->first_free_file > clipno)
    mainw->first_free_file = clipno;
  return new_clip;
}


/**
   @brief get next free file slot, or -1 if we are full

  can support MAX_FILES files (default 65536) */
static int get_next_free_file(void) {
  int idx = mainw->first_free_file++;
  while ((mainw->first_free_file != ALL_USED) && mainw->files[mainw->first_free_file]) {
    mainw->first_free_file++;
    if (mainw->first_free_file >= MAX_FILES) mainw->first_free_file = ALL_USED;
  }
  return idx;
}


/**
    @brief get a temp "handle" from disk.

    Call this to get a temp handle for returning info from the backend
    (this is deprecated for simple data, use lives_popen() instead whenever possible)

    This function is also called from get_new_handle() to create a permanent handle
    for an opened file.

    there are two special instances when this is called with an index != -1:
    - when saving a set and a clip is moved from outside the set to inside it.
    we need a new handle which is guaranteed unique for the set, but we retain all the other details
    - when called from get_new_handle() to create the disk part of a clip

    otherwise, index should be passed in as -1 (the normal case)
    -- handle will be fetched and a directory created in workdir.
    -- clip_type is set to CLIP_TYPE_TEMP.
    call close_temp_handle() on it after use, then restore mainw->current_file

    function returns FALSE if write to workdir fails.

    WARNING:
    this function changes mainw->current_file, unless it returns FALSE (could not create cfile)

    get_new_handle() calls this with the index value passed to it, which should not be -1,
    sets defaults for the clip,
    and also sets the clip name and filename. That function should be used instead to create permanent clips. */
boolean get_temp_handle(int index) {
  boolean is_unique, create = FALSE;

  if (CURRENT_CLIP_IS_TEMP) {
    break_me("temp clip in temp clip !!");
    return TRUE;
  }

  if (index < -1 || index > MAX_FILES) {
    char *msg = lives_strdup_printf("Attempt to create invalid new temp clip %d\n", index);
    LIVES_WARN(msg);
    lives_free(msg);
    return FALSE;
  }

  if (index == -1) {
    if (mainw->first_free_file == ALL_USED) {
      too_many_files();
      return FALSE;
    }
    create = TRUE;
    index = mainw->first_free_file;
    get_next_free_file();
  }

  do {
    is_unique = TRUE;

    // get handle from info file, the first time we will also malloc a
    // new "file" struct here and create a directory in prefs->workdir
    if (!get_handle_from_info_file(index)) {
      lives_freep((void **)&mainw->files[index]);
      if (mainw->first_free_file == ALL_USED || index < mainw->first_free_file)
        mainw->first_free_file = index;
      return FALSE;
    }

    if (*mainw->set_name) {
      char *setclipdir = CLIPDIR(cfile->handle);
      if (lives_file_test(setclipdir, LIVES_FILE_TEST_IS_DIR)) is_unique = FALSE;
      lives_free(setclipdir);
    }
  } while (!is_unique);

  mainw->current_file = index;

  if (create) {
    // create a marker file in directory, else we will be barred from
    // removing it
    char *canremove = lives_build_filename(prefs->workdir, cfile->handle,
                                           TEMPFILE_MARKER "." LIVES_FILE_EXT_TMP, NULL);
    lives_touch(canremove);
    lives_free(canremove);
    // fill with default values
    create_cfile(index, cfile->handle, FALSE);
    cfile->clip_type = CLIP_TYPE_TEMP;
  }
  return TRUE;
}


/**
   @brief set default values for a clip (in memory)

   if new_file == -1 we create (malloc) a new clip and switch to it
   - setting its handle to "handle" (reload / crash recovery)

   if new_file != -1 the parameter "handle" is ignored, and we switch to new_file, without mallocing anything
   - "handle" in the clip must have been set already (called from get_new_handle() and get_temp_handle())
   -- get_new_handle() will set name and fliename and switch back to original clip.

   default values are then set for the clip
   - a "unique_id" is assigned via uuidgen or lives_random()
   - type is set to CLIP_TYPE_DISK
   - img_type is set depending on prefs->image_type
   - frames is set to 0
   etc.

   - loaded is set = to is_loaded

   WARNING: on success, returns the clip, and changes the value of
   mainw->current_file !!  returns NULL if: new_file is out of range
   or points to a NULL clip; new_file is -1 and all free clips are
   in use (unlikely), or malloc fails.
*/
lives_clip_t *create_cfile(int new_file, const char *handle, boolean is_loaded) {
  lives_clip_t *sfile;
  char *stfile;

  if (new_file == -1) {
    // if new_file == -1, we are going to create a new clip
    new_file = mainw->first_free_file;
    if (new_file == -1) {
      too_many_files();
      return NULL;
    }

    mainw->current_file = new_file;
    get_next_free_file();

    if (new_file < 0 || new_file > MAX_FILES || IS_VALID_CLIP(new_file)) {
      char *msg = lives_strdup_printf("Attempt to create invalid new clip %d\n", new_file);
      LIVES_WARN(msg);
      lives_free(msg);
      return NULL;
    }

    if (!handle) {
      // if handle is NULL, we create a new clip on disk, switch to it
      // (unused)
      if (!get_handle_from_info_file(new_file)) return NULL;
      sfile = mainw->files[new_file];
    } else {
      // else just create the in-memory part and set the handle
      sfile = mainw->files[new_file] = (lives_clip_t *)(lives_calloc(1, sizeof(lives_clip_t)));
      if (!sfile) return NULL;
      lives_snprintf(sfile->handle, 256, "%s", handle);
    }
  }

  mainw->current_file = new_file;

  cfile->is_loaded = is_loaded;

  // any cfile (clip) initialisation goes in here
  lives_memcpy((void *)&cfile->binfmt_check.chars, "LiVESXXX", 8);
  cfile->binfmt_version.num = make_version_hash(LiVES_VERSION);
  cfile->binfmt_bytes.size = (size_t)((void *)&cfile->binfmt_end - (void *)cfile);
  cfile->menuentry = NULL;
  cfile->start = cfile->end = 0;
  cfile->old_frames = cfile->opening_frames = cfile->frames = 0;
  lives_snprintf(cfile->type, 40, "%s", _("Unknown"));
  cfile->f_size = 0l;
  cfile->achans = 0;
  cfile->arate = 0;
  cfile->arps = 0;
  cfile->afilesize = 0l;
  cfile->asampsize = 0;
  cfile->adirection = LIVES_DIRECTION_FORWARD;
  cfile->undoable = FALSE;
  cfile->redoable = FALSE;
  cfile->changed = FALSE;
  cfile->was_in_set = FALSE;
  cfile->hsize = cfile->vsize = cfile->ohsize = cfile->ovsize = 0;
  cfile->fps = cfile->pb_fps = prefs->default_fps;
  cfile->resample_events = NULL;
  cfile->insert_start = cfile->insert_end = 0;
  cfile->is_untitled = TRUE;
  cfile->was_renamed = FALSE;
  cfile->undo_action = UNDO_NONE;
  cfile->opening_audio = cfile->opening = cfile->opening_only_audio = FALSE;
  cfile->pointer_time = 0.;
  cfile->real_pointer_time = 0.;
  cfile->restoring = cfile->opening_loc = cfile->nopreview = FALSE;
  cfile->video_time = cfile->laudio_time = cfile->raudio_time = 0.;
  cfile->freeze_fps = 0.;
  cfile->last_vframe_played = 0;
  cfile->frameno = cfile->last_frameno = cfile->saved_frameno = 1;
  cfile->progress_start = cfile->progress_end = 0;
  cfile->play_paused = cfile->nokeep = FALSE;
  cfile->undo_start = cfile->undo_end = 0;
  cfile->ext_src = NULL;
  cfile->ext_src_type = LIVES_EXT_SRC_NONE;
  cfile->clip_type = CLIP_TYPE_DISK;
  cfile->ratio_fps = FALSE;
  cfile->aseek_pos = 0;
  cfile->unique_id = gen_unique_id();
  cfile->layout_map = NULL;
  cfile->frame_index = cfile->frame_index_back = NULL;
  cfile->fx_frame_pump = 0;
  cfile->pumper = NULL;
  cfile->stored_layout_frame = 0;
  cfile->stored_layout_audio = 0.;
  cfile->stored_layout_fps = 0.;
  cfile->stored_layout_idx = -1;
  cfile->interlace = LIVES_INTERLACE_NONE;
  cfile->subt = NULL;
  cfile->no_proc_sys_errors = cfile->no_proc_read_errors = cfile->no_proc_write_errors = FALSE;
  cfile->keep_without_preview = FALSE;
  cfile->cb_src = -1;
  cfile->needs_update = cfile->needs_silent_update = FALSE;
  cfile->audio_waveform = NULL;
  cfile->md5sum[0] = 0;
  cfile->gamma_type = WEED_GAMMA_SRGB;
  cfile->last_play_sequence = 0;
  cfile->tcache_dubious_from = 0;
  cfile->tcache_height = 0;
  cfile->tcache = NULL;
  cfile->checked = FALSE;
  cfile->has_binfmt = TRUE;

  if (!strcmp(prefs->image_ext, LIVES_FILE_EXT_JPG)) cfile->img_type = IMG_TYPE_JPEG;
  else cfile->img_type = IMG_TYPE_PNG;

  cfile->bpp = (cfile->img_type == IMG_TYPE_JPEG) ? 24 : 32;
  cfile->deinterlace = FALSE;

  cfile->play_paused = FALSE;
  cfile->header_version = LIVES_CLIP_HEADER_VERSION;

  cfile->event_list = cfile->event_list_back = NULL;
  cfile->next_event = NULL;
  cfile->vol = 1.;

  lives_memset(cfile->name, 0, 1);
  lives_memset(cfile->mime_type, 0, 1);
  lives_memset(cfile->file_name, 0, 1);
  lives_memset(cfile->save_file_name, 0, 1);

  lives_memset(cfile->comment, 0, 1);
  lives_memset(cfile->author, 0, 1);
  lives_memset(cfile->title, 0, 1);
  lives_memset(cfile->keywords, 0, 1);

  cfile->signed_endian = AFORM_UNKNOWN;
  lives_snprintf(cfile->undo_text, 32, "%s", _("_Undo"));
  lives_snprintf(cfile->redo_text, 32, "%s", _("_Redo"));

  stfile = lives_build_filename(prefs->workdir, cfile->handle, LIVES_STATUS_FILE_NAME, NULL);

  lives_snprintf(cfile->info_file, PATH_MAX, "%s", stfile);
  lives_free(stfile);

  // backwards compat.
  cfile->checked_for_old_header = FALSE;
  cfile->has_old_header = FALSE;

  return cfile;
}


LIVES_GLOBAL_INLINE char *get_untitled_name(int number) {
  // utility function to get clip name
  return lives_strdup_printf(_("Untitled%d"), number);
}


int create_nullvideo_clip(const char *handle) {
  // create a file with no video, just produces blank frames
  // may be used to playback with audio, for testign etc.
  int new_file;
  int current_file = mainw->current_file;
  create_cfile(-1, handle, TRUE);
  new_file = mainw->current_file;
  mainw->current_file = current_file;
  mainw->files[new_file]->clip_type = CLIP_TYPE_NULL_VIDEO;
  return new_file;
}


boolean get_new_handle(int index, const char *name) {
  // here is where we first initialize for the clipboard
  // and for paste_as_new, and restore, etc.
  // pass in name as NULL or "" and it will be set with an untitled number

  // this function *does not* change mainw->current_file (except briefly), or add to the menu
  // or update mainw->clips_available

  // differences from get_temp_handle:
  // - here we dont't switch clips;
  // - index is normally passed in rather than generated (pulled from next_free_file) - this allows
  //     the caller to know the index number and do preconfig before calling
  // - we set name and file_name from the name parameter, or if name is NULL, we set an untitled name
  //        and increment mainw->untitled_number
  // - the clip should be closed using close_current_file() instead of close_temp_handle()

  char *xname;

  int current_file = mainw->current_file;

  // if TRUE, changes mainw->current_file (and hence cfile)
  if (!get_temp_handle(index)) return FALSE;

  // setup would have been done already in get_temp_handle()
  if (index == -1) index = mainw->current_file;

  else create_cfile(index, cfile->handle, FALSE);

  // note : don't need to update first_free_file for the clipboard
  // because we used index 0 instead of a free index number
  if (index != 0) {
    get_next_free_file();
  }

  if (!name || !*name) {
    cfile->is_untitled = TRUE;
    xname = get_untitled_name(mainw->untitled_number++);
  } else xname = lives_strdup(name);

  lives_snprintf(cfile->file_name, PATH_MAX, "%s", xname);
  lives_snprintf(cfile->name, CLIP_NAME_MAXLEN, "%s", xname);

  mainw->current_file = current_file;

  lives_free(xname);
  return TRUE;
}


boolean add_file_info(const char *check_handle, boolean aud_only) {
  // file information has been retrieved, set struct cfile with details
  // contained in mainw->msg. We do this twice, once before opening the file, once again after.
  // The first time, frames and afilesize may not be correct.
  char *mesg, *mesg1;
  char **array;
  char *test_fps_string1;
  char *test_fps_string2;

  if (aud_only && !mainw->save_with_sound) {
    cfile->arps = cfile->arate = cfile->achans = cfile->asampsize = 0;
    cfile->afilesize = 0l;
    return TRUE;
  }

  if (!strcmp(mainw->msg, "killed")) {
    char *com;
    // user pressed "enough"
    // just in case last frame is damaged, we delete it (physically, otherwise it will get dragged in when the file is opened)
    if (!cfile->ext_src) {
      cfile->frames = get_frame_count(mainw->current_file, cfile->opening_frames);
      if (cfile->frames > 1) {
        com = lives_strdup_printf("%s cut \"%s\" %d %d %d %d \"%s\" %.3f %d %d %d",
                                  prefs->backend, cfile->handle, cfile->frames, cfile->frames,
                                  FALSE, cfile->frames, get_image_ext_for_type(cfile->img_type),
                                  0., 0, 0, 0);
        lives_system(com, FALSE);
        lives_free(com);
        cfile->frames--;
      }
    }

    // commit audio
    mainw->cancelled = CANCEL_NONE;
    lives_rm(cfile->info_file);

    com = lives_strdup_printf("%s commit_audio \"%s\" 1", prefs->backend, cfile->handle);
    lives_system(com, TRUE);
    lives_free(com);

    wait_for_bg_audio_sync(mainw->current_file);

    reget_afilesize(mainw->current_file);
    d_print_enough(cfile->frames);

    if (prefs->auto_trim_audio) {
      if (cfile->laudio_time > cfile->video_time) {
        d_print(_("Auto trimming %.2f seconds of audio at end..."), cfile->laudio_time - cfile->video_time);
        if (on_trim_audio_activate(NULL, LIVES_INT_TO_POINTER(0))) d_print_done();
        else d_print("\n");
        cfile->changed = FALSE;
      }
    }
  } else {
    if (check_handle) {
      int npieces = get_token_count(mainw->msg, '|');
      if (npieces < 2) return FALSE;

      array = lives_strsplit(mainw->msg, "|", npieces);

      if (!strcmp(array[0], "error")) {
        if (npieces >= 3) {
          mesg = lives_strdup_printf(_("\nAn error occurred doing\n%s\n"), array[2]);
          LIVES_ERROR(array[2]);
        } else mesg = (_("\nAn error occurred opening the file\n"));
        widget_opts.non_modal = TRUE;
        do_error_dialog(mesg);
        widget_opts.non_modal = FALSE;
        lives_free(mesg);
        lives_strfreev(array);
        return FALSE;
      }

      // sanity check handle against status file
      // (this should never happen...)
      if (strcmp(check_handle, array[1])) {
        LIVES_ERROR("Handle!=statusfile !");
        mesg = lives_strdup_printf(_("\nError getting file info for clip %s.\nBad things may happen with this clip.\n"),
                                   check_handle);
        widget_opts.non_modal = TRUE;
        do_error_dialog(mesg);
        widget_opts.non_modal = FALSE;
        lives_free(mesg);
        lives_strfreev(array);
        return FALSE;
      }

      cfile->arps = cfile->arate = atoi(array[9]);
      cfile->achans = atoi(array[10]);
      cfile->asampsize = atoi(array[11]);
      cfile->signed_endian = get_signed_endian(atoi(array[12]), atoi(array[13]));
      cfile->afilesize = strtol(array[14], NULL, 10);
      if (aud_only) {
        lives_strfreev(array);
        return TRUE;
      }

      cfile->frames = atoi(array[2]);
      if (aud_only) {
        lives_strfreev(array);
        return TRUE;
      }
      lives_snprintf(cfile->type, 40, "%s", array[3]);
      cfile->hsize = atoi(array[4]);
      cfile->vsize = atoi(array[5]);
      cfile->bpp = atoi(array[6]);
      cfile->pb_fps = cfile->fps = lives_strtod(array[7], NULL);
      cfile->f_size = strtol(array[8], NULL, 10);

      if (npieces > 15 && array[15]) {
        if (prefs->btgamma) {
          if (!strcmp(array[15], "bt709")) cfile->gamma_type = WEED_GAMMA_BT709;
        }
      }

      if (!*cfile->title && npieces > 16 && array[16]) {
        lives_snprintf(cfile->title, 1024, "%s", lives_strstrip(array[16]));
      }
      if (!*cfile->author && npieces > 17 && array[17]) {
        lives_snprintf(cfile->author, 1024, "%s", lives_strstrip(array[17]));
      }
      if (!*cfile->comment && npieces > 18 && array[18]) {
        lives_snprintf(cfile->comment, 1024, "%s", lives_strstrip(array[18]));
      }

      lives_strfreev(array);
    }
  }

  cfile->video_time = 0;

  test_fps_string1 = lives_strdup_printf("%.3f00000", cfile->fps);
  test_fps_string2 = lives_strdup_printf("%.8f", cfile->fps);

  if (strcmp(test_fps_string1, test_fps_string2)) {
    cfile->ratio_fps = TRUE;
  } else {
    cfile->ratio_fps = FALSE;
  }
  lives_free(test_fps_string1);
  lives_free(test_fps_string2);

  if (!mainw->save_with_sound) {
    cfile->arps = cfile->arate = cfile->achans = cfile->asampsize = 0;
    cfile->afilesize = 0l;
  }

  if (cfile->frames <= 0) {
    if (cfile->afilesize == 0l && cfile->is_loaded) {
      // we got no video or audio...
      return FALSE;
    }
    cfile->start = cfile->end = cfile->undo_start = cfile->undo_end = 0;
  } else {
    // start with all selected
    cfile->start = 1;
    cfile->end = cfile->frames;
    cfile->undo_start = cfile->start;
    cfile->undo_end = cfile->end;
  }

  cfile->orig_file_name = TRUE;
  cfile->is_untitled = FALSE;

  // some files give us silly frame rates, even single frames...
  // fps of 1000. is used for some streams (i.e. play each frame as it is received)
  if (cfile->fps == 0. || cfile->fps == 1000. || (cfile->frames < 2 && cfile->is_loaded)) {

    if ((cfile->afilesize * cfile->asampsize * cfile->arate * cfile->achans == 0) || cfile->frames < 2) {
      if (cfile->frames != 1) {
        d_print(_("\nPlayback speed not found or invalid ! Using default fps of %.3f fps. \n"
                  "Default can be set in Tools | Preferences | Misc.\n"),
                prefs->default_fps);
      }
      cfile->pb_fps = cfile->fps = prefs->default_fps;
    } else {
      cfile->laudio_time = cfile->raudio_time = cfile->afilesize / cfile->asampsize * 8. / cfile->arate / cfile->achans;
      cfile->pb_fps = cfile->fps = 1.*(int)(cfile->frames / cfile->laudio_time);
      if (cfile->fps > FPS_MAX || cfile->fps < 1.) {
        cfile->pb_fps = cfile->fps = prefs->default_fps;
      }
      d_print(_("Playback speed was adjusted to %.3f frames per second to fit audio.\n"), cfile->fps);
    }
  }

  cfile->video_time = (double)cfile->frames / cfile->fps;

  if (cfile->opening) return TRUE;

  if ((!strcmp(cfile->type, LIVES_IMAGE_TYPE_JPEG) || !strcmp(cfile->type, LIVES_IMAGE_TYPE_PNG))) {
    mesg = (_("Image format detected"));
    d_print(mesg);
    lives_free(mesg);
    return TRUE;
  }

  if (cfile->bpp == 256) {
    mesg1 = lives_strdup_printf(_("Frames=%d type=%s size=%dx%d *bpp=Greyscale* fps=%.3f\nAudio:"), cfile->frames,
                                cfile->type, cfile->hsize, cfile->vsize, cfile->fps);
  } else {
    if (cfile->bpp != 32) cfile->bpp = 24; // assume RGB24  *** TODO - check
    mesg1 = lives_strdup_printf(_("Frames=%d type=%s size=%dx%d bpp=%d fps=%.3f\nAudio:"), cfile->frames,
                                cfile->type, cfile->hsize, cfile->vsize, cfile->bpp, cfile->fps);
  }

  if (cfile->achans == 0) {
    mesg = lives_strdup_printf(_("%s none\n"), mesg1);
  } else {
    mesg = lives_strdup_printf(P_("%s %d Hz %d channel %d bps\n", "%s %d Hz %d channels %d bps\n", cfile->achans),
                               mesg1, cfile->arate, cfile->achans, cfile->asampsize);
  }
  d_print(mesg);
  lives_free(mesg1);
  lives_free(mesg);

  // get the author,title,comments
  if (*cfile->author) {
    d_print(_(" - Author: %s\n"), cfile->author);
  }
  if (*cfile->title) {
    d_print(_(" - Title: %s\n"), cfile->title);
  }
  if (*cfile->comment) {
    d_print(_(" - Comment: %s\n"), cfile->comment);
  }

  return TRUE;
}


boolean save_file_comments(int fileno) {
  // save the comments etc for smogrify
  int retval;
  int comment_fd;
  char *comment_file = lives_strdup_printf("%s/%s/.comment", prefs->workdir, cfile->handle);
  lives_clip_t *sfile = mainw->files[fileno];

  lives_rm(comment_file);

  do {
    retval = 0;
    comment_fd = creat(comment_file, S_IRUSR | S_IWUSR);
    if (comment_fd < 0) {
      THREADVAR(write_failed) = TRUE;
      retval = do_write_failed_error_s_with_retry(comment_file, lives_strerror(errno));
    } else {
      THREADVAR(write_failed) = FALSE;
      lives_write(comment_fd, sfile->title, strlen(sfile->title), TRUE);
      lives_write(comment_fd, "||%", 3, TRUE);
      lives_write(comment_fd, sfile->author, strlen(sfile->author), TRUE);
      lives_write(comment_fd, "||%", 3, TRUE);
      lives_write(comment_fd, sfile->comment, strlen(sfile->comment), TRUE);

      close(comment_fd);

      if (THREADVAR(write_failed)) {
        retval = do_write_failed_error_s_with_retry(comment_file, NULL);
      }
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  lives_free(comment_file);

  if (THREADVAR(write_failed)) return FALSE;

  return TRUE;
}


void wait_for_stop(const char *stop_command) {
  FILE *infofile;

  // only used for audio player mplayer or audio player sox

# define SECOND_STOP_TIME 0.1
# define STOP_GIVE_UP_TIME 1.0

  double time_waited = 0.;
  boolean sent_second_stop = FALSE;

  // send another stop if necessary
  while (!(infofile = fopen(cfile->info_file, "r"))) {
    lives_widget_process_updates(LIVES_MAIN_WINDOW_WIDGET);
    lives_usleep(prefs->sleep_time);
    time_waited += 1000000. / prefs->sleep_time;
    if (time_waited > SECOND_STOP_TIME && !sent_second_stop) {
      lives_system(stop_command, TRUE);
      sent_second_stop = TRUE;
    }

    if (time_waited > STOP_GIVE_UP_TIME) {
      // give up waiting, but send a last try...
      lives_system(stop_command, TRUE);
      break;
    }
  }
  if (infofile) fclose(infofile);
}


boolean save_frame_inner(int clip, int frame, const char *file_name, int width, int height, boolean from_osc) {
  // save 1 frame as an image
  // width==-1, height==-1 to use "natural" values
  LiVESResponseType resp;
  lives_clip_t *sfile = mainw->files[clip];
  char full_file_name[PATH_MAX];
  char *com, *tmp;

  boolean allow_over = FALSE;

  if (!from_osc && strrchr(file_name, '.') == NULL) {
    lives_snprintf(full_file_name, PATH_MAX, "%s.%s", file_name,
                   get_image_ext_for_type(sfile->img_type));
  } else {
    lives_snprintf(full_file_name, PATH_MAX, "%s", file_name);
    if (!allow_over) allow_over = TRUE;
  }

  // TODO - allow overwriting in sandbox
  if (from_osc && lives_file_test(full_file_name, LIVES_FILE_TEST_EXISTS)) return FALSE;

  tmp = lives_filename_from_utf8(full_file_name, -1, NULL, NULL, NULL);

  if (!mainw->multitrack) {
    d_print(_("Saving frame %d as %s..."), frame, full_file_name);

    if (sfile->clip_type == CLIP_TYPE_FILE) {
      frames_t res = virtual_to_images(clip, frame, frame, FALSE, NULL);
      if (res <= 0) {
        d_print_file_error_failed();
        return FALSE;
      }
    }

    do {
      resp = LIVES_RESPONSE_NONE;

      com = lives_strdup_printf("%s save_frame %s %d \"%s\" %d %d", prefs->backend_sync, sfile->handle,
                                frame, tmp, width, height);
      lives_system(com, FALSE);
      lives_free(com);

      if (THREADVAR(write_failed)) {
        THREADVAR(write_failed) = 0;
        d_print_file_error_failed();
        resp = do_file_perm_error(tmp, TRUE);
        if (resp == LIVES_RESPONSE_CANCEL) {
          lives_free(tmp);
          return FALSE;
        }
      }
      if (!THREADVAR(com_failed)) {
        lives_free(tmp);
        d_print_done();
        return TRUE;
      }
    } while (resp == LIVES_RESPONSE_RETRY);
  } else {
    // multitrack mode
    LiVESError *gerr = NULL;
    LiVESPixbuf *pixbuf;
    int retval;

    mt_show_current_frame(mainw->multitrack, TRUE);
    resize_layer(mainw->frame_layer, sfile->hsize, sfile->vsize, LIVES_INTERP_BEST, WEED_PALETTE_RGB24, 0);
    convert_layer_palette(mainw->frame_layer, WEED_PALETTE_RGB24, 0);
    weed_set_int_value(mainw->frame_layer, WEED_LEAF_GAMMA_TYPE, WEED_GAMMA_SRGB);
    pixbuf = layer_to_pixbuf(mainw->frame_layer, TRUE, FALSE);
    weed_plant_free(mainw->frame_layer);
    mainw->frame_layer = NULL;

    do {
      retval = 0;
      if (sfile->img_type == IMG_TYPE_JPEG) lives_pixbuf_save(pixbuf, tmp, IMG_TYPE_JPEG, 100,
            sfile->hsize, sfile->vsize, &gerr);
      else if (sfile->img_type == IMG_TYPE_PNG) lives_pixbuf_save(pixbuf, tmp, IMG_TYPE_PNG, 100,
            sfile->hsize, sfile->vsize, &gerr);

      if (gerr) {
        retval = do_write_failed_error_s_with_retry(full_file_name, gerr->message);
        lives_error_free(gerr);
        gerr = NULL;
      }
    } while (retval == LIVES_RESPONSE_RETRY);

    free(tmp);
    lives_widget_object_unref(pixbuf);
  }

  // some other error condition
  return FALSE;
}


void backup_file(int clip, int start, int end, const char *file_name) {
  lives_clip_t *sfile = mainw->files[clip];
  char **array;

  char *title;
  char full_file_name[PATH_MAX];

  char *com, *tmp;

  boolean with_perf = FALSE;
  boolean retval, allow_over;

  int withsound = 1;
  int current_file = mainw->current_file;

  if (strrchr(file_name, '.') == NULL) {
    lives_snprintf(full_file_name, PATH_MAX, "%s.%s", file_name, LIVES_FILE_EXT_BACKUP);
    allow_over = FALSE;
  } else {
    lives_snprintf(full_file_name, PATH_MAX, "%s", file_name);
    allow_over = TRUE;
  }

  // check if file exists
  if (!check_file(full_file_name, allow_over)) return;

  // create header files
  retval = write_headers(sfile); // for pre LiVES 0.9.6
  retval = save_clip_values(clip); // new style (0.9.6+)

  if (!retval) return;

  //...and backup
  title = get_menu_name(sfile, FALSE);
  d_print(_("Backing up %s to %s"), title, full_file_name);
  lives_free(title);

  if (!mainw->save_with_sound) {
    d_print(_(" without sound"));
    withsound = 0;
  }

  d_print("...");
  cfile->progress_start = 1;
  cfile->progress_end = sfile->frames;

  if (sfile->clip_type == CLIP_TYPE_FILE) {
    frames_t ret;
    char *msg = (_("Pulling frames from clip..."));
    if ((ret = realize_all_frames(clip, msg, FALSE)) < cfile->frames) {
      lives_free(msg);
      cfile->nopreview = FALSE;
      if (ret > 0) d_print_cancelled();
      return;
    }
    lives_free(msg);
  }

  com = lives_strdup_printf("%s backup %s %d %d %d %s", prefs->backend, sfile->handle, withsound,
                            start, end, (tmp = lives_filename_from_utf8(full_file_name, -1, NULL, NULL, NULL)));
  lives_free(tmp);

  // TODO
  mainw->current_file = clip;

  lives_rm(cfile->info_file);
  cfile->nopreview = TRUE;
  lives_system(com, FALSE);
  lives_free(com);

  if (THREADVAR(com_failed)) {
    THREADVAR(com_failed) = FALSE;
    mainw->current_file = current_file;
    return;
  }

  if (!(do_progress_dialog(TRUE, TRUE, _("Backing up"))) || mainw->error) {
    if (mainw->error) {
      d_print_failed();
    }

    // cancelled - clear up files
    cfile->nopreview = FALSE;

    // using restore details in the 'wrong' way here...it will also clear files
    com = lives_strdup_printf("%s restore_details %s", prefs->backend, cfile->handle);
    lives_popen(com, FALSE, mainw->msg, MAINW_MSG_SIZE);
    lives_free(com);

    //save_clip_values(mainw->current_file);
    mainw->current_file = current_file;
    return;
  }

  cfile->nopreview = FALSE;

  mainw->current_file = current_file;

  if (mainw->error) {
    widget_opts.non_modal = TRUE;
    do_error_dialog(mainw->msg);
    widget_opts.non_modal = FALSE;
    d_print_failed();
    return;
  }

  if (with_perf) {
    d_print(_("performance data was backed up..."));
  }

  array = lives_strsplit(mainw->msg, "|", 3);
  sfile->f_size = strtol(array[1], NULL, 10);
  lives_strfreev(array);

  lives_snprintf(sfile->file_name, PATH_MAX, "%s", full_file_name);
  if (!sfile->was_renamed) {
    lives_snprintf(sfile->name, CLIP_NAME_MAXLEN, "%s", full_file_name);
    set_main_title(cfile->name, 0);
    lives_menu_item_set_text(sfile->menuentry, full_file_name, FALSE);
  }
  if (prefs->show_recent)
    add_to_recent(full_file_name, 0., 0, NULL);

  sfile->changed = FALSE;
  // set is_untitled to stop users from saving with a .lv1 extension
  sfile->is_untitled = TRUE;
  d_print_done();
}


boolean write_headers(lives_clip_t *file) {
  // this function is included only for backwards compatibility with ancient builds of LiVES
  //

  int retval;
  int header_fd;
  char *hdrfile;

  // save the file details
  hdrfile = lives_build_filename(prefs->workdir, file->handle, LIVES_CLIP_HEADER_OLD, NULL);

  do {
    retval = 0;
    header_fd = creat(hdrfile, S_IRUSR | S_IWUSR);
    if (header_fd < 0) {
      retval = do_write_failed_error_s_with_retry(hdrfile, lives_strerror(errno));
    } else {
      THREADVAR(write_failed) = FALSE;

      lives_write_le(header_fd, &cfile->bpp, 4, TRUE);
      lives_write_le(header_fd, &cfile->fps, 8, TRUE);
      lives_write_le(header_fd, &cfile->hsize, 4, TRUE);
      lives_write_le(header_fd, &cfile->vsize, 4, TRUE);
      lives_write_le(header_fd, &cfile->arps, 4, TRUE);
      lives_write_le(header_fd, &cfile->signed_endian, 4, TRUE);
      lives_write_le(header_fd, &cfile->arate, 4, TRUE);
      lives_write_le(header_fd, &cfile->unique_id, 8, TRUE);
      lives_write_le(header_fd, &cfile->achans, 4, TRUE);
      lives_write_le(header_fd, &cfile->asampsize, 4, TRUE);

      lives_write(header_fd, LiVES_VERSION, strlen(LiVES_VERSION), TRUE);
      close(header_fd);

      if (THREADVAR(write_failed)) retval = do_write_failed_error_s_with_retry(hdrfile, NULL);
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  lives_free(hdrfile);

  if (retval != LIVES_RESPONSE_CANCEL) {
    // more file details (since version 0.7.5)
    hdrfile = lives_build_filename(prefs->workdir, file->handle, LIVES_CLIP_HEADER_OLD2, NULL);

    do {
      retval = 0;
      header_fd = creat(hdrfile, S_IRUSR | S_IWUSR);

      if (header_fd < 0) {
        retval = do_write_failed_error_s_with_retry(hdrfile, lives_strerror(errno));
      } else {
        THREADVAR(write_failed) = FALSE;
        lives_write_le(header_fd, &file->frames, 4, TRUE);
        lives_write(header_fd, &file->title, 1024, TRUE);
        lives_write(header_fd, &file->author, 1024, TRUE);
        lives_write(header_fd, &file->comment, 1024, TRUE);
        close(header_fd);
      }
      if (THREADVAR(write_failed)) retval = do_write_failed_error_s_with_retry(hdrfile, NULL);
    } while (retval == LIVES_RESPONSE_RETRY);

    lives_free(hdrfile);
  }

  if (retval == LIVES_RESPONSE_CANCEL) {
    THREADVAR(write_failed) = FALSE;
    return FALSE;
  }
  return TRUE;
}


boolean read_headers(int fileno, const char *dir, const char *file_name) {
  // file_name is only used to get the file size on the disk
  lives_clip_t *sfile;
  char **array;
  char buff[1024];
  char version[32];
  char *com, *tmp;
  char *old_hdrfile, *lives_header = NULL;

  off_t header_size;
  int version_hash;
  int pieces;
  int header_fd;
  int retval2;
  int asigned = 0, aendian = LIVES_LITTLE_ENDIAN;

  lives_clip_details_t detail;

  boolean retval, retvala;
  boolean is_ascrap = FALSE;

  off_t sizhead = 28; //8 * 4 + 8 + 8;

  time_t old_time = 0, new_time = 1;
  struct stat mystat;

  if (!IS_VALID_CLIP(fileno)) return FALSE;

  if (mainw->hdrs_cache) cached_list_free(&mainw->hdrs_cache);

  sfile = mainw->files[fileno];

  old_hdrfile = lives_build_filename(dir, LIVES_CLIP_HEADER_OLD, NULL);

  if (fileno == mainw->ascrap_file) {
    is_ascrap = TRUE;
    /// ascrap_file now uses a different header name; this is to facilitate diskspace cleanup
    /// otherwise it may be wrongly classified as a recoverable clip
    lives_header = lives_build_filename(dir, LIVES_ACLIP_HEADER, NULL);
    if (!lives_file_test(lives_header, LIVES_FILE_TEST_EXISTS)) {
      lives_free(lives_header);
      lives_header = NULL;
    }
  }
  if (!lives_header) lives_header = lives_build_filename(dir, LIVES_CLIP_HEADER, NULL);

  sfile->checked_for_old_header = TRUE;
  sfile->img_type = IMG_TYPE_UNKNOWN;

  if (lives_file_test(lives_header, LIVES_FILE_TEST_EXISTS)) {
    do {
      retval2 = LIVES_RESPONSE_OK;
      if (!(mainw->hdrs_cache = cache_file_contents(lives_header))) {
        if (fileno != mainw->current_file) goto rhd_failed;
        retval2 = do_read_failed_error_s_with_retry(lives_header, NULL);
      }
    } while (retval2 == LIVES_RESPONSE_RETRY);

    if (retval2 == LIVES_RESPONSE_CANCEL) {
      goto rhd_failed;
    }

    if (fileno == mainw->current_file) {
      threaded_dialog_spin(0.);
    }

    if (!is_ascrap) restore_clip_binfmt(fileno);

    do {
      do {
        detail = CLIP_DETAILS_HEADER_VERSION;
        retval = get_clip_value(fileno, detail, &sfile->header_version, 16);
        if (retval) {
          if (sfile->header_version < 100) goto old_check;
        } else {
          if (lives_file_test(old_hdrfile, LIVES_FILE_TEST_EXISTS)) {
            goto old_check;
          }
          if (fileno != mainw->current_file) {
            goto rhd_failed;
          }
          if (mainw->hdrs_cache) {
            retval2 = do_header_missing_detail_error(fileno, CLIP_DETAILS_HEADER_VERSION);
          } else {
            retval2 = do_header_read_error_with_retry(fileno);
          }
        }
      } while (retval2 == LIVES_RESPONSE_RETRY);

      if (retval2 == LIVES_RESPONSE_CANCEL) goto rhd_failed;

      if (is_ascrap) goto get_avals;

      detail = CLIP_DETAILS_FRAMES;
      retval = get_clip_value(fileno, detail, &sfile->frames, 0);

      if (retval) {
        detail = CLIP_DETAILS_BPP;
        retval = get_clip_value(fileno, detail, &sfile->bpp, 0);
      }
      if (retval) {
        detail = CLIP_DETAILS_FPS;
        retval = get_clip_value(fileno, detail, &sfile->fps, 0);
      }
      if (retval) {
        detail = CLIP_DETAILS_PB_FPS;
        retval = get_clip_value(fileno, detail, &sfile->pb_fps, 0);
        if (!retval) {
          retval = TRUE;
          sfile->pb_fps = sfile->fps;
        }
      }
      if (retval) {
        retval = get_clip_value(fileno, CLIP_DETAILS_PB_FRAMENO, &sfile->frameno, 0);
        if (!retval) {
          retval = TRUE;
          sfile->frameno = 1;
        }
        if (sfile->frameno <= 0) sfile->frameno = 1;
      }
      if (retval) {
        detail = CLIP_DETAILS_WIDTH;
        retval = get_clip_value(fileno, detail, &sfile->hsize, 0);
      }
      if (retval) {
        detail = CLIP_DETAILS_HEIGHT;
        retval = get_clip_value(fileno, detail, &sfile->vsize, 0);
      }
      if (retval) {
        if (sfile->header_version > 100) {
          detail = CLIP_DETAILS_GAMMA_TYPE;
          get_clip_value(fileno, detail, &sfile->gamma_type, 0);
          if (sfile->gamma_type == 0) sfile->gamma_type = WEED_GAMMA_SRGB;
          if (sfile->gamma_type != WEED_GAMMA_SRGB) {
            if (!do_gamma_import_warn(sfile->has_binfmt ?
                                      sfile->binfmt_version.num : 0, sfile->gamma_type)) goto rhd_failed;
          }
        }
      }
      if (retval) {
        detail = CLIP_DETAILS_CLIPNAME;
        get_clip_value(fileno, detail, sfile->name, CLIP_NAME_MAXLEN);
      }
      if (retval) {
        detail = CLIP_DETAILS_FILENAME;
        get_clip_value(fileno, detail, sfile->file_name, PATH_MAX);
      }

get_avals:
      if (retval) {
        detail = CLIP_DETAILS_ACHANS;
        retvala = get_clip_value(fileno, detail, &sfile->achans, 0);
        if (!retvala) sfile->achans = 0;
      }

      if (sfile->achans == 0) retvala = FALSE;
      else retvala = TRUE;

      if (retval && retvala) {
        detail = CLIP_DETAILS_ARATE;
        retvala = get_clip_value(fileno, detail, &sfile->arps, 0);
      }

      if (!retvala) sfile->arps = sfile->achans = sfile->arate = sfile->asampsize = 0;
      if (sfile->arps == 0) retvala = FALSE;

      if (retvala && retval) {
        detail = CLIP_DETAILS_PB_ARATE;
        retvala = get_clip_value(fileno, detail, &sfile->arate, 0);
        if (!retvala) {
          retvala = TRUE;
          sfile->arate = sfile->arps;
        }
      }
      if (retvala && retval) {
        detail = CLIP_DETAILS_ASIGNED;
        retval = get_clip_value(fileno, detail, &asigned, 0);
      }
      if (retvala && retval) {
        detail = CLIP_DETAILS_AENDIAN;
        retval = get_clip_value(fileno, detail, &aendian, 0);
      }

      sfile->signed_endian = asigned + aendian;

      if (retvala && retval) {
        detail = CLIP_DETAILS_ASAMPS;
        retval = get_clip_value(fileno, detail, &sfile->asampsize, 0);
      }
      if (!retval) {
        if (fileno != mainw->current_file) goto rhd_failed;
        if (mainw->hdrs_cache) {
          retval2 = do_header_missing_detail_error(fileno, detail);
        } else {
          retval2 = do_header_read_error_with_retry(fileno);
        }
      } else {
        if (!is_ascrap) {
          get_clip_value(fileno, CLIP_DETAILS_TITLE, sfile->title, 1024);
          get_clip_value(fileno, CLIP_DETAILS_AUTHOR, sfile->author, 1024);
          get_clip_value(fileno, CLIP_DETAILS_COMMENT, sfile->comment, 1024);
          get_clip_value(fileno, CLIP_DETAILS_KEYWORDS, sfile->keywords, 1024);
          get_clip_value(fileno, CLIP_DETAILS_INTERLACE, &sfile->interlace, 0);
          // user must have selected this:
          if (sfile->interlace != LIVES_INTERLACE_NONE) sfile->deinterlace = TRUE;
        }
        lives_free(old_hdrfile);
        lives_free(lives_header);
        if (!prefs->vj_mode) {
          sfile->afilesize = reget_afilesize_inner(fileno);
        }
        /// need to maintain mainw->hdrs_cache in this case, as it may be
        // passed to further functions, but it needs to be freed and set to NULL
        // at some point
        return TRUE;
      }
    } while (retval2 == LIVES_RESPONSE_RETRY);
    goto rhd_failed;
  }

old_check:

  if (lives_file_test(old_hdrfile, LIVES_FILE_TEST_EXISTS)) {
    sfile->has_old_header = TRUE;
    if (!stat(old_hdrfile, &mystat)) old_time = mystat.st_mtime;
    if (!stat(lives_header, &mystat)) new_time = mystat.st_mtime;
  }

  lives_free(lives_header);
  lives_header = NULL;
  ///////////////

  if (sfile->has_old_header && old_time <= new_time) {
    retval2 = LIVES_RESPONSE_OK;
    detail = CLIP_DETAILS_FRAMES;

    if (get_clip_value(fileno, detail, &sfile->frames, 0)) {
      char *tmp;

      // use new style header (LiVES 0.9.6+)
      // clean up and get file sizes
      if (file_name) {
        com = lives_strdup_printf("%s restore_details \"%s\" \"%s\" 0",
                                  prefs->backend_sync, sfile->handle,
                                  (tmp = lives_filename_from_utf8(file_name, -1, NULL, NULL, NULL)));
        lives_free(tmp);
      } else
        com = lives_strdup_printf("%s restore_details \"%s\" . 1", prefs->backend_sync, sfile->handle);

      lives_popen(com, fileno != mainw->current_file, buff, 1024);
      lives_free(com);

      if (THREADVAR(com_failed)) {
        THREADVAR(com_failed) = FALSE;
        goto rhd_failed;
      }

      pieces = get_token_count(buff, '|');

      if (pieces > 3) {
        array = lives_strsplit(buff, "|", pieces);
        sfile->f_size = strtol(array[1], NULL, 10);
        sfile->afilesize = strtol(array[2], NULL, 10);
        if (sfile->clip_type == CLIP_TYPE_DISK) {
          if (!strcmp(array[3], LIVES_FILE_EXT_JPG)) sfile->img_type = IMG_TYPE_JPEG;
          else sfile->img_type = IMG_TYPE_PNG;
        }
        lives_strfreev(array);
      }
      if (fileno == mainw->current_file) threaded_dialog_spin(0.);
    } else goto rhd_failed;
    lives_free(old_hdrfile);
    /// mainw->hdrs_cache never set
    return TRUE;
  }

  do {
    // old style headers (pre 0.9.6)
    retval = LIVES_RESPONSE_OK;
    THREADVAR(read_failed) = FALSE;
    lives_memset(version, 0, 32);
    lives_memset(buff, 0, 1024);

    header_fd = lives_open2(old_hdrfile, O_RDONLY);

    if (header_fd < 0) {
      if (fileno != mainw->current_file) {
        goto rhd_failed;
      }
      retval = do_read_failed_error_s_with_retry(old_hdrfile, lives_strerror(errno));
    } else {
      THREADVAR(read_failed) = FALSE;
      header_size = get_file_size(header_fd);

      if (header_size < sizhead) {
        close(header_fd);
        goto rhd_failed;
      } else {
        THREADVAR(read_failed) = FALSE;
        lives_read_le(header_fd, &sfile->fps, 4, FALSE);
        if (!THREADVAR(read_failed))
          lives_read_le(header_fd, &sfile->bpp, 8, FALSE);
        if (!THREADVAR(read_failed))
          lives_read_le(header_fd, &sfile->hsize, 4, FALSE);
        if (!THREADVAR(read_failed))
          lives_read_le(header_fd, &sfile->vsize, 4, FALSE);
        if (!THREADVAR(read_failed))
          lives_read_le(header_fd, &sfile->arps, 4, FALSE);
        if (!THREADVAR(read_failed))
          lives_read_le(header_fd, &sfile->signed_endian, 4, FALSE);
        if (!THREADVAR(read_failed))
          lives_read_le(header_fd, &sfile->arate, 4, FALSE);
        if (!THREADVAR(read_failed))
          lives_read_le(header_fd, &sfile->unique_id, 8, FALSE);
        if (!THREADVAR(read_failed))
          lives_read_le(header_fd, &sfile->achans, 4, FALSE);
        if (!THREADVAR(read_failed))
          lives_read_le(header_fd, &sfile->asampsize, 4, FALSE);

        if (header_size > sizhead) {
          if (header_size - sizhead > 31) {
            if (!THREADVAR(read_failed))
              lives_read(header_fd, &version, 31, FALSE);
            version[31] = '\0';
          } else {
            if (!THREADVAR(read_failed))
              lives_read(header_fd, &version, header_size - sizhead, FALSE);
            version[header_size - sizhead] = '\0';
          }
        }
      }
      close(header_fd);
    }

    if (THREADVAR(read_failed)) {
      if (fileno != mainw->current_file) goto rhd_failed;
      retval = do_read_failed_error_s_with_retry(old_hdrfile, NULL);
      if (retval == LIVES_RESPONSE_CANCEL) goto rhd_failed;
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  lives_freep((void **)&old_hdrfile);

  if (retval == LIVES_RESPONSE_CANCEL) goto rhd_failed;

  // handle version changes
  version_hash = verhash(version);
  if (version_hash < 7001) {
    sfile->arps = sfile->arate;
    sfile->signed_endian = mainw->endian;
  }

  com = lives_strdup_printf("%s restore_details %s %s %d", prefs->backend_sync, sfile->handle,
                            (tmp = lives_filename_from_utf8(file_name, -1, NULL, NULL, NULL)),
                            !strcmp(file_name, "."));

  lives_popen(com, FALSE, buff, 1024);
  lives_free(com);
  lives_free(tmp);

  if (THREADVAR(com_failed)) {
    THREADVAR(com_failed) = FALSE;
    goto rhd_failed;
  }

  pieces = get_token_count(buff, '|');
  array = lives_strsplit(buff, "|", pieces);
  sfile->f_size = strtol(array[1], NULL, 10);
  sfile->afilesize = strtol(array[2], NULL, 10);

  if (sfile->clip_type == CLIP_TYPE_DISK) {
    if (!strcmp(array[3], LIVES_FILE_EXT_JPG)) sfile->img_type = IMG_TYPE_JPEG;
    else sfile->img_type = IMG_TYPE_PNG;
  }

  sfile->frames = atoi(array[4]);

  sfile->bpp = (sfile->img_type == IMG_TYPE_JPEG) ? 24 : 32;

  if (pieces > 4 && array[5]) {
    lives_snprintf(sfile->title, 1024, "%s", lives_strstrip(array[4]));
  }
  if (pieces > 5 && array[6]) {
    lives_snprintf(sfile->author, 1024, "%s", lives_strstrip(array[5]));
  }
  if (pieces > 6 && array[7]) {
    lives_snprintf(sfile->comment, 1024, "%s", lives_strstrip(array[6]));
  }

  lives_strfreev(array);
  return TRUE;

rhd_failed:
  lives_freep((void **)&lives_header);
  lives_freep((void **)&old_hdrfile);
  return FALSE;
}


void open_set_file(int clipnum) {
  char name[CLIP_NAME_MAXLEN];

  if (mainw->current_file < 1) return;

  lives_memset(name, 0, CLIP_NAME_MAXLEN);

  if (mainw->hdrs_cache) {
    boolean retval;
    // LiVES 0.9.6+

    retval = get_clip_value(mainw->current_file, CLIP_DETAILS_PB_FPS, &cfile->pb_fps, 0);
    if (!retval) {
      cfile->pb_fps = cfile->fps;
    }
    retval = get_clip_value(mainw->current_file, CLIP_DETAILS_PB_FRAMENO, &cfile->frameno, 0);
    if (!retval) {
      cfile->frameno = 1;
    }

    retval = get_clip_value(mainw->current_file, CLIP_DETAILS_CLIPNAME, name, CLIP_NAME_MAXLEN);
    if (!retval) {
      char *tmp;
      lives_snprintf(name, CLIP_NAME_MAXLEN, "%s", (tmp = get_untitled_name(mainw->untitled_number++)));
      lives_free(tmp);
      cfile->needs_update = TRUE;
    }
    retval = get_clip_value(mainw->current_file, CLIP_DETAILS_UNIQUE_ID, &cfile->unique_id, 0);
    if (!retval) {
      cfile->unique_id = gen_unique_id();
      cfile->needs_silent_update = TRUE;
    }
    retval = get_clip_value(mainw->current_file, CLIP_DETAILS_INTERLACE, &cfile->interlace, 0);
    if (!retval) {
      cfile->interlace = LIVES_INTERLACE_NONE;
      cfile->needs_silent_update = TRUE;
    }
    if (cfile->interlace != LIVES_INTERLACE_NONE) cfile->deinterlace = TRUE;
  } else {
    // pre 0.9.6 <- ancient code
    ssize_t nlen;
    int set_fd;
    int pb_fps;
    int retval;
    char *setfile = lives_strdup_printf("%s/%s/set.%s", prefs->workdir, cfile->handle, mainw->set_name);

    do {
      retval = 0;
      if ((set_fd = lives_open2(setfile, O_RDONLY)) > -1) {
        // get perf_start
        if ((nlen = lives_read_le(set_fd, &pb_fps, 4, TRUE)) > 0) {
          cfile->pb_fps = pb_fps / 1000.;
          lives_read_le(set_fd, &cfile->frameno, 4, TRUE);
          lives_read(set_fd, name, CLIP_NAME_MAXLEN, TRUE);
        }
        close(set_fd);
      } else retval = do_read_failed_error_s_with_retry(setfile, lives_strerror(errno));
    } while (retval == LIVES_RESPONSE_RETRY);

    lives_free(setfile);
    cfile->needs_silent_update = TRUE;
  }

  if (!*name) {
    lives_snprintf(name, CLIP_NAME_MAXLEN, "set_clip %.3d", clipnum);
  } else {
    // pre 3.x, files erroneously had the set name appended permanently, so here we undo that
    if (lives_string_ends_with(name, " (%s)", mainw->set_name)) {
      char *remove = lives_strdup_printf(" (%s)", mainw->set_name);
      if (strlen(name) > strlen(remove)) name[strlen(name) - strlen(remove)] = 0;
      lives_free(remove);
      cfile->needs_silent_update = TRUE;
    }
    lives_snprintf(cfile->name, CLIP_NAME_MAXLEN, "%s", name);
  }
}


void reload_subs(int fileno) {
  lives_clip_t *sfile;
  char *subfname;
  if (!IS_VALID_CLIP(fileno)) return;

  sfile = mainw->files[fileno];
  subfname = lives_build_filename(prefs->workdir, sfile->handle, SUBS_FILENAME "."
                                  LIVES_FILE_EXT_SRT, NULL);
  if (lives_file_test(subfname, LIVES_FILE_TEST_EXISTS)) {
    subtitles_init(sfile, subfname, SUBTITLE_TYPE_SRT);
  } else {
    lives_free(subfname);
    subfname = lives_build_filename(prefs->workdir, sfile->handle, SUBS_FILENAME "."
                                    LIVES_FILE_EXT_SUB, NULL);
    if (lives_file_test(subfname, LIVES_FILE_TEST_EXISTS)) {
      subtitles_init(sfile, subfname, SUBTITLE_TYPE_SUB);
    }
  }
  lives_free(subfname);
}


ulong restore_file(const char *file_name) {
  char *com = lives_strdup("dummy");
  char *mesg, *mesg1, *tmp;
  boolean is_OK = TRUE;
  char *fname = lives_strdup(file_name);
  char *clipdir;

  int old_file = mainw->current_file, current_file;
  int new_file = mainw->first_free_file;
  boolean not_cancelled;

  // create a new file
  if (!get_new_handle(new_file, fname)) {
    return 0;
  }

  d_print(_("Restoring %s..."), file_name);

  mainw->current_file = new_file;

  cfile->hsize = mainw->def_width;
  cfile->vsize = mainw->def_height;

  if (!mainw->multitrack) {
    switch_to_file((mainw->current_file = old_file), new_file);
    set_main_title(cfile->file_name, 0);
  }

  com = lives_strdup_printf("%s restore %s %s", prefs->backend, cfile->handle,
                            (tmp = lives_filename_from_utf8(file_name, -1, NULL, NULL, NULL)));

  lives_rm(cfile->info_file);
  lives_system(com, FALSE);
  lives_free(tmp);
  lives_free(com);

  if (THREADVAR(com_failed)) {
    THREADVAR(com_failed) = FALSE;
    close_current_file(old_file);
    return 0;
  }

  cfile->restoring = TRUE;
  not_cancelled = do_progress_dialog(TRUE, TRUE, _("Restoring"));
  cfile->restoring = FALSE;

  if (mainw->error || !not_cancelled) {
    if (mainw->error && mainw->cancelled != CANCEL_ERROR) {
      do_error_dialog(mainw->msg);
    }
    close_current_file(old_file);
    return 0;
  }

  // call function to return rest of file details
  // fsize, afilesize and frames
  clipdir = lives_build_path(prefs->workdir, cfile->handle, NULL);
  is_OK = read_headers(mainw->current_file, clipdir, file_name);
  lives_free(clipdir);
  if (mainw->hdrs_cache) cached_list_free(&mainw->hdrs_cache);

  if (!is_OK) {
    mesg = lives_strdup_printf(_("\n\nThe file %s is corrupt.\nLiVES was unable to restore it.\n"),
                               file_name);
    do_error_dialog(mesg);
    lives_free(mesg);

    d_print_failed();
    close_current_file(old_file);
    return 0;
  }

  // get img_type, check frame count and size
  if (!cfile->checked && !check_clip_integrity(mainw->current_file, NULL, cfile->frames)) {
    if (cfile->afilesize == 0) {
      reget_afilesize_inner(mainw->current_file);
    }
    if (!check_frame_count(mainw->current_file, FALSE)) {
      cfile->frames = get_frame_count(mainw->current_file, 1);
    }
  }
  cfile->checked = TRUE;

  // add entry to window menu
  // TODO - do this earlier and allow switching during restore
  add_to_clipmenu();

  if (prefs->show_recent) {
    add_to_recent(file_name, 0., 0, NULL);
  }

  if (cfile->frames > 0) {
    cfile->start = 1;
  } else {
    cfile->start = 0;
  }
  cfile->end = cfile->frames;
  cfile->arps = cfile->arate;
  cfile->pb_fps = cfile->fps;
  cfile->opening = FALSE;
  cfile->changed = FALSE;

  if (prefs->autoload_subs) {
    reload_subs(mainw->current_file);
  }

  lives_snprintf(cfile->type, 40, "Frames");
  mesg1 = lives_strdup_printf(_("Frames=%d type=%s size=%dx%d bpp=%d fps=%.3f\nAudio:"), cfile->frames, cfile->type,
                              cfile->hsize, cfile->vsize, cfile->bpp, cfile->fps);

  if (cfile->afilesize == 0l) {
    cfile->achans = 0;
    mesg = lives_strdup_printf(_("%s none\n"), mesg1);
  } else {
    mesg = lives_strdup_printf(P_("%s %d Hz %d channel %d bps\n", "%s %d Hz %d channels %d bps\n", cfile->achans),
                               mesg1, cfile->arate, cfile->achans, cfile->asampsize);
  }
  d_print(mesg);
  lives_free(mesg);
  lives_free(mesg1);

  cfile->is_loaded = TRUE;
  current_file = mainw->current_file;

  // set new bpp
  cfile->bpp = (cfile->img_type == IMG_TYPE_JPEG) ? 24 : 32;

  cfile->saved_frameno = cfile->frameno;
  if (cfile->frameno > cfile->frames && cfile->frameno > 1) cfile->frameno = cfile->frames;
  cfile->last_frameno = cfile->frameno;
  cfile->pointer_time = cfile->real_pointer_time = calc_time_from_frame(mainw->current_file, cfile->frameno);
  if (cfile->real_pointer_time > CLIP_TOTAL_TIME(mainw->current_file))
    cfile->real_pointer_time = CLIP_TOTAL_TIME(mainw->current_file);
  if (cfile->pointer_time > cfile->video_time) cfile->pointer_time = 0.;

  if (cfile->achans) {
    cfile->aseek_pos = (off64_t)((double)(cfile->real_pointer_time * cfile->arate) * cfile->achans *
                                 (cfile->asampsize / 8));
    if (cfile->aseek_pos > cfile->afilesize) cfile->aseek_pos = 0.;
  }

  if (!save_clip_values(current_file)) {
    close_current_file(old_file);
    return 0;
  }

  if (prefs->crash_recovery) add_to_recovery_file(cfile->handle);

  if (!mainw->multitrack) {
    switch_to_file((mainw->current_file = old_file), current_file);
  }
  lives_notify(LIVES_OSC_NOTIFY_CLIP_OPENED, "");

  return cfile->unique_id;
}


int save_event_frames(void) {
  // when doing a resample, we save a list of frames for the back end to do
  // a reorder

  // here we also update the frame_index for clips of type CLIP_TYPE_FILE

  char *hdrfile = lives_strdup_printf("%s/%s/event.frames", prefs->workdir, cfile->handle);

  int header_fd, i = 0;
  int retval;
  int perf_start, perf_end;
  int nevents;

  if (!cfile->event_list) {
    lives_rm(hdrfile);
    return -1;
  }

  perf_start = (int)(cfile->fps * event_list_get_start_secs(cfile->event_list)) + 1;
  perf_end = perf_start + (nevents = count_events(cfile->event_list, FALSE, 0, 0)) - 1;

  if (!event_list_to_block(cfile->event_list, nevents)) return -1;

  if (cfile->frame_index) {
    LiVESResponseType response;
    int xframes = cfile->frames;
    char *what = (_("creating the frame index for resampling "));

    if (cfile->frame_index_back) lives_free(cfile->frame_index_back);
    cfile->frame_index_back = cfile->frame_index;
    cfile->frame_index = NULL;

    do {
      response = LIVES_RESPONSE_OK;
      create_frame_index(mainw->current_file, FALSE, 0, nevents);
      if (!cfile->frame_index) {
        response = do_memory_error_dialog(what, nevents * 4);
      }
    } while (response == LIVES_RESPONSE_RETRY);
    lives_free(what);
    if (response == LIVES_RESPONSE_CANCEL) {
      cfile->frame_index = cfile->frame_index_back;
      cfile->frame_index_back = NULL;
      return -1;
    }

    for (i = 0; i < nevents; i++) {
      cfile->frame_index[i] = cfile->frame_index_back[(cfile->resample_events + i)->value - 1];
    }

    cfile->frames = nevents;
    if (!check_if_non_virtual(mainw->current_file, 1, cfile->frames)) save_frame_index(mainw->current_file);
    cfile->frames = xframes;
  }

  do {
    retval = 0;
    header_fd = creat(hdrfile, S_IRUSR | S_IWUSR);
    if (header_fd < 0) {
      retval = do_write_failed_error_s_with_retry(hdrfile, lives_strerror(errno));
    } else {
      // use machine endian.
      // When we call "smogrify reorder", we will pass the endianness as 3rd parameter

      THREADVAR(write_failed) = FALSE;
      lives_write(header_fd, &perf_start, 4, FALSE);

      if (cfile->resample_events) {
        for (i = 0; i <= perf_end - perf_start; i++) {
          if (THREADVAR(write_failed)) break;
          lives_write(header_fd, &((cfile->resample_events + i)->value), 4, TRUE);
        }
        lives_freep((void **)&cfile->resample_events);
      }

      if (THREADVAR(write_failed)) {
        retval = do_write_failed_error_s_with_retry(hdrfile, NULL);
      }

      close(header_fd);
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  if (retval == LIVES_RESPONSE_CANCEL) {
    i = -1;
  }

  lives_free(hdrfile);
  return i;
}


/////////////////////////////////////////////////
/// scrap file
///  the scrap file is used during recording to dump any streamed (non-disk) clips to
/// during render/preview we load frames from the scrap file, but only as necessary

/// ascrap file
/// this is used to record external audio during playback with record on (if the user requests this)
/// afterwards the audio from it can be rendered/played back

static double ascrap_mb;  // MB written to audio file
static uint64_t free_mb; // MB free to write

void add_to_ascrap_mb(uint64_t bytes) {
  ascrap_mb += bytes / 1000000.;
}


boolean open_scrap_file(void) {
  // create a scrap file for recording generated video frames
  int current_file = mainw->current_file;
  char *dir;
  char *handle, *scrap_handle;

  if (!check_for_executable(&capable->has_mktemp, EXEC_MKTEMP)) {
    do_program_not_found_error(EXEC_MKTEMP);
    return FALSE;
  }

  handle = get_worktmp("_scrap");
  if (!handle) {
    workdir_warning();
    return FALSE;
  }
  if (!create_cfile(-1, handle, FALSE)) {
    dir = lives_build_path(prefs->workdir, cfile->handle, NULL);
    lives_rmdir(dir, FALSE);
    lives_free(dir); lives_free(handle);
    return FALSE;
  }
  lives_free(handle);

  mainw->scrap_file = mainw->current_file;

  lives_snprintf(cfile->type, 40, "scrap");

  scrap_handle = lives_strdup_printf("scrap|%s", cfile->handle);
  if (prefs->crash_recovery) add_to_recovery_file(scrap_handle);
  lives_free(scrap_handle);

  pthread_mutex_lock(&mainw->clip_list_mutex);
  mainw->cliplist = lives_list_append(mainw->cliplist, LIVES_INT_TO_POINTER(mainw->current_file));
  pthread_mutex_unlock(&mainw->clip_list_mutex);

  dir = lives_build_path(prefs->workdir, cfile->handle, NULL);
  free_mb = (double)get_ds_free(dir) / (double)ONE_MILLION;
  lives_free(dir);

  mainw->current_file = current_file;

  if (mainw->ascrap_file == -1) ascrap_mb = 0.;

  return TRUE;
}


boolean open_ascrap_file(void) {
  // create a scrap file for recording audio
  int current_file = mainw->current_file;
  char *dir;
  char *handle, *ascrap_handle;

  if (!check_for_executable(&capable->has_mktemp, EXEC_MKTEMP)) {
    do_program_not_found_error(EXEC_MKTEMP);
    return FALSE;
  }

  handle = get_worktmp("_ascrap");
  if (!handle) {
    workdir_warning();
    return FALSE;
  }
  if (!create_cfile(-1, handle, FALSE)) {
    dir = lives_build_path(prefs->workdir, cfile->handle, NULL);
    lives_rmdir(dir, FALSE);
    lives_free(dir); lives_free(handle);
    return FALSE;
  }
  lives_free(handle);

  mainw->ascrap_file = mainw->current_file;
  lives_snprintf(cfile->type, 40, "ascrap");

  cfile->opening = FALSE;

  cfile->achans = 2;
  cfile->arate = cfile->arps = DEFAULT_AUDIO_RATE;
  cfile->asampsize = 16;
  cfile->signed_endian = 0; // ???

#ifdef HAVE_PULSE_AUDIO
  if (prefs->audio_player == AUD_PLAYER_PULSE) {
    if (prefs->audio_src == AUDIO_SRC_EXT) {
      if (mainw->pulsed_read) {
        cfile->arate = cfile->arps = mainw->pulsed_read->in_arate;
      }
    } else {
      if (mainw->pulsed) {
        cfile->arate = cfile->arps = mainw->pulsed->out_arate;
      }
    }
  }
#endif

#ifdef ENABLE_JACK
  if (prefs->audio_player == AUD_PLAYER_JACK) {
    if (prefs->audio_src == AUDIO_SRC_EXT) {
      if (mainw->jackd_read) {
        cfile->arate = cfile->arps = mainw->jackd_read->sample_in_rate;
      }
    } else {
      if (mainw->jackd) {
        cfile->arate = cfile->arps = mainw->jackd->sample_out_rate;
      }
    }
  }
#endif

  ascrap_handle = lives_strdup_printf("ascrap|%s", cfile->handle);
  if (prefs->crash_recovery) add_to_recovery_file(ascrap_handle);
  lives_free(ascrap_handle);

  pthread_mutex_lock(&mainw->clip_list_mutex);
  mainw->cliplist = lives_list_append(mainw->cliplist, LIVES_INT_TO_POINTER(mainw->current_file));
  pthread_mutex_unlock(&mainw->clip_list_mutex);

  dir = lives_build_path(prefs->workdir, cfile->handle, NULL);
  free_mb = (double)get_ds_free(dir) / (double)ONE_MILLION;
  lives_free(dir);

  mainw->current_file = current_file;

  ascrap_mb = 0.;

  return TRUE;
}


boolean load_from_scrap_file(weed_layer_t *layer, int frame) {
  // load raw frame data from scrap file

  // this will also set cfile width and height - for letterboxing etc.

  // return FALSE if the frame does not exist/we are unable to read it

  char *oname;

  lives_clip_t *scrapfile = mainw->files[mainw->scrap_file];

  int fd;
  if (!IS_VALID_CLIP(mainw->scrap_file)) return FALSE;

  if (!scrapfile->ext_src) {
    oname = make_image_file_name(scrapfile, 1, LIVES_FILE_EXT_SCRAP);
    fd = lives_open_buffered_rdonly(oname);
    lives_free(oname);
    if (fd < 0) return FALSE;
#ifdef HAVE_POSIX_FADVISE
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    scrapfile->ext_src = LIVES_INT_TO_POINTER(fd);
    scrapfile->ext_src_type = LIVES_EXT_SRC_FILE_BUFF;
  } else fd = LIVES_POINTER_TO_INT(scrapfile->ext_src);

  if (frame < 0 || !layer) return TRUE; /// just open fd

  if (!weed_plant_deserialise(fd, NULL, layer)) {
    //g_print("bad scrapfile frame\n");
    return FALSE;
  }
  return TRUE;
}


static void ds_warn(boolean freelow, uint64_t bytes) {
  char *reason, *aorb;
  char *amount = lives_format_storage_space_string(bytes);
  if (freelow) {
    reason = (_("FREE DISK SPACE"));
    aorb = (_("BELOW"));
  } else {
    reason = (_("DISK SPACE USED"));
    aorb = (_("ABOVE"));
  }
  d_print(_("\nRECORDING was PAUSED because %s in %s IS %s %s !\n"
            "Diskspace limits can be set in Preferences / Misc.\n"),
          reason, prefs->workdir, aorb, amount);
  on_record_perf_activate(NULL, NULL);
  d_print_urgency(URGENCY_MSG_TIMEOUT, _("RECORDING WAS PAUSED DUE TO DISKSPACE LIMITS\n"));
  lives_free(reason);
  lives_free(aorb);
}


boolean check_for_disk_space(boolean fullcheck) {
  /// fullcheck == FALSE, we MAY check ds used, and we WILL check free ds using cached value
  /// fullcheck == TRUE, we WILL update free ds
  static int64_t free_ds = -1;
  static double xscrap_mb = -1., xascrap_mb = -1.;
  static double xxscrap_mb = -1., xxascrap_mb = -1.;
  static int64_t ds_used = -1;
  static boolean wrtable = FALSE;

  double scrap_mb = 0.;

  if (prefs->disk_quota == 0 && prefs->rec_stop_gb < 0.) return TRUE;

  if (fullcheck) ds_used = -1;

  if (IS_VALID_CLIP(mainw->scrap_file)) {
    scrap_mb = (double)mainw->files[mainw->scrap_file]->f_size / (double)ONE_MILLION;
  }

  if (prefs->disk_quota > 0) {
    int64_t xds_used = -1;

    if ((ds_used = disk_monitor_check_result(prefs->workdir)) > -1) {
      xds_used = ds_used;
      xxscrap_mb = scrap_mb;
      xxascrap_mb = ascrap_mb;
    } else {
      if (xxscrap_mb == -1. || xxscrap_mb > scrap_mb) xxscrap_mb = scrap_mb;
      if (xxascrap_mb == -1. || xxascrap_mb > ascrap_mb) xxascrap_mb = ascrap_mb;
      if (ds_used > -1) xds_used = ds_used
                                     + (int64_t)(scrap_mb + ascrap_mb - xxscrap_mb - xxascrap_mb)
                                     * ONE_MILLION;
    }
    if (xds_used > -1) {
      /// value is in BYTES
      if ((uint64_t)xds_used >= prefs->disk_quota * ONE_BILLION) {
        if (mainw->record && !mainw->record_paused) {
          ds_warn(FALSE, (uint64_t)ds_used);
        }
        return FALSE;
      }
    }
  }

  // check if we have enough free space left on the volume (return FALSE if not)
  if (prefs->rec_stop_gb > -1.) {
    // check free space again
    if (fullcheck || free_ds == -1 || xscrap_mb == -1 || xascrap_mb == -1
        || scrap_mb < xscrap_mb || ascrap_mb < xascrap_mb) {
      free_ds = (int64_t)get_ds_free(prefs->workdir);
      xscrap_mb = scrap_mb;
      xascrap_mb = ascrap_mb;
      if (free_ds == 0) wrtable = is_writeable_dir(prefs->workdir);
      else wrtable = TRUE;
    }
    if (wrtable) {
      double free_mb = (double)free_ds / (double)ONE_MILLION;
      double freesp = free_mb - (scrap_mb + ascrap_mb - xscrap_mb - xascrap_mb);
      if ((double)freesp / 1000. < prefs->rec_stop_gb) {
        if (mainw->record && !mainw->record_paused) {
          ds_warn(TRUE, freesp * ONE_MILLION);
        }
        return FALSE;
      }
    }
  }
  return TRUE;
}


static void _save_to_scrap_file(weed_layer_t *layer) {
  // returns frame number
  // dump the raw layer (frame) data to disk

  // TODO: run as bg thread

  size_t pdata_size;

  lives_clip_t *scrapfile = mainw->files[mainw->scrap_file];

  boolean writeable = TRUE;

  char *framecount;

  //int flags = O_WRONLY | O_CREAT | O_TRUNC;
  int fd;

  if (!scrapfile->ext_src) {
    char *oname = make_image_file_name(scrapfile, 1, LIVES_FILE_EXT_SCRAP), *dirname;

#ifdef O_NOATIME
    //flags |= O_NOATIME;
#endif

    dirname = lives_build_filename(prefs->workdir, scrapfile->handle, NULL);
    lives_mkdir_with_parents(dirname, capable->umask);
    lives_free(dirname);

    fd = lives_create_buffered_nosync(oname, DEF_FILE_PERMS);
    lives_free(oname);

    if (fd < 0) {
      weed_layer_free(layer);
      return;
    }
    scrapfile->ext_src = LIVES_INT_TO_POINTER(fd);
    scrapfile->ext_src_type = LIVES_EXT_SRC_FILE_BUFF;

#ifdef HAVE_POSIX_FADVISE
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  } else fd = LIVES_POINTER_TO_INT(scrapfile->ext_src);


  // serialise entire frame to scrap file
  pdata_size = weed_plant_serialise(fd, layer, NULL);
  weed_layer_free(layer);

  scrapfile->f_size += pdata_size;

  // check free space every 256 frames or every 10 MB of audio (TODO ****)
  if ((scrapfile->frames & 0xFF) == 0) {
    char *dir = lives_build_filename(prefs->workdir, scrapfile->handle, NULL);
    free_mb = (double)get_ds_free(dir) / 1000000.;
    if (free_mb == 0) writeable = is_writeable_dir(dir);
    lives_free(dir);
  }

  if ((!mainw->fs || (prefs->play_monitor != widget_opts.monitor + 1 && capable->nmonitors > 1)) && !prefs->hide_framebar &&
      !mainw->faded) {
    double scrap_mb = (double)scrapfile->f_size / 1000000.;
    if ((scrap_mb + ascrap_mb) < (double)free_mb * .75) {
      // TRANSLATORS: rec(ord) %.2f M(ega)B(ytes)
      framecount = lives_strdup_printf(_("rec %.2f MB"), scrap_mb + ascrap_mb);
    } else {
      // warn if scrap_file > 3/4 of free space
      // TRANSLATORS: !rec(ord) %.2f M(ega)B(ytes)
      if (writeable)
        framecount = lives_strdup_printf(_("!rec %.2f MB"), scrap_mb + ascrap_mb);
      else
        // TRANSLATORS: rec(ord) ?? M(ega)B(ytes)
        framecount = (_("rec ?? MB"));
    }
    lives_entry_set_text(LIVES_ENTRY(mainw->framecounter), framecount);
    lives_free(framecount);
  }

  /// check every 64 frames for quota overrun, because its a background task
  check_for_disk_space((scrapfile->frames & 0x3F) ? TRUE : FALSE);
}

static lives_proc_thread_t scrap_file_procthrd = NULL;

int save_to_scrap_file(weed_layer_t *layer) {
  weed_layer_t *orig_layer;
  lives_clip_t *scrapfile = mainw->files[mainw->scrap_file];
  if (!IS_VALID_CLIP(mainw->scrap_file)) return -1;
  if (!layer) return scrapfile->frames;
  orig_layer = weed_layer_copy(NULL, layer);
  if (scrap_file_procthrd) {
    lives_proc_thread_join(scrap_file_procthrd);
  }
  scrap_file_procthrd = lives_proc_thread_create(LIVES_THRDATTR_NONE,
                        (lives_funcptr_t)_save_to_scrap_file, -1, "V", orig_layer);
  return scrapfile->frames;
}

void close_scrap_file(boolean remove) {
  int current_file = mainw->current_file;

  if (!IS_VALID_CLIP(mainw->scrap_file)) return;

  if (scrap_file_procthrd) {
    lives_proc_thread_join(scrap_file_procthrd);
    scrap_file_procthrd = NULL;
  }

  mainw->current_file = mainw->scrap_file;
  if (cfile->ext_src && cfile->ext_src_type == LIVES_EXT_SRC_FILE_BUFF)
    lives_close_buffered(LIVES_POINTER_TO_INT(cfile->ext_src));
  cfile->ext_src = NULL;
  cfile->ext_src_type = LIVES_EXT_SRC_NONE;

  if (remove) close_temp_handle(current_file);
  else mainw->current_file = current_file;

  pthread_mutex_lock(&mainw->clip_list_mutex);
  mainw->cliplist = lives_list_remove(mainw->cliplist, LIVES_INT_TO_POINTER(mainw->scrap_file));
  pthread_mutex_unlock(&mainw->clip_list_mutex);

  if (prefs->crash_recovery) rewrite_recovery_file();

  mainw->scrap_file = -1;
}


void close_ascrap_file(boolean remove) {
  int current_file = mainw->current_file;

  if (mainw->ascrap_file == -1) return;

  if (remove) {
    mainw->current_file = mainw->ascrap_file;
    close_temp_handle(current_file);
  }

  pthread_mutex_lock(&mainw->clip_list_mutex);
  mainw->cliplist = lives_list_remove(mainw->cliplist, LIVES_INT_TO_POINTER(mainw->ascrap_file));
  pthread_mutex_unlock(&mainw->clip_list_mutex);

  if (prefs->crash_recovery) rewrite_recovery_file();

  mainw->ascrap_file = -1;
}


void recover_layout_map(int numclips) {
  // load global layout map for a set and assign entries to clips [mainw->files[i]->layout_map]
  LiVESList *omlist, *mlist, *lmap_node, *lmap_node_next, *lmap_entry_list, *lmap_entry_list_next;

  layout_map *lmap_entry;
  uint32_t mask;

  char **array;
  char *check_handle;

  if (numclips > MAX_FILES) numclips = MAX_FILES;

  if ((omlist = load_layout_map())) {
    int i;

    mlist = omlist;

    // assign layout map to clips
    for (i = 1; i <= numclips; i++) {
      lives_clip_t *sfile = mainw->files[i];
      if (!sfile) continue;
      lmap_node = mlist;
      while (lmap_node) {
        lmap_node_next = lmap_node->next;
        lmap_entry = (layout_map *)lmap_node->data;
        check_handle = lives_strdup(sfile->handle);

        if (strstr(lmap_entry->handle, "/") == NULL) {
          lives_free(check_handle);
          check_handle = lives_path_get_basename(sfile->handle);
        }

        if ((!strcmp(check_handle, lmap_entry->handle) && (sfile->unique_id == lmap_entry->unique_id)) ||
            (prefs->mt_load_fuzzy && (!strcmp(check_handle, lmap_entry->handle) || (sfile->unique_id == lmap_entry->unique_id)))
           ) {
          // check handle and unique id match
          // got a match, assign list to layout_map and delete this node
          lmap_entry_list = lmap_entry->list;
          while (lmap_entry_list) {
            lmap_entry_list_next = lmap_entry_list->next;
            array = lives_strsplit((char *)lmap_entry_list->data, "|", -1);
            if (!lives_file_test(array[0], LIVES_FILE_TEST_EXISTS)) {
              //g_print("removing layout because no file %s\n", array[0]);
              // layout file has been deleted, remove this entry
              if (lmap_entry_list->prev) lmap_entry_list->prev->next = lmap_entry_list_next;
              else lmap_entry->list = lmap_entry_list_next;
              if (lmap_entry_list_next) lmap_entry_list_next->prev = lmap_entry_list->prev;
              lives_free((livespointer)lmap_entry_list->data);
              lives_list_free(lmap_entry_list);
            }
            lives_strfreev(array);
            lmap_entry_list = lmap_entry_list_next;
          }
          sfile->layout_map = lmap_entry->list;
          lives_free(lmap_entry->handle);
          lives_free(lmap_entry->name);
          lives_free(lmap_entry);
          if (lmap_node->prev) lmap_node->prev->next = lmap_node_next;
          else omlist = mlist = lmap_node_next;
          if (lmap_node_next) lmap_node_next->prev = lmap_node->prev;
          lmap_node->prev = lmap_node->next = NULL;
          lives_list_free(lmap_node);
          /// check for missing frames and audio in layouts
          // TODO: -- needs checking ----
          mask = 0;
          mainw->xlays = layout_frame_is_affected(i, sfile->frames + 1, 0, mainw->xlays);
          if (mainw->xlays) {
            add_lmap_error(LMAP_ERROR_DELETE_FRAMES, sfile->name, (livespointer)sfile->layout_map, i,
                           sfile->frames, 0., FALSE);
            lives_list_free_all(&mainw->xlays);
            mask |= WARN_MASK_LAYOUT_DELETE_FRAMES;
            //g_print("FRMS %d\n", cfile->frames);
          }

          mainw->xlays = layout_audio_is_affected(i, sfile->laudio_time, 0., mainw->xlays);
          if (mainw->xlays) {
            add_lmap_error(LMAP_ERROR_DELETE_AUDIO, sfile->name, (livespointer)sfile->layout_map, i,
                           sfile->frames, sfile->laudio_time, FALSE);
            lives_list_free_all(&mainw->xlays);
            mask |= WARN_MASK_LAYOUT_DELETE_AUDIO;
            //g_print("AUD %f\n", cfile->laudio_time);
          }
          if (mask != 0) popup_lmap_errors(NULL, LIVES_INT_TO_POINTER(mask));
        }

        lives_free(check_handle);
        lmap_node = lmap_node_next;
      }
    }

    lmap_node = mlist;
    while (lmap_node) {
      lmap_entry = (layout_map *)lmap_node->data;
      if (lmap_entry->name) lives_free(lmap_entry->name);
      if (lmap_entry->handle) lives_free(lmap_entry->handle);
      lives_list_free_all(&lmap_entry->list);
      lmap_node = lmap_node->next;
    }
    if (omlist) lives_list_free(omlist);
  }
}


boolean reload_clip(int fileno, int maxframe) {
  // reload clip -- for CLIP_TYPE_FILE
  // cd to clip directory - so decoder plugins can write temp files
  LiVESList *odeclist;
  lives_clip_t *sfile = mainw->files[fileno];

  const lives_clip_data_t *cdata = NULL;
  lives_clip_data_t *fake_cdata = (lives_clip_data_t *)lives_calloc(sizeof(lives_clip_data_t), 1);

  double orig_fps = sfile->fps;

  char decoder_name[PATH_MAX];
  char *orig_filename = lives_strdup(sfile->file_name);
  char *cwd = lives_get_current_dir();
  char *ppath = lives_build_filename(prefs->workdir, sfile->handle, NULL);

  LiVESResponseType response;
  boolean was_renamed = FALSE, retb = FALSE;
  int current_file;

  fake_cdata = (lives_clip_data_t *)struct_from_template(LIVES_STRUCT_CLIP_DATA_T);

  if (!mainw->decoders_loaded) {
    mainw->decoder_list = load_decoders();
    mainw->decoders_loaded = TRUE;
  }

  odeclist = lives_list_copy(mainw->decoder_list);  ///< retain original order to restore for freshly opened clips
  retb = get_clip_value(fileno, CLIP_DETAILS_DECODER_NAME, decoder_name, PATH_MAX);
  if (retb && *decoder_name) {
    decoder_plugin_move_to_first(decoder_name);
  }
  retb = FALSE;
  lives_chdir(ppath, FALSE);
  lives_free(ppath);

  while (1) {
    threaded_dialog_spin(0.);

    fake_cdata->URI = lives_strdup(sfile->file_name);
    fake_cdata->fps = sfile->fps;
    fake_cdata->nframes = maxframe;

    response = LIVES_RESPONSE_NONE;

    if ((cdata = get_decoder_cdata(fileno, prefs->disabled_decoders, fake_cdata->fps != 0. ? fake_cdata : NULL)) == NULL) {
      if (mainw->error) {
manual_locate:
        response = do_file_notfound_dialog(_("The original file"), orig_filename);
        if (response == LIVES_RESPONSE_RETRY) {
          lives_freep((void **)&fake_cdata->URI);
          continue;
        }
        if (response == LIVES_RESPONSE_BROWSE) {
          int resp;
          char fname[PATH_MAX], dirname[PATH_MAX], *newname;
          LiVESWidget *chooser;

          lives_snprintf(dirname, PATH_MAX, "%s", orig_filename);
          lives_snprintf(fname, PATH_MAX, "%s", orig_filename);

          get_dirname(dirname);
          get_basename(fname);

          chooser = choose_file_with_preview(dirname, fname, NULL, LIVES_FILE_SELECTION_VIDEO_AUDIO);

          resp = lives_dialog_run(LIVES_DIALOG(chooser));

          end_fs_preview();

          if (resp == LIVES_RESPONSE_ACCEPT) {
            newname = lives_file_chooser_get_filename(LIVES_FILE_CHOOSER(chooser));
            lives_widget_destroy(LIVES_WIDGET(chooser));

            if (newname && *newname) {
              char *tmp;
              lives_snprintf(sfile->file_name, PATH_MAX, "%s", (tmp = lives_filename_to_utf8(newname, -1, NULL, NULL, NULL)));
              lives_free(tmp);
              lives_free(newname);
            }

            lives_freep((void **)&fake_cdata->URI);

            //re-scan for these
            sfile->fps = 0.;
            maxframe = 0;

            was_renamed = TRUE;
            // try again with the new file
            continue;
          }
          // cancelled from filechooser
          lives_widget_destroy(LIVES_WIDGET(chooser));
          goto manual_locate;
        }
        // cancelled
      } else {
        // unopenable
        if (was_renamed) goto manual_locate;
        do_no_decoder_error(sfile->file_name);
      }

      lives_chdir(cwd, FALSE);
      lives_free(cwd);

      // NOT openable, or not found and user cancelled, switch back to original clip
      if (!sfile->checked && cdata) {
        check_clip_integrity(fileno, cdata, maxframe);
        if (sfile->frames > 0 || sfile->afilesize > 0) {
          // recover whatever we can
          sfile->clip_type = CLIP_TYPE_FILE;
          retb = check_if_non_virtual(fileno, 1, sfile->frames);
        }
        sfile->checked = TRUE;
      }
      if (!retb) {
        current_file = mainw->current_file;
        mainw->current_file = fileno;
        close_current_file(current_file);
      }
      unref_struct(&fake_cdata->lsd);

      lives_free(orig_filename);
      lives_list_free(mainw->decoder_list);
      mainw->decoder_list = odeclist;
      return retb;
    }

    // got cdata
    if (was_renamed) {
      // manual relocation
      sfile->fps = orig_fps;
      if (!sfile->checked && !check_clip_integrity(fileno, cdata, maxframe)) {
        // get correct img_type, fps, etc.
        if (THREADVAR(com_failed) || THREADVAR(write_failed)) do_header_write_error(fileno);
        goto manual_locate;
      }
      sfile->checked = TRUE;
      sfile->needs_silent_update = TRUE; // force filename update in header
      if (prefs->show_recent) {
        // replace in recent menu
        char file[PATH_MAX];
        int i;
        for (i = 0; i < 4; i++) {
          char *tmp;
          char *pref = lives_strdup_printf("%s%d", PREF_RECENT, i + 1);
          get_utf8_pref(pref, file, PATH_MAX);
          tmp = subst(file, orig_filename, sfile->file_name);
          if (lives_utf8_strcmp(tmp, file)) {
            lives_snprintf(file, PATH_MAX, "%s", tmp);
            set_utf8_pref(pref, file);
            lives_menu_item_set_text(mainw->recent[i], file, FALSE);
            if (mainw->multitrack) lives_menu_item_set_text(mainw->multitrack->recent[i], file, FALSE);
          }
          lives_free(tmp);
          lives_free(pref);
        }
        if (mainw->prefs_cache) {
          // update recent files -> force reload of prefs
          cached_list_free(&mainw->prefs_cache);
          mainw->prefs_cache = cache_file_contents(prefs->configfile);
        }
      }
    }

    threaded_dialog_spin(0.);
    unref_struct(&fake_cdata->lsd);
    break;
  }

  lives_free(orig_filename);
  lives_chdir(cwd, FALSE);
  lives_free(cwd);

  sfile->clip_type = CLIP_TYPE_FILE;
  get_mime_type(sfile->type, 40, cdata);
  sfile->img_type = IMG_TYPE_BEST; // read_headers() will have set this to "jpeg" (default)
  // we will set correct value in check_clip_integrity() if there are any real images

  if (sfile->ext_src) {
    boolean bad_header = FALSE;
    boolean correct = TRUE;
    if (!was_renamed) {
      if (!sfile->checked)
        correct = check_clip_integrity(fileno, cdata, maxframe); // get correct img_type, fps, etc.
      sfile->checked = TRUE;
    }
    if (!correct) {
      if (THREADVAR(com_failed) || THREADVAR(write_failed)) bad_header = TRUE;
    } else {
      lives_decoder_t *dplug = (lives_decoder_t *)sfile->ext_src;
      if (dplug) {
        lives_decoder_sys_t *dpsys = (lives_decoder_sys_t *)dplug->decoder;
        if (dpsys && *dpsys->name && strcmp(dpsys->name, decoder_name)) {
          save_clip_value(fileno, CLIP_DETAILS_DECODER_NAME, (void *)dpsys->name);
          if (THREADVAR(com_failed) || THREADVAR(write_failed)) bad_header = TRUE;
        }
      }
    }

    if (bad_header) do_header_write_error(fileno);
  }
  lives_list_free(mainw->decoder_list);
  mainw->decoder_list = odeclist;
  if (prefs->autoload_subs) {
    reload_subs(fileno);
  }
  return TRUE;
}

#define _RELOAD(field) sfile->field = loaded->field
#define _RELOAD_STRING(field, len) lives_snprintf(sfile->field, len, "%s", loaded->field)

#define DSIZE_MAX 100000

static lives_clip_t *_restore_binfmt(int clipno, boolean forensic) {
  if (IS_NORMAL_CLIP(clipno)) {
    lives_clip_t *sfile = mainw->files[clipno];
    char *fname = lives_build_filename(prefs->workdir, sfile->handle, TOTALSAVE_NAME, NULL);
    if (lives_file_test(fname, LIVES_FILE_TEST_EXISTS)) {
      ssize_t bytes;
      size_t cursize = (size_t)((char *)&sfile->binfmt_end - (char *)sfile), dsize;
      char *xloaded = (char *)lives_calloc(1, cursize);
      boolean badsize = FALSE;
      int fd = lives_open_buffered_rdonly(fname);
      size_t fsize = lives_buffered_orig_size(fd);
      lives_clip_t *loaded = (lives_clip_t *)xloaded;
      if (fsize < cursize) badsize = TRUE;
      else {
        bytes = lives_read_buffered(fd, xloaded, 8, TRUE);
        if (bytes < 8 || lives_memcmp(loaded->binfmt_check.chars, CLIP_BINFMT_CHECK, 8)) badsize = TRUE;
        else {
          bytes += lives_read_buffered(fd, xloaded + 8, 16, TRUE);
          if (bytes < 16) badsize = TRUE;
          else {
            dsize = loaded->binfmt_bytes.num;
            if (dsize < cursize) badsize = TRUE;
            else {
              if (dsize > cursize && dsize < DSIZE_MAX) {
                xloaded = lives_realloc(xloaded, dsize);
                loaded = (lives_clip_t *)xloaded;
              } else dsize = cursize;
              bytes += lives_read_buffered(fd, xloaded + 24, dsize - 24, TRUE);
              if (bytes < dsize) badsize = TRUE;
            }
          }
        }
      }
      lives_close_buffered(fd);
      //if (!forensic) lives_rm(fname);
      lives_free(fname);

      if (badsize) {
        lives_free(xloaded);
        return FALSE;
      }

      THREADVAR(com_failed) = FALSE;
      if (THREADVAR(read_failed) == fd + 1) {
        THREADVAR(read_failed) = 0;
        lives_free(xloaded);
        return NULL;
      }

      sfile->has_binfmt = TRUE;

      if (forensic) return loaded;

      _RELOAD_STRING(save_file_name, PATH_MAX);  _RELOAD(start); _RELOAD(end); _RELOAD(is_untitled); _RELOAD(was_in_set);
      _RELOAD(ratio_fps); _RELOAD_STRING(mime_type, 256);
      _RELOAD(changed); _RELOAD(deinterlace); _RELOAD(vol);
      if (sfile->start < 1) sfile->start = 1;
      if (sfile->end > sfile->frames) sfile->end = sfile->frames;
      if (sfile->start > sfile->end) sfile->start = sfile->end;
      if (lives_strlen(sfile->save_file_name) > PATH_MAX) lives_memset(sfile->save_file_name, 0, PATH_MAX);
      if (sfile->pointer_time > sfile->video_time) sfile->pointer_time = 0.;
      if (sfile->real_pointer_time > CLIP_TOTAL_TIME(clipno)) sfile->real_pointer_time = sfile->pointer_time;
      return loaded;
    }
    lives_free(fname);
  }
  return NULL;
}

#undef _RELOAD
#undef _RELOAD_STRING

boolean restore_clip_binfmt(int clipno) {
  lives_clip_t *recov = _restore_binfmt(clipno, FALSE);
  if (!recov) return FALSE;
  lives_free(recov);
  return TRUE;
}

lives_clip_t *clip_forensic(int clipno) {
  return  _restore_binfmt(clipno, TRUE);
}

boolean recover_files(char *recovery_file, boolean auto_recover) {
  FILE *rfile = NULL;

  char buff[256], *buffptr;
  char *clipdir;

  LiVESResponseType resp;

  int clipnum = 0;
  int maxframe;
  int last_good_file = -1, ngoodclips;

  boolean is_scrap, is_ascrap;
  boolean did_set_check = FALSE;
  boolean is_ready = mainw->is_ready, mt_is_ready = FALSE;
  boolean mt_needs_idlefunc = FALSE;
  boolean retb = TRUE, retval;
  boolean load_from_set = TRUE;
  boolean rec_cleanup = FALSE;

  // setting is_ready allows us to get the correct transient window for dialogs
  // otherwise the dialogs will appear behind the main interface
  // we do this for mainwindow and multitrack

  // we will reset these before returning
  mainw->is_ready = TRUE;

  if (mainw->multitrack) {
    if (mainw->multitrack->idlefunc > 0) {
      lives_source_remove(mainw->multitrack->idlefunc);
      mainw->multitrack->idlefunc = 0;
      mt_needs_idlefunc = TRUE;
    }
    mt_desensitise(mainw->multitrack);
    mt_is_ready = mainw->multitrack->is_ready;
    mainw->multitrack->is_ready = TRUE;
  }

  if (!auto_recover) {
    if (1 || mainw->multitrack) {
      lives_widget_show_all(LIVES_MAIN_WINDOW_WIDGET);
      lives_widget_context_update();
    }
    if (!do_yesno_dialog
        (_("\nFiles from a previous run of LiVES were found.\nDo you want to attempt to recover them ?\n"))) {
      retb = FALSE;
      goto recovery_done;
    }
  }

  if (recovery_file) {
    do {
      resp = LIVES_RESPONSE_NONE;
      rfile = fopen(recovery_file, "r");
      if (!rfile) {
        resp = do_read_failed_error_s_with_retry(recovery_file, lives_strerror(errno));
        if (resp == LIVES_RESPONSE_CANCEL) {
          retb = FALSE;
          goto recovery_done;
        }
      }
    } while (resp == LIVES_RESPONSE_RETRY);
  }

  do_threaded_dialog(_("Recovering files"), FALSE);
  d_print(_("\nRecovering files..."));

  threaded_dialog_spin(0.);

  mainw->suppress_dprint = TRUE;
  mainw->recovering_files = TRUE;

  while (1) {
    if (mainw->hdrs_cache) cached_list_free(&mainw->hdrs_cache);

    threaded_dialog_spin(0.);
    is_scrap = FALSE;
    is_ascrap = FALSE;

    THREADVAR(read_failed) = FALSE;

    if (recovery_file) {
      if (!lives_fgets(buff, 256, rfile)) {
        reset_clipmenu();
        threaded_dialog_spin(0.);
        mainw->suppress_dprint = FALSE;
        if (THREADVAR(read_failed)) {
          d_print_failed();
          do_read_failed_error_s(recovery_file, NULL);
        } else d_print_done();
        break;
      }
    } else {
      if (!mainw->recovery_list) {
        reset_clipmenu();
        mainw->suppress_dprint = FALSE;
        d_print_done();
        break;
      }
      lives_snprintf(buff, 256, "%s", (char *)mainw->recovery_list->data);
      mainw->recovery_list = mainw->recovery_list->next;
    }

    lives_chomp(buff);

    if (buff[strlen(buff) - 1] == '*') {
      boolean crash_recovery = prefs->crash_recovery;
      LiVESResponseType resp;
      // set to be opened
      buff[strlen(buff) - 1 - strlen(LIVES_DIR_SEP)] = 0;
      do {
        resp = LIVES_RESPONSE_OK;
        if (!is_legal_set_name(buff, TRUE, TRUE)) {
          resp = do_abort_cancel_retry_dialog(_("Click Abort to exit LiVES immediately, Retry to try again,"
                                                " or Cancel to continue without reloading the set.\n"));
        }
      } while (resp == LIVES_RESPONSE_RETRY);
      if (resp == LIVES_RESPONSE_CANCEL) continue;

      /** dont write an entry yet, in case of the unklikely chance we were assigned the same pid as the recovery file,
        otherwise we will end up in am endless loop of reloading the same set and appending it to the recovery file
        in any case, the old file is still there and we will create a fresh recovery file after a succesful reload */
      prefs->crash_recovery = FALSE;

      if (!reload_set(buff)) {
        prefs->crash_recovery = crash_recovery; /// reset to original value
        mainw->suppress_dprint = FALSE;
        d_print_failed();
        mainw->suppress_dprint = FALSE;
        continue;
      }
      mainw->was_set = TRUE;
      prefs->crash_recovery = crash_recovery; /// reset to original value
    } else {
      /// load single file
      if (!strncmp(buff, "scrap|", 6)) {
        is_scrap = TRUE;
        buffptr = buff + 6;
      } else if (!strncmp(buff, "ascrap|", 7)) {
        is_ascrap = TRUE;
        buffptr = buff + 7;
      } else {
        if (!strncmp(buff, "ascrap", 6) || !strncmp(buff, "scrap", 5)) {
          rec_cleanup = TRUE;
          continue;
        }
        buffptr = buff;
      }

      clipdir = lives_build_filename(prefs->workdir, buffptr, NULL);

      if (!lives_file_test(clipdir, LIVES_FILE_TEST_IS_DIR)) {
        lives_free(clipdir);
        continue;
      }
      lives_free(clipdir);

      if (strstr(buffptr, "/" CLIPS_DIRNAME "/")) {
        char **array;
        threaded_dialog_spin(0.);
        if (!load_from_set) continue;
        array = lives_strsplit(buffptr, "/" CLIPS_DIRNAME "/", -1);
        mainw->was_set = TRUE;
        lives_snprintf(mainw->set_name, 128, "%s", array[0]);
        lives_strfreev(array);

        if (!did_set_check && !check_for_lock_file(mainw->set_name, 0)) {
          if (!do_set_locked_warning(mainw->set_name)) {
            load_from_set = FALSE;
            mainw->was_set = FALSE;
            mainw->set_name[0] = 0;
          }
          did_set_check = TRUE;
        }
      }

      /// create a new cfile and fill in the details
      if (!create_cfile(-1, buffptr, FALSE)) {
        threaded_dialog_spin(0.);
        end_threaded_dialog();
        mainw->suppress_dprint = FALSE;
        d_print_failed();
        break;
      }

      if (is_scrap || is_ascrap) {
        pthread_mutex_lock(&mainw->clip_list_mutex);
        mainw->cliplist = lives_list_append(mainw->cliplist, LIVES_INT_TO_POINTER(mainw->current_file));
        pthread_mutex_unlock(&mainw->clip_list_mutex);
      }

      if (is_scrap) {
        mainw->scrap_file = mainw->current_file;
        cfile->opening = FALSE;
        lives_snprintf(cfile->type, 40, "scrap");
        cfile->frames = 1;
        cfile->hsize = 640;
        cfile->vsize = 480;
        continue;
      }

      if (is_ascrap) {
        mainw->ascrap_file = mainw->current_file;
        cfile->opening = FALSE;
        lives_snprintf(cfile->type, 40, "ascrap");
      }

      /// get file details; this will cache the header in mainw->hdrs_cache
      // we need to keep this around for open_set_file(), below.
      clipdir = lives_build_path(prefs->workdir, cfile->handle, NULL);
      retval = read_headers(mainw->current_file, clipdir, NULL);
      lives_free(clipdir);

      if (is_ascrap) {
        if (!retval) {
          mainw->first_free_file = mainw->current_file;
          mainw->ascrap_file = -1;
        }
        continue;
      }

      if (mainw->current_file < 1) continue;

      /// see function reload_set() for detailed comments
      if ((maxframe = load_frame_index(mainw->current_file)) > 0) {
        /// CLIP_TYPE_FILE
        if (!*cfile->file_name) continue;
        if (!reload_clip(mainw->current_file, maxframe)) continue;
        if (cfile->img_type == IMG_TYPE_UNKNOWN) {
          lives_clip_data_t *cdata = ((lives_decoder_t *)cfile->ext_src)->cdata;
          int fvirt = count_virtual_frames(cfile->frame_index, 1, cfile->frames);
          if (fvirt < cfile->frames) {
            if (!cfile->checked && !check_clip_integrity(mainw->current_file, cdata, cfile->frames)) {
              cfile->needs_update = TRUE;
            }
            cfile->checked = TRUE;
          }
          if (cfile->header_version >= 102) cfile->fps = cfile->pb_fps;
        }
      } else {
        /// CLIP_TYPE_DISK
        boolean is_ok = TRUE;
        if (!cfile->checked) {
          if (!(is_ok = check_clip_integrity(mainw->current_file, NULL, cfile->frames))) {
            cfile->needs_update = TRUE;
          }
          cfile->checked = TRUE;
        }
        if (!prefs->vj_mode && !is_ok) {
          if (cfile->afilesize == 0) {
            reget_afilesize_inner(mainw->current_file);
          }
          if (!check_frame_count(mainw->current_file, is_ok)) {
            cfile->frames = get_frame_count(mainw->current_file, 1);
            cfile->needs_update = TRUE;
          }
        }
      }
      if (!recovery_file && !cfile->checked) {
        lives_clip_data_t *cdata = ((lives_decoder_t *)cfile->ext_src)->cdata;
        if (!check_clip_integrity(mainw->current_file, cdata, cfile->frames)) {
          cfile->needs_update = TRUE;
          if (cfile->header_version >= 102) cfile->fps = cfile->pb_fps;
        }
      }

      threaded_dialog_spin(0.);

      /** not really from a set, but let's pretend to get the details
        read the playback fps, play frame, and name */

      /// NEED TO maintain mainw->hdrs_cache when entering the function,
      /// else it will be considered a legacy file load
      open_set_file(++clipnum);

      threaded_dialog_spin(0.);

      if (mainw->hdrs_cache) cached_list_free(&mainw->hdrs_cache);

      if (mainw->current_file < 1) continue;

      if (cfile->clip_type == CLIP_TYPE_FILE && cfile->header_version >= 102) cfile->fps = cfile->pb_fps;
      get_total_time(cfile);

      if (CLIP_TOTAL_TIME(mainw->current_file) == 0.) {
        close_current_file(last_good_file);
        continue;
      }

      last_good_file = mainw->current_file;

      if (cfile->needs_update || cfile->needs_silent_update) {
        if (cfile->needs_update) do_clip_divergence_error(mainw->current_file);
        save_clip_values(mainw->current_file);
        cfile->needs_silent_update = cfile->needs_update = FALSE;
      }

      // add to clip menu
      threaded_dialog_spin(0.);
      add_to_clipmenu();
      cfile->start = cfile->frames > 0 ? 1 : 0;
      cfile->end = cfile->frames;
      cfile->is_loaded = TRUE;
      cfile->changed = TRUE;
      lives_rm(cfile->info_file);
      if (!mainw->multitrack) set_main_title(cfile->name, 0);

      if (cfile->frameno > cfile->frames) cfile->frameno = cfile->last_frameno = 1;

      if (!mainw->multitrack) {
        lives_signal_handler_block(mainw->spinbutton_start, mainw->spin_start_func);
        lives_spin_button_set_value(LIVES_SPIN_BUTTON(mainw->spinbutton_start), cfile->start);
        lives_signal_handler_unblock(mainw->spinbutton_start, mainw->spin_start_func);
        lives_signal_handler_block(mainw->spinbutton_end, mainw->spin_end_func);
        lives_spin_button_set_value(LIVES_SPIN_BUTTON(mainw->spinbutton_end), cfile->end);
        lives_signal_handler_unblock(mainw->spinbutton_end, mainw->spin_end_func);
        showclipimgs();
      } else {
        int current_file = mainw->current_file;
        lives_mt *multi = mainw->multitrack;
        mainw->multitrack = NULL;
        mainw->current_file = -1;
        reget_afilesize(current_file);
        mainw->current_file = current_file;
        mainw->multitrack = multi;
        get_total_time(cfile);
        mainw->current_file = mainw->multitrack->render_file;
        mt_init_clips(mainw->multitrack, current_file, TRUE);
        set_poly_tab(mainw->multitrack, POLY_CLIPS);
        lives_widget_process_updates(LIVES_MAIN_WINDOW_WIDGET);
        mt_clip_select(mainw->multitrack, TRUE);
        lives_widget_process_updates(LIVES_MAIN_WINDOW_WIDGET);
        mainw->current_file = current_file;
      }

      threaded_dialog_spin(0.);

      if (cfile->frameno > cfile->frames && cfile->frameno > 1) cfile->frameno = cfile->frames;
      cfile->last_frameno = cfile->frameno;
      cfile->pointer_time = cfile->real_pointer_time = calc_time_from_frame(mainw->current_file, cfile->frameno);
      if (cfile->real_pointer_time > CLIP_TOTAL_TIME(mainw->current_file))
        cfile->real_pointer_time = CLIP_TOTAL_TIME(mainw->current_file);
      if (cfile->pointer_time > cfile->video_time) cfile->pointer_time = 0.;

      if (cfile->achans) {
        cfile->aseek_pos = (off64_t)((double)(cfile->real_pointer_time * cfile->arate) * cfile->achans *
                                     (cfile->asampsize / 8));
        if (cfile->aseek_pos > cfile->afilesize) cfile->aseek_pos = 0.;
      }

      if (mainw->current_file != -1)
        if (*mainw->set_name) recover_layout_map(mainw->current_file);

      if (!mainw->multitrack) resize(1);

      lives_notify(LIVES_OSC_NOTIFY_CLIP_OPENED, "");
    }
  }

  if (mainw->hdrs_cache) cached_list_free(&mainw->hdrs_cache);

  ngoodclips = lives_list_length(mainw->cliplist);
  if (!ngoodclips) {
    d_print(_("No clips were recovered.\n"));
  }
  d_print(P_("%d clip was recovered ", "%d clips were recovered ", ngoodclips), ngoodclips);
  if (recovery_file)
    d_print(_("from the previous session.\n"));
  else
    d_print(_("from previous sessions.\n"));

  if (!mainw->multitrack) { // TODO check if we can do this in mt too
    int start_file = mainw->current_file;
    if (start_file > 1 && start_file == mainw->ascrap_file && mainw->files[start_file - 1]) {
      start_file--;
    }
    if (start_file > 1 && start_file == mainw->scrap_file && mainw->files[start_file - 1]) {
      start_file--;
    }
    if (start_file > 1 && start_file == mainw->ascrap_file && mainw->files[start_file - 1]) {
      start_file--;
    }
    if ((!IS_VALID_CLIP(start_file) || (mainw->files[start_file]->frames == 0 && mainw->files[start_file]->afilesize == 0))
        && mainw->files[1] && start_file != 1) {
      for (start_file = MAX_FILES; start_file > 0; start_file--) {
        if (mainw->files[start_file]
            && (mainw->files[start_file]->frames > 0 || mainw->files[start_file]->afilesize > 0))
          if (start_file != mainw->scrap_file && start_file != mainw->ascrap_file) break;
      }
    }

    if (start_file != mainw->current_file) {
      rec_cleanup = TRUE;
      switch_to_file(mainw->current_file, start_file);
      showclipimgs();
      redraw_timeline(mainw->current_file);
    }
  } else {
    mt_clip_select(mainw->multitrack, TRUE); // scroll clip on screen
  }

  if (recovery_file) fclose(rfile);

recovery_done:

  end_threaded_dialog();

  mainw->suppress_dprint = FALSE;
  lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
  mainw->recovering_files = FALSE;
  mainw->is_ready = is_ready;
  if (mainw->multitrack) {
    mainw->multitrack->is_ready = mt_is_ready;
    mainw->current_file = mainw->multitrack->render_file;
    polymorph(mainw->multitrack, POLY_NONE);
    polymorph(mainw->multitrack, POLY_CLIPS);
    mt_sensitise(mainw->multitrack);
    if (mt_needs_idlefunc) mainw->multitrack->idlefunc = mt_idle_add(mainw->multitrack);
  } else update_play_times();
  mainw->last_dprint_file = -1;
  mainw->no_switch_dprint = FALSE;
  d_print("");
  mainw->invalid_clips = rec_cleanup;
  return retb;
}


void add_to_recovery_file(const char *handle) {
  lives_echo(handle, mainw->recovery_file, TRUE);

  if (THREADVAR(com_failed)) {
    THREADVAR(com_failed) = FALSE;
    return;
  }

  if ((mainw->multitrack && mainw->multitrack->event_list) || mainw->stored_event_list)
    write_backup_layout_numbering(mainw->multitrack);
}


boolean rewrite_recovery_file(void) {
  // part of the crash recovery system
  // returns TRUE if successful
  LiVESList *clist = mainw->cliplist;
  char *recovery_entry;
  char *temp_recovery_file;

  boolean opened = FALSE;
  boolean wrote_set_entry = FALSE;

  int recovery_fd = -1;
  LiVESResponseType retval;

  if (!clist || !prefs->crash_recovery) {
    lives_rm(mainw->recovery_file);
    return FALSE;
  }

  temp_recovery_file = lives_strdup_printf("%s.%s", mainw->recovery_file, LIVES_FILE_EXT_TMP);

  do {
    retval = LIVES_RESPONSE_NONE;
    THREADVAR(write_failed) = FALSE;
    opened = FALSE;
    recovery_fd = -1;

    for (; clist; clist = clist->next) {
      int i = LIVES_POINTER_TO_INT(clist->data);
      if (IS_NORMAL_CLIP(i)) {
        lives_clip_t *sfile = mainw->files[i];
        if (i == mainw->scrap_file) {
          recovery_entry = lives_strdup_printf("scrap|%s\n", sfile->handle);
        } else if (i == mainw->ascrap_file) {
          recovery_entry = lives_strdup_printf("ascrap|%s\n", sfile->handle);
        } else {
          if (sfile->was_in_set && *mainw->set_name) {
            if (!wrote_set_entry) {
              recovery_entry = lives_build_filename(mainw->set_name, "*\n", NULL);
              wrote_set_entry = TRUE;
            } else continue;
          } else recovery_entry = lives_strdup_printf("%s\n", sfile->handle);
        }

        if (!opened) recovery_fd = creat(temp_recovery_file, S_IRUSR | S_IWUSR);
        if (recovery_fd < 0) retval = do_write_failed_error_s_with_retry(temp_recovery_file, lives_strerror(errno));
        else {
          opened = TRUE;
          lives_write(recovery_fd, recovery_entry, strlen(recovery_entry), TRUE);
          if (THREADVAR(write_failed)) retval = do_write_failed_error_s_with_retry(temp_recovery_file, NULL);
        }
        lives_free(recovery_entry);
      }
      if (THREADVAR(write_failed)) break;
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  if (!opened) lives_rm(mainw->recovery_file);
  else if (recovery_fd >= 0) {
    close(recovery_fd);
    retval = LIVES_RESPONSE_INVALID;
    do {
      lives_mv(temp_recovery_file, mainw->recovery_file);
      if (THREADVAR(com_failed)) {
        retval = do_write_failed_error_s_with_retry(temp_recovery_file, NULL);
      }
    } while (retval == LIVES_RESPONSE_RETRY);
  }

  lives_free(temp_recovery_file);

  if ((mainw->multitrack && mainw->multitrack->event_list) || mainw->stored_event_list)
    write_backup_layout_numbering(mainw->multitrack);

  return TRUE;
}


boolean check_for_recovery_files(boolean auto_recover) {
  uint32_t recpid = 0;

  char *recovery_file, *recovery_numbering_file, *recording_file, *recording_numbering_file, *xfile;
  char *com;

  boolean retval = FALSE;
  boolean found = FALSE, found_recording = FALSE;

  int lgid = lives_getgid();
  int luid = lives_getuid();

  lives_pgid_t lpid = capable->mainpid;

  // ask backend to find the latest recovery file which is not owned by a running version of LiVES
  com = lives_strdup_printf("%s get_recovery_file %d %d %s recovery %d", prefs->backend_sync, luid, lgid,
                            capable->myname, capable->mainpid);

  lives_popen(com, FALSE, mainw->msg, MAINW_MSG_SIZE);
  lives_free(com);

  if (THREADVAR(com_failed)) {
    THREADVAR(com_failed) = FALSE;
    return FALSE;
  }

  recpid = atoi(mainw->msg);
  if (recpid == 0) return FALSE;

  retval = recover_files((recovery_file = lives_strdup_printf("%s/recovery.%d.%d.%d", prefs->workdir, luid,
                                          lgid, recpid)), auto_recover);
  lives_free(recovery_file);

  if (!retval || prefs->vj_mode) {
    com = lives_strdup_printf("%s clean_recovery_files %d %d \"%s\" %d %d", prefs->backend_sync, luid, lgid, capable->myname,
                              capable->mainpid, prefs->vj_mode);
    lives_system(com, FALSE);
    lives_free(com);
    if (prefs->vj_mode) {
      rewrite_recovery_file();
      return TRUE;
    }
    return FALSE;
  }

#if !GTK_CHECK_VERSION(3, 0, 0)
  if (CURRENT_CLIP_IS_VALID) {
    showclipimgs();
    lives_widget_queue_resize(mainw->video_draw);
    lives_widget_queue_resize(mainw->laudio_draw);
    lives_widget_queue_resize(mainw->raudio_draw);
  }
#endif

  THREADVAR(com_failed) = FALSE;

  /// CRITICAL: make sure this gets called even on system failure and abort
  if (prefs->crash_recovery) mainw->abort_hook_func = (lives_funcptr_t)rewrite_recovery_file;

  // check for layout recovery file
  recovery_file = lives_strdup_printf("%s/%s.%d.%d.%d.%s", prefs->workdir, LAYOUT_FILENAME, luid, lgid, recpid,
                                      LIVES_FILE_EXT_LAYOUT);
  recovery_numbering_file = lives_strdup_printf("%s/%s.%d.%d.%d", prefs->workdir, LAYOUT_NUMBERING_FILENAME, luid, lgid, recpid);

  recording_file = lives_strdup_printf("%s/recorded-%s.%d.%d.%d.%s", prefs->workdir, LAYOUT_FILENAME, luid, lgid, recpid,
                                       LIVES_FILE_EXT_LAYOUT);

  recording_numbering_file = lives_strdup_printf("%s/recorded-%s.%d.%d.%d", prefs->workdir, LAYOUT_NUMBERING_FILENAME, luid, lgid,
                             recpid);

  if (!lives_file_test(recovery_file, LIVES_FILE_TEST_EXISTS)) {
    lives_free(recovery_file);
    recovery_file = lives_strdup_printf("%s/%s.%d.%d.%d", prefs->workdir, LAYOUT_FILENAME, luid, lgid, recpid);
    if (lives_file_test(recovery_file, LIVES_FILE_TEST_EXISTS)) {
      found = TRUE;
    }
  } else {
    found = TRUE;
  }
  if (found) {
    if (!lives_file_test(recovery_numbering_file, LIVES_FILE_TEST_EXISTS)) {
      found = FALSE;
    }
  }

  if (prefs->rr_crash && lives_file_test(recording_file, LIVES_FILE_TEST_EXISTS)) {
    if (lives_file_test(recording_numbering_file, LIVES_FILE_TEST_EXISTS)) {
      found_recording = TRUE;
      xfile = lives_strdup_printf("%s/keep_recorded-layout.%d.%d.%d", prefs->workdir, luid, lgid, lpid);
      lives_mv(recording_file, xfile);
      lives_free(xfile);
      xfile = lives_strdup_printf("%s/keep_recorded-layout_numbering.%d.%d.%d", prefs->workdir, luid, lgid, lpid);
      lives_mv(recording_numbering_file, xfile);
      lives_free(xfile);
      mainw->recording_recovered = TRUE;
    }
  }

  if (found) {
    // move files temporarily to stop them being cleansed
    xfile = lives_strdup_printf("%s/keep_layout.%d.%d.%d", prefs->workdir, luid, lgid, lpid);
    lives_mv(recovery_file, xfile);
    lives_free(xfile);
    xfile = lives_strdup_printf("%s/keep_layout_numbering.%d.%d.%d", prefs->workdir, luid, lgid, lpid);
    lives_mv(recovery_numbering_file, xfile);
    lives_free(xfile);
    mainw->recoverable_layout = TRUE;
  }

  if (!found && !found_recording) {
    if (mainw->scrap_file != -1) close_scrap_file(TRUE);
    if (mainw->ascrap_file != -1) close_ascrap_file(TRUE);
  }

  lives_free(recovery_file);
  lives_free(recovery_numbering_file);
  lives_free(recording_file);
  lives_free(recording_numbering_file);

  if (THREADVAR(com_failed) && prefs->crash_recovery) {
    rewrite_recovery_file();
    return FALSE;
  }

  com = lives_strdup_printf("%s clean_recovery_files %d %d \"%s\" %d 0", prefs->backend_sync, luid, lgid, capable->myname,
                            capable->mainpid);
  lives_system(com, FALSE);
  lives_free(com);

  recovery_file = lives_strdup_printf("%s/%s.%d.%d.%d.%s", prefs->workdir, LAYOUT_FILENAME, luid, lgid, lpid,
                                      LIVES_FILE_EXT_LAYOUT);
  recovery_numbering_file = lives_strdup_printf("%s/%s.%d.%d.%d", prefs->workdir, LAYOUT_NUMBERING_FILENAME, luid, lgid, lpid);

  if (mainw->recoverable_layout) {
    // move files back
    xfile = lives_strdup_printf("%s/keep_layout.%d.%d.%d", prefs->workdir, luid, lgid, lpid);
    lives_mv(xfile, recovery_file);
    lives_free(xfile);
    xfile = lives_strdup_printf("%s/keep_layout_numbering.%d.%d.%d", prefs->workdir, luid, lgid, lpid);
    lives_mv(xfile, recovery_numbering_file);
    lives_free(xfile);
  }

  recording_file = lives_strdup_printf("%s/recorded-%s.%d.%d.%d.%s", prefs->workdir, LAYOUT_FILENAME, luid, lgid, lpid,
                                       LIVES_FILE_EXT_LAYOUT);
  recording_numbering_file = lives_strdup_printf("%s/recorded-%s.%d.%d.%d", prefs->workdir, LAYOUT_NUMBERING_FILENAME, luid, lgid,
                             lpid);

  if (mainw->recording_recovered) {
    xfile = lives_strdup_printf("%s/keep_recorded-layout.%d.%d.%d", prefs->workdir, luid, lgid, lpid);
    /// may fail -> abort
    lives_mv(xfile, recording_file);
    lives_free(xfile);
    xfile = lives_strdup_printf("%s/keep_recorded-layout_numbering.%d.%d.%d", prefs->workdir, luid, lgid, lpid);
    lives_mv(xfile, recording_numbering_file);
    lives_free(xfile);
  }

  lives_free(recovery_file);
  lives_free(recovery_numbering_file);
  lives_free(recording_file);
  lives_free(recording_numbering_file);

  if (prefs->crash_recovery) rewrite_recovery_file();
  mainw->abort_hook_func = NULL;

  if (!mainw->recoverable_layout && !mainw->recording_recovered) {
    if (mainw->invalid_clips && (prefs->warning_mask ^ (WARN_MASK_CLEAN_AFTER_CRASH | WARN_MASK_CLEAN_INVALID))
        == WARN_MASK_CLEAN_INVALID) do_after_invalid_warning();
    else do_after_crash_warning();
    mainw->invalid_clips = FALSE;
  }
  return retval;
}

