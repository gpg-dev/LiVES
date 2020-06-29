// rte_window.c
// LiVES (lives-exe)
// (c) G. Finch 2005 - 2018 <salsaman+lives@gmail.com>
// released under the GNU GPL 3 or later
// see file ../COPYING or www.gnu.org for licensing details

#include "main.h"
#include "rte_window.h"
#include "effects.h"
#include "paramwindow.h"
#include "ce_thumbs.h"

static int old_rte_keys_virtual = 0;

static LiVESWidget **key_checks;
static LiVESWidget **key_grabs;
static LiVESWidget **mode_radios;
static LiVESWidget **combos;
static LiVESWidget **combo_entries;
static LiVESWidget *dummy_radio;
static LiVESWidget **nlabels;
static LiVESWidget **type_labels;
static LiVESWidget **param_buttons;
static LiVESWidget **conx_buttons;
static LiVESWidget **clear_buttons;
static LiVESWidget **info_buttons;
static LiVESWidget *clear_all_button;
static LiVESWidget *save_keymap_button;
static LiVESWidget *load_keymap_button;

static LiVESWidget *datacon_dialog = NULL;

static ulong *ch_fns;
static ulong *gr_fns;
static ulong *mode_ra_fns;

static int keyw = -1, modew = -1;

static LiVESList *hash_list = NULL;
static LiVESList *name_list = NULL;
static LiVESList *extended_name_list = NULL;

static boolean ca_canc;

static char *empty_string = "";

//////////////////////////////////////////////////////////////////////////////

static void set_param_and_con_buttons(int key, int mode);
static void check_clear_all_button(void);

static boolean rte_window_is_hidden = TRUE;


boolean rte_window_hidden(void) {
  return rte_window_is_hidden;
}


void rte_window_set_interactive(boolean interactive) {
  register int i, j;
  int modes = rte_getmodespk();
  int idx;

  if (!interactive) {
    lives_widget_set_sensitive(clear_all_button, FALSE);
    lives_widget_set_sensitive(save_keymap_button, FALSE);
    lives_widget_set_sensitive(load_keymap_button, FALSE);
    for (i = 0; i < prefs->rte_keys_virtual; i++) {
      for (j = modes - 1; j >= 0; j--) {
        idx = i * modes + j;
        lives_widget_set_sensitive(conx_buttons[idx], FALSE);
        lives_widget_set_sensitive(param_buttons[idx], FALSE);
        lives_widget_set_sensitive(combos[idx], FALSE);
        lives_widget_set_sensitive(clear_buttons[idx], FALSE);
        lives_widget_set_sensitive(mode_radios[idx], FALSE);
      }
      lives_widget_set_sensitive(key_checks[i], FALSE);
      lives_widget_set_sensitive(key_grabs[i], FALSE);
    }
  } else {
    for (i = 0; i < prefs->rte_keys_virtual; i++) {
      for (j = modes - 1; j >= 0; j--) {
        idx = i * modes + j;
        set_param_and_con_buttons(i, j);
      }
    }
    check_clear_all_button();
    lives_widget_set_sensitive(save_keymap_button, TRUE);
    lives_widget_set_sensitive(load_keymap_button, TRUE);
  }
}


void rtew_set_key_check_state(void) {
  // set (delayed) keycheck state
  register int i;
  for (i = 0; i < prefs->rte_keys_virtual; i++) {
    lives_signal_handler_block(key_checks[i], ch_fns[i]);
    lives_toggle_button_set_active(LIVES_TOGGLE_BUTTON(key_checks[i]),
                                   LIVES_POINTER_TO_INT(lives_widget_object_get_data(LIVES_WIDGET_OBJECT(key_checks[i]), "active")));
    lives_signal_handler_unblock(key_checks[i], ch_fns[i]);
  }
}


void type_label_set_text(int key, int mode) {
  int modes = rte_getmodespk();
  int idx = key * modes + mode;
  char *type = rte_keymode_get_type(key + 1, mode), *tmp;

  if (*type) {
    lives_label_set_text(LIVES_LABEL(type_labels[idx]), (tmp = lives_strdup_printf(_("Type: %s"), type)));
    lives_free(tmp);
    lives_widget_set_sensitive(info_buttons[idx], TRUE);
    lives_widget_set_sensitive(clear_buttons[idx], TRUE);
    lives_widget_set_sensitive(mode_radios[idx], TRUE);
    lives_widget_set_sensitive(nlabels[idx], TRUE);
    lives_widget_set_sensitive(type_labels[idx], TRUE);
  } else {
    lives_widget_set_sensitive(info_buttons[idx], FALSE);
    lives_widget_set_sensitive(clear_buttons[idx], FALSE);
    lives_widget_set_sensitive(mode_radios[idx], FALSE);
    lives_widget_set_sensitive(nlabels[idx], FALSE);
    lives_widget_set_sensitive(type_labels[idx], FALSE);
  }
  lives_free(type);
}


boolean on_clear_all_clicked(LiVESButton *button, livespointer user_data) {
  int modes = rte_getmodespk();
  register int i, j;

  ca_canc = FALSE;

  // prompt for "are you sure ?"
  if (user_data != NULL) if (!do_warning_dialog(_("\n\nUnbind all effects from all keys/modes.\n\nAre you sure ?\n\n"))) {
      ca_canc = TRUE;
      return FALSE;
    }

  pconx_delete_all();
  cconx_delete_all();

  for (i = 0; i < prefs->rte_keys_virtual; i++) {
    if (rte_window != NULL) {
      lives_toggle_button_set_active(LIVES_TOGGLE_BUTTON(key_checks[i]), FALSE);
      lives_widget_object_set_data(LIVES_WIDGET_OBJECT(key_checks[i]), "active", LIVES_INT_TO_POINTER(FALSE));
    }
    for (j = modes - 1; j >= 0; j--) {
      weed_delete_effectkey(i + 1, j);
      if (rte_window != NULL) {
        rtew_combo_set_text(i, j, "");
      }
    }
    if (mainw->ce_thumbs) ce_thumbs_reset_combo(i);
  }

  if (button != NULL) lives_widget_set_sensitive(LIVES_WIDGET(button), FALSE);

  return FALSE;
}


static boolean save_keymap2_file(char *kfname) {
  // save perkey defaults
  int slen;
  int version = 1;
  int modes = rte_getmodespk();
  int kfd;
  int retval;
  int i, j;

  char *hashname, *tmp;

  do {
    retval = 0;
    kfd = lives_create_buffered(kfname, DEF_FILE_PERMS);
    if (kfd == -1) {
      retval = do_write_failed_error_s_with_retry(kfname, lives_strerror(errno), LIVES_WINDOW(rte_window));
    } else {
      lives_write_le_buffered(kfd, &version, 4, TRUE);
      for (i = 1; i <= prefs->rte_keys_virtual; i++) {
        if (THREADVAR(write_failed) == kfd + 1) break;
        for (j = 0; j < modes; j++) {
          if (rte_keymode_valid(i, j, TRUE)) {
            lives_write_le_buffered(kfd, &i, 4, TRUE);
            if (THREADVAR(write_failed) == kfd + 1) break;
            // TODO: use newer version with separator
            hashname = lives_strdup_printf("Weed%s",
                                           (tmp = make_weed_hashname(rte_keymode_get_filter_idx(i, j), TRUE, FALSE, 0, FALSE)));
            lives_free(tmp);
            slen = lives_strlen(hashname);
            lives_write_le_buffered(kfd, &slen, 4, TRUE);
            lives_write_buffered(kfd, hashname, slen, TRUE);
            lives_free(hashname);
            write_key_defaults(kfd, i - 1, j);
          }
        }
      }

      lives_close_buffered(kfd);

      if (THREADVAR(write_failed) == kfd + 1) {
        THREADVAR(write_failed) = 0;
        retval = do_write_failed_error_s_with_retry(kfname, NULL, LIVES_WINDOW(rte_window));
      }
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  if (retval == LIVES_RESPONSE_CANCEL) return FALSE;
  return TRUE;
}


static boolean save_keymap3_file(char *kfname) {
  // save data connections

  char *hashname;

  int version = 1;

  int slen;
  int kfd;
  int retval;
  int count = 0, totcons, nconns;

  register int i, j;

  do {
    retval = 0;
    kfd = lives_create_buffered(kfname, DEF_FILE_PERMS);
    if (kfd == -1) {
      retval = do_write_failed_error_s_with_retry(kfname, lives_strerror(errno), LIVES_WINDOW(rte_window));
    } else {
      THREADVAR(write_failed) = FALSE;

      lives_write_le_buffered(kfd, &version, 4, TRUE);

      if (mainw->cconx != NULL) {
        lives_cconnect_t *cconx = mainw->cconx;
        int nchans;

        while (cconx != NULL) {
          count++;
          cconx = cconx->next;
        }

        lives_write_le_buffered(kfd, &count, 4, TRUE);
        if (THREADVAR(write_failed)) goto write_failed1;

        cconx = mainw->cconx;
        while (cconx != NULL) {
          totcons = 0;
          j = 0;

          lives_write_le_buffered(kfd, &cconx->okey, 4, TRUE);
          if (THREADVAR(write_failed)) goto write_failed1;

          lives_write_le_buffered(kfd, &cconx->omode, 4, TRUE);
          if (THREADVAR(write_failed)) goto write_failed1;

          // TODO: use newer version with separator
          hashname = make_weed_hashname(rte_keymode_get_filter_idx(cconx->okey + 1, cconx->omode), TRUE, FALSE, 0, FALSE);
          slen = lives_strlen(hashname);
          lives_write_le_buffered(kfd, &slen, 4, TRUE);
          lives_write_buffered(kfd, hashname, slen, TRUE);
          lives_free(hashname);

          nchans = cconx->nchans;
          lives_write_le_buffered(kfd, &nchans, 4, TRUE);
          if (THREADVAR(write_failed)) goto write_failed1;

          for (i = 0; i < nchans; i++) {
            lives_write_le_buffered(kfd, &cconx->chans[i], 4, TRUE);
            if (THREADVAR(write_failed)) goto write_failed1;

            nconns = cconx->nconns[i];
            lives_write_le_buffered(kfd, &nconns, 4, TRUE);
            if (THREADVAR(write_failed)) goto write_failed1;

            totcons += nconns;

            while (j < totcons) {
              lives_write_le_buffered(kfd, &cconx->ikey[j], 4, TRUE);
              if (THREADVAR(write_failed)) goto write_failed1;

              lives_write_le_buffered(kfd, &cconx->imode[j], 4, TRUE);
              if (THREADVAR(write_failed)) goto write_failed1;

              // TODO: use newer version with separator
              hashname =
                make_weed_hashname(rte_keymode_get_filter_idx(cconx->ikey[j] + 1, cconx->imode[j]), TRUE, FALSE, 0, FALSE);
              slen = lives_strlen(hashname);
              lives_write_le_buffered(kfd, &slen, 4, TRUE);
              lives_write_buffered(kfd, hashname, slen, TRUE);
              lives_free(hashname);

              lives_write_le_buffered(kfd, &cconx->icnum[j], 4, TRUE);
              if (THREADVAR(write_failed)) goto write_failed1;

              j++;
            }
          }

          cconx = cconx->next;
        }
      } else {
        lives_write_le_buffered(kfd, &count, 4, TRUE);
        if (THREADVAR(write_failed)) goto write_failed1;
      }

      if (mainw->pconx != NULL) {
        lives_pconnect_t *pconx = mainw->pconx;

        int nparams;

        count = 0;

        while (pconx != NULL) {
          count++;
          pconx = pconx->next;
        }

        lives_write_le_buffered(kfd, &count, 4, TRUE);
        if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;

        pconx = mainw->pconx;
        while (pconx != NULL) {
          totcons = 0;
          j = 0;

          lives_write_le_buffered(kfd, &pconx->okey, 4, TRUE);
          if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;

          lives_write_le_buffered(kfd, &pconx->omode, 4, TRUE);
          if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;

          // TODO: use newer version with separator
          hashname =
            make_weed_hashname(rte_keymode_get_filter_idx(pconx->okey + 1, pconx->omode), TRUE, FALSE, 0, FALSE);
          slen = lives_strlen(hashname);
          lives_write_le_buffered(kfd, &slen, 4, TRUE);
          lives_write_buffered(kfd, hashname, slen, TRUE);
          lives_free(hashname);

          nparams = pconx->nparams;
          lives_write_le_buffered(kfd, &nparams, 4, TRUE);
          if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;

          for (i = 0; i < nparams; i++) {
            lives_write_le_buffered(kfd, &pconx->params[i], 4, TRUE);
            if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;

            nconns = pconx->nconns[i];
            lives_write_le_buffered(kfd, &nconns, 4, TRUE);
            if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;

            totcons += nconns;

            while (j < totcons) {
              lives_write_le_buffered(kfd, &pconx->ikey[j], 4, TRUE);
              if (THREADVAR(write_failed)) goto write_failed1;

              lives_write_le_buffered(kfd, &pconx->imode[j], 4, TRUE);
              if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;

              // TODO: use newer version with separator
              hashname =
                make_weed_hashname(rte_keymode_get_filter_idx(pconx->ikey[j] + 1, pconx->imode[j]), TRUE, FALSE, 0, FALSE);
              slen = lives_strlen(hashname);
              lives_write_le_buffered(kfd, &slen, 4, TRUE);
              lives_write_buffered(kfd, hashname, slen, TRUE);
              lives_free(hashname);

              lives_write_le_buffered(kfd, &pconx->ipnum[j], 4, TRUE);
              if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;

              lives_write_le_buffered(kfd, &pconx->autoscale[j], 4, TRUE);
              if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;

              j++;
            }
          }

          pconx = pconx->next;
        }
      } else {
        lives_write_le_buffered(kfd, &count, 4, TRUE);
        if (THREADVAR(write_failed) == kfd + 1) goto write_failed1;
      }

write_failed1:
      lives_close_buffered(kfd);

      if (THREADVAR(write_failed) == kfd + 1) {
        THREADVAR(write_failed) = 0;
        retval = do_write_failed_error_s_with_retry(kfname, NULL, LIVES_WINDOW(rte_window));
      }
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  if (retval == LIVES_RESPONSE_CANCEL) return FALSE;
  return TRUE;
}


static boolean on_save_keymap_clicked(LiVESButton *button, livespointer user_data) {
  // save as keymap type 1 file - to allow backwards compatibility with older versions of LiVES
  // default.keymap

  // format is text
  // key|hashname

  // if we have per key defaults, then we add an extra file:
  // default.keymap2

  // format is binary
  // (4 bytes key) (4 bytes hlen) (hlen bytes hashname) then a dump of the weed plant default values

  // if we have data connections, we will save a third file

  FILE *kfile;
  LiVESList *list = NULL;

  char *msg, *tmp;
  char *keymap_file = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, "default.keymap", NULL);
  char *keymap_file2 = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, "default.keymap2", NULL);
  char *keymap_file3 = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, "default.keymap3", NULL);

  boolean update = FALSE;

  int modes = rte_getmodespk();
  int i, j;
  int retval;

  if (button != NULL) {
    if (!do_warning_dialog(_("\n\nClick 'OK' to save this keymap as your default\n\n"))) {
      lives_free(keymap_file3);
      lives_free(keymap_file2);
      lives_free(keymap_file);
      return FALSE;
    }
    d_print(_("Saving keymap to %s\n"), keymap_file);
  } else {
    update = TRUE;
    list = (LiVESList *)user_data;
    if (list == NULL) return FALSE;
    d_print(_("\nUpdating keymap file %s..."), keymap_file);
  }

  do {
    retval = 0;
    if (!(kfile = fopen(keymap_file, "w"))) {
      msg = lives_strdup_printf(_("\n\nUnable to write keymap file\n%s\nError was %s\n"), keymap_file, lives_strerror(errno));
      retval = do_abort_cancel_retry_dialog(msg, LIVES_WINDOW(rte_window));
      lives_free(msg);
    } else {
      lives_fputs("LiVES keymap file version 4\n", kfile);

      if (!update) {
        for (i = 1; i <= prefs->rte_keys_virtual; i++) {
          for (j = 0; j < modes; j++) {
            if (rte_keymode_valid(i, j, TRUE)) {
              // TODO: use newer version with separator
              lives_fputs(lives_strdup_printf("%d|Weed%s\n", i,
                                              (tmp = make_weed_hashname(rte_keymode_get_filter_idx(i, j), TRUE, FALSE, 0, FALSE))), kfile);
              lives_free(tmp);
            }
          }
        }
      } else {
        for (i = 0; i < lives_list_length(list); i++) {
          lives_fputs((char *)lives_list_nth_data(list, i), kfile);
        }
      }

      fclose(kfile);
    }

    if (THREADVAR(write_failed)) {
      retval = do_write_failed_error_s_with_retry(keymap_file, NULL, LIVES_WINDOW(rte_window));
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  // if we have default values, save them
  if (1 || has_key_defaults()) {
    if (!save_keymap2_file(keymap_file2)) {
      lives_rm(keymap_file2);
      retval = LIVES_RESPONSE_CANCEL;
    }
  } else lives_rm(keymap_file2);

  // if we have data connections, save them
  if (mainw->pconx != NULL || mainw->cconx != NULL) {
    if (!save_keymap3_file(keymap_file3)) {
      lives_rm(keymap_file3);
      retval = LIVES_RESPONSE_CANCEL;
    }
  } else lives_rm(keymap_file3);

  lives_free(keymap_file3);
  lives_free(keymap_file2);
  lives_free(keymap_file);

  if (retval == LIVES_RESPONSE_CANCEL) d_print_file_error_failed();
  else d_print_done();

  return FALSE;
}


void on_save_rte_defs_activate(LiVESMenuItem *menuitem, livespointer user_data) {
  char *msg;

  int fd;
  int retval;
  int numfx;

  register int i;

  if (prefs->fxdefsfile == NULL) {
    prefs->fxdefsfile = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, FX_DEFS_FILENAME, NULL);
  }

  if (prefs->fxsizesfile == NULL) {
    prefs->fxsizesfile = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, FX_SIZES_FILENAME, NULL);
  }

  d_print(_("Saving real time effect defaults to %s..."), prefs->fxdefsfile);

  numfx = rte_get_numfilters();

  do {
    do_threaded_dialog(_("Saving real time effect defaults..."), FALSE);
    retval = LIVES_RESPONSE_NONE;
    if ((fd = lives_create_buffered(prefs->fxdefsfile, DEF_FILE_PERMS)) == -1) {
      end_threaded_dialog();
      msg = lives_strdup_printf(_("\n\nUnable to write defaults file\n%s\nError code %d\n"), prefs->fxdefsfile, errno);
      retval = do_abort_cancel_retry_dialog(msg, LIVES_WINDOW(rte_window));
      lives_free(msg);
    } else {
      msg = lives_strdup_printf("%s\n", FX_DEFS_VERSIONSTRING_1_1);
      lives_write_buffered(fd, msg, lives_strlen(msg), TRUE);
      lives_free(msg);

      if (THREADVAR(write_failed) == fd + 1) {
        THREADVAR(write_failed) = 0;
        end_threaded_dialog();
        retval = do_write_failed_error_s_with_retry(prefs->fxdefsfile, NULL, LIVES_WINDOW(rte_window));
      } else {
        // break on file write error
        for (i = 0; i < numfx; i++) {
          if (!write_filter_defaults(fd, i)) {
            end_threaded_dialog();
            retval = do_write_failed_error_s_with_retry(prefs->fxdefsfile, NULL, LIVES_WINDOW(rte_window));
            break;
          }
        }
      }
      lives_close_buffered(fd);
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  if (retval == LIVES_RESPONSE_CANCEL) {
    d_print_file_error_failed();
    return;
  }

  threaded_dialog_spin(0.);

  do {
    retval = LIVES_RESPONSE_NONE;
    if ((fd = lives_create_buffered(prefs->fxsizesfile, DEF_FILE_PERMS)) == -1) {
      end_threaded_dialog();
      retval = do_write_failed_error_s_with_retry(prefs->fxsizesfile, lives_strerror(errno), LIVES_WINDOW(rte_window));
      lives_free(msg);
    } else {
      msg = lives_strdup_printf("%s\n", FX_SIZES_VERSIONSTRING_2);
      lives_write_buffered(fd, msg, lives_strlen(msg), TRUE);
      lives_free(msg);
      if (THREADVAR(write_failed) == fd + 1) {
        THREADVAR(write_failed) = 0;
        end_threaded_dialog();
        retval = do_write_failed_error_s_with_retry(prefs->fxsizesfile, NULL, LIVES_WINDOW(rte_window));
      } else {
        for (i = 0; i < numfx; i++) {
          if (!write_generator_sizes(fd, i)) {
            end_threaded_dialog();
            retval = do_write_failed_error_s_with_retry(prefs->fxsizesfile, NULL, LIVES_WINDOW(rte_window));
            break;
          }
        }
      }
      lives_close_buffered(fd);
    }
    if (retval == LIVES_RESPONSE_RETRY) do_threaded_dialog(_("Saving real time effect defaults..."), FALSE);
  } while (retval == LIVES_RESPONSE_RETRY);

  if (retval == LIVES_RESPONSE_CANCEL) {
    d_print_file_error_failed();
    return;
  }

  d_print_done();
  end_threaded_dialog();

  return;
}


void load_rte_defs(void) {
  ssize_t bytes;
  char *buf, *msg;
  size_t msglen;
  int fd;
  int retval;

  if (prefs->fxdefsfile == NULL) {
    prefs->fxdefsfile = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, FX_DEFS_FILENAME, NULL);
  }

  if (lives_file_test(prefs->fxdefsfile, LIVES_FILE_TEST_EXISTS)) {
    do {
      retval = 0;
      if ((fd = lives_open_buffered_rdonly(prefs->fxdefsfile)) == -1) {
        retval = do_read_failed_error_s_with_retry(prefs->fxdefsfile, lives_strerror(errno), NULL);
      } else {
        THREADVAR(read_failed) = FALSE;
        d_print(_("Loading real time effect defaults from %s..."), prefs->fxdefsfile);

        msg = lives_strdup_printf("%s\n", FX_DEFS_VERSIONSTRING_1_1);
        msglen = lives_strlen(msg);
        buf = lives_malloc(msglen + 1);
        bytes = lives_read_buffered(fd, buf, msglen, TRUE);
        if (bytes == (ssize_t)msglen && !lives_strncmp((char *)buf, msg, msglen)) {
          if (read_filter_defaults(fd)) {
            d_print_done();
          } else {
            d_print_file_error_failed();
            retval = do_read_failed_error_s_with_retry(prefs->fxdefsfile, NULL, NULL);
          }
        } else {
          d_print_file_error_failed();
          if (bytes < (ssize_t)msglen) {
            retval = do_read_failed_error_s_with_retry(prefs->fxdefsfile, NULL, NULL);
          }
        }

        lives_close_buffered(fd);

        lives_free(buf);
        lives_free(msg);
      }
    } while (retval == LIVES_RESPONSE_RETRY);
  }

  if (prefs->fxsizesfile == NULL) {
    prefs->fxsizesfile = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, FX_SIZES_FILENAME, NULL);
  }

  if (lives_file_test(prefs->fxsizesfile, LIVES_FILE_TEST_EXISTS)) {
    do {
      retval = 0;
      if ((fd = lives_open_buffered_rdonly(prefs->fxsizesfile)) == -1) {
        retval = do_read_failed_error_s_with_retry(prefs->fxsizesfile, lives_strerror(errno), NULL);
        if (retval == LIVES_RESPONSE_CANCEL) return;
      } else {
        d_print(_("Loading generator default sizes from %s..."), prefs->fxsizesfile);

        msg = lives_strdup_printf("%s\n", FX_SIZES_VERSIONSTRING_2);
        msglen = lives_strlen(msg);
        buf = lives_malloc(msglen + 1);
        bytes = lives_read_buffered(fd, buf, msglen, TRUE);
        if (bytes == (ssize_t)msglen && !strncmp((char *)buf, msg, msglen)) {
          if (read_generator_sizes(fd)) {
            d_print_done();
          } else {
            d_print_file_error_failed();
            retval = do_read_failed_error_s_with_retry(prefs->fxsizesfile, NULL, NULL);
          }
        } else {
          d_print_file_error_failed();
          if (bytes < (ssize_t)msglen) {
            retval = do_read_failed_error_s_with_retry(prefs->fxsizesfile, NULL, NULL);
          }
        }
        lives_close_buffered(fd);

        lives_free(buf);
        lives_free(msg);
      }
    } while (retval == LIVES_RESPONSE_RETRY);
  }

  return;
}


static void check_clear_all_button(void) {
  int modes = rte_getmodespk();
  int i, j;
  boolean hasone = FALSE;

  for (i = 0; i < prefs->rte_keys_virtual; i++) {
    for (j = modes - 1; j >= 0; j--) {
      if (rte_keymode_valid(i + 1, j, TRUE)) hasone = TRUE;
    }
  }

  lives_widget_set_sensitive(LIVES_WIDGET(clear_all_button), hasone);
}


static boolean read_perkey_defaults(int kfd, int key, int mode, int version) {
  boolean ret = TRUE;
  int nparams;
  ssize_t bytes = lives_read_le_buffered(kfd, &nparams, 4, TRUE);

  if (nparams > 65536) {
    lives_printerr("Too many params, file is probably broken.\n");
    return FALSE;
  }

  if (bytes < sizint) {
    return FALSE;
  }

  if (nparams > 0) {
    ret = read_key_defaults(kfd, nparams, key, mode, version);
  }
  return ret;
}


static boolean load_datacons(const char *fname, uint8_t **badkeymap) {
  weed_plant_t **ochans, **ichans, **iparams;

  weed_plant_t *filter;

  ssize_t bytes;

  char *hashname;

  boolean ret = TRUE;
  boolean is_valid, is_valid2;
  boolean eof = FALSE;

  int kfd;

  int retval;

  int hlen;

  int version, nchans, nparams, nconns, ncconx, npconx;

  int nichans, nochans, niparams, noparams, error;

  int okey, omode, ocnum, opnum, ikey, imode, icnum, ipnum, autoscale;

  int maxmodes = rte_getmodespk();

  register int i, j, k, count;

  do {
    retval = 0;
    if ((kfd = lives_open_buffered_rdonly(fname)) == -1) {
      retval = do_read_failed_error_s_with_retry(fname, lives_strerror(errno), NULL);
    } else {
      THREADVAR(read_failed) = FALSE;

      bytes = lives_read_le_buffered(kfd, &version, 4, TRUE);
      if (bytes < 4) {
        eof = TRUE;
        break;
      }

      bytes = lives_read_le_buffered(kfd, &ncconx, 4, TRUE);
      if (bytes < 4) {
        eof = TRUE;
        break;
      }

      for (count = 0; count < ncconx; count++) {
        is_valid = TRUE;

        bytes = lives_read_le_buffered(kfd, &okey, 4, TRUE);
        if (bytes < 4) {
          eof = TRUE;
          break;
        }

        if (okey < 0 || okey >= prefs->rte_keys_virtual) is_valid = FALSE;

        bytes = lives_read_le_buffered(kfd, &omode, 4, TRUE);
        if (bytes < 4) {
          eof = TRUE;
          break;
        }

        bytes = lives_read_le_buffered(kfd, &hlen, 4, TRUE);
        if (bytes < 4) {
          eof = TRUE;
          break;
        }

        hashname = (char *)lives_malloc(hlen + 1);

        if (hashname == NULL) {
          eof = TRUE;
          break;
        }

        bytes = lives_read_buffered(kfd, hashname, hlen, TRUE);
        if (bytes < hlen) {
          eof = TRUE;
          lives_free(hashname);
          break;
        }

        lives_memset(hashname + hlen, 0, 1);

        if (omode < 0 || omode >= maxmodes) is_valid = FALSE;

        if (is_valid) {
          // if we had bad/missing fx, adjust the omode value
          for (i = 0; i < omode; i++) omode -= badkeymap[okey][omode];
        }

        if (omode < 0 || omode >= maxmodes) is_valid = FALSE;

        if (is_valid) {
          int fidx = rte_keymode_get_filter_idx(okey + 1, omode);
          if (fidx == -1) is_valid = FALSE;
          else {
            // TODO: use newer version with separator
            char *hashname2 = make_weed_hashname(fidx, TRUE, FALSE, 0, FALSE);
            if (strcmp(hashname, hashname2)) is_valid = FALSE;
            lives_free(hashname2);
            if (!is_valid) {
              hashname2 = make_weed_hashname(fidx, TRUE, TRUE, 0, FALSE);
              if (!strcmp(hashname, hashname2)) is_valid = TRUE;
              lives_free(hashname2);
            }
          }
        }

        lives_free(hashname);

        bytes = lives_read_le_buffered(kfd, &nchans, 4, TRUE);
        if (bytes < 4) {
          eof = TRUE;
          break;
        }

        for (i = 0; i < nchans; i++) {
          is_valid2 = is_valid;

          bytes = lives_read_le_buffered(kfd, &ocnum, 4, TRUE);
          if (bytes < 4) {
            eof = TRUE;
            break;
          }

          // check ocnum
          filter = rte_keymode_get_filter(okey + 1, omode);
          nochans = weed_leaf_num_elements(filter, WEED_LEAF_OUT_CHANNEL_TEMPLATES);
          if (ocnum >= nochans) is_valid2 = FALSE;
          else {
            ochans = weed_get_plantptr_array(filter, WEED_LEAF_OUT_CHANNEL_TEMPLATES, &error);
            if (!has_alpha_palette(ochans[ocnum], filter)) is_valid2 = FALSE;
            lives_free(ochans);
          }

          bytes = lives_read_le_buffered(kfd, &nconns, 4, TRUE);
          if (bytes < 4) {
            eof = TRUE;
            break;
          }

          for (j = 0; j < nconns; j++) {
            bytes = lives_read_le_buffered(kfd, &ikey, 4, TRUE);
            if (bytes < 4) {
              eof = TRUE;
              break;
            }

            bytes = lives_read_le_buffered(kfd, &imode, 4, TRUE);
            if (bytes < 4) {
              eof = TRUE;
              break;
            }

            bytes = lives_read_le_buffered(kfd, &hlen, 4, TRUE);
            if (bytes < 4) {
              eof = TRUE;
              break;
            }

            hashname = (char *)lives_malloc(hlen + 1);

            if (hashname == NULL) {
              eof = TRUE;
              break;
            }

            bytes = lives_read_buffered(kfd, hashname, hlen, TRUE);
            if (bytes < hlen) {
              eof = TRUE;
              lives_free(hashname);
              break;
            }

            lives_memset(hashname + hlen, 0, 1);

            if (imode < 0 || (ikey >= 0 && imode >= maxmodes)) is_valid2 = FALSE;

            if (is_valid2) {
              // if we had bad/missing fx, adjust the omode value
              for (k = 0; k < imode; k++) imode -= badkeymap[ikey][imode];
            }

            if (imode < 0 || (ikey >= 0 && imode >= maxmodes)) is_valid2 = FALSE;

            if (is_valid2) {
              int fidx = rte_keymode_get_filter_idx(ikey + 1, imode);
              if (fidx == -1) is_valid2 = FALSE;
              else {
                // TODO: use newer version with separator
                char *hashname2 = make_weed_hashname(fidx, TRUE, FALSE, 0, FALSE);
                if (strcmp(hashname, hashname2)) is_valid2 = FALSE;
                lives_free(hashname2);
                if (!is_valid2) {
                  hashname2 = make_weed_hashname(fidx, TRUE, TRUE, 0, FALSE);
                  if (!strcmp(hashname, hashname2)) is_valid2 = TRUE;
                  lives_free(hashname2);
                }
              }
            }

            lives_free(hashname);

            bytes = lives_read_le_buffered(kfd, &icnum, 4, TRUE);
            if (bytes < 4) {
              eof = TRUE;
              break;
            }

            // check icnum
            filter = rte_keymode_get_filter(ikey + 1, imode);
            nichans = weed_leaf_num_elements(filter, WEED_LEAF_IN_CHANNEL_TEMPLATES);
            if (icnum >= nichans) is_valid2 = FALSE;
            else {
              ichans = weed_get_plantptr_array(filter, WEED_LEAF_IN_CHANNEL_TEMPLATES, &error);
              if (!has_alpha_palette(ichans[icnum], filter)) is_valid2 = FALSE;
              lives_free(ichans);
            }

            if (is_valid2) cconx_add_connection(okey, omode, ocnum, ikey, imode, icnum);
          }

          if (eof) break;
        }

        if (eof) break;
      }

      if (eof) break;

      // params

      bytes = lives_read_le_buffered(kfd, &npconx, 4, TRUE);
      if (bytes < 4) {
        eof = TRUE;
        break;
      }

      for (count = 0; count < npconx; count++) {
        is_valid = TRUE;

        bytes = lives_read_le_buffered(kfd, &okey, 4, TRUE);
        if (bytes < 4) {
          eof = TRUE;
          break;
        }

        if (okey < 0 || okey >= prefs->rte_keys_virtual) is_valid = FALSE;

        bytes = lives_read_le_buffered(kfd, &omode, 4, TRUE);
        if (bytes < 4) {
          eof = TRUE;
          break;
        }

        bytes = lives_read_le_buffered(kfd, &hlen, 4, TRUE);
        if (bytes < 4) {
          eof = TRUE;
          break;
        }

        hashname = (char *)lives_malloc(hlen + 1);

        if (hashname == NULL) {
          eof = TRUE;
          break;
        }

        bytes = lives_read_buffered(kfd, hashname, hlen, TRUE);
        if (bytes < hlen) {
          eof = TRUE;
          lives_free(hashname);
          break;
        }

        lives_memset(hashname + hlen, 0, 1);

        if (omode < 0 || omode >= maxmodes) is_valid = FALSE;

        if (is_valid) {
          // if we had bad/missing fx, adjust the omode value
          for (i = 0; i < omode; i++) omode -= badkeymap[okey][omode];
        }

        if (omode < 0 || omode >= maxmodes) is_valid = FALSE;

        if (is_valid) {
          int fidx = rte_keymode_get_filter_idx(okey + 1, omode);
          if (fidx == -1) is_valid = FALSE;
          else {
            // TODO: use newer version with separator
            char *hashname2 = make_weed_hashname(fidx, TRUE, FALSE, 0, FALSE);
            if (strcmp(hashname, hashname2)) is_valid = FALSE;
            lives_free(hashname2);
            if (!is_valid) {
              hashname2 = make_weed_hashname(fidx, TRUE, TRUE, 0, FALSE);
              if (!strcmp(hashname, hashname2)) is_valid = TRUE;
              lives_free(hashname2);
            }
          }
        }

        lives_free(hashname);

        bytes = lives_read_le_buffered(kfd, &nparams, 4, TRUE);
        if (bytes < 4) {
          eof = TRUE;
          break;
        }

        for (i = 0; i < nparams; i++) {
          is_valid2 = is_valid;

          bytes = lives_read_le_buffered(kfd, &opnum, 4, TRUE);
          if (bytes < 4) {
            eof = TRUE;
            break;
          }

          // check opnum
          filter = rte_keymode_get_filter(okey + 1, omode);
          noparams = weed_leaf_num_elements(filter, WEED_LEAF_OUT_PARAMETER_TEMPLATES);
          if (opnum >= noparams) is_valid2 = FALSE;

          bytes = lives_read_le_buffered(kfd, &nconns, 4, TRUE);
          if (bytes < 4) {
            eof = TRUE;
            break;
          }

          for (j = 0; j < nconns; j++) {
            bytes = lives_read_le_buffered(kfd, &ikey, 4, TRUE);
            if (bytes < 4) {
              eof = TRUE;
              break;
            }

            bytes = lives_read_le_buffered(kfd, &imode, 4, TRUE);
            if (bytes < 4) {
              eof = TRUE;
              break;
            }

            bytes = lives_read_le_buffered(kfd, &hlen, 4, TRUE);
            if (bytes < 4) {
              eof = TRUE;
              break;
            }

            hashname = (char *)lives_malloc(hlen + 1);

            if (hashname == NULL) {
              eof = TRUE;
              break;
            }

            bytes = lives_read_buffered(kfd, hashname, hlen, TRUE);
            if (bytes < hlen) {
              eof = TRUE;
              lives_free(hashname);
              break;
            }

            lives_memset(hashname + hlen, 0, 1);

            if (imode < 0 || (ikey >= 0 && imode >= maxmodes)) is_valid2 = FALSE;

            if (is_valid2 && ikey >= 0) {
              // if we had bad/missing fx, adjust the omode value
              for (k = 0; k < imode; k++) imode -= badkeymap[ikey][imode];
            }

            if (imode < 0 || (ikey >= 0 && imode >= maxmodes)) is_valid2 = FALSE;

            if (is_valid2 && ikey >= 0) {
              int fidx = rte_keymode_get_filter_idx(ikey + 1, imode);
              if (fidx == -1) is_valid2 = FALSE;
              else {
                // TODO: use newer version with separator
                char *hashname2 = make_weed_hashname(fidx, TRUE, FALSE, 0, FALSE);
                if (strcmp(hashname, hashname2)) is_valid2 = FALSE;
                lives_free(hashname2);
                if (!is_valid2) {
                  hashname2 = make_weed_hashname(fidx, TRUE, TRUE, 0, FALSE);
                  if (!strcmp(hashname, hashname2)) is_valid2 = TRUE;
                  lives_free(hashname2);
                }
              }
            }

            lives_free(hashname);

            bytes = lives_read_le_buffered(kfd, &ipnum, 4, TRUE);
            if (bytes < 4) {
              eof = TRUE;
              break;
            }

            if (ikey >= 0) {
              // check ipnum
              filter = rte_keymode_get_filter(ikey + 1, imode);
              niparams = weed_leaf_num_elements(filter, WEED_LEAF_IN_PARAMETER_TEMPLATES);
              if (ipnum >= niparams) is_valid2 = FALSE;
              else {
                if (ipnum >= 0) {
                  iparams = weed_get_plantptr_array(filter, WEED_LEAF_IN_PARAMETER_TEMPLATES, &error);
                  if (weed_plant_has_leaf(iparams[ipnum], WEED_LEAF_HOST_INTERNAL_CONNECTION)) is_valid2 = FALSE;
                  lives_free(iparams);
                }
              }
            }

            bytes = lives_read_le_buffered(kfd, &autoscale, 4, TRUE);
            if (bytes < 4) {
              eof = TRUE;
              break;
            }

            if (is_valid2) {
              pconx_add_connection(okey, omode, opnum, ikey, imode, ipnum, autoscale);
            }
          }

          if (eof) break;
        }

        if (eof) break;
      }

      lives_close_buffered(kfd);
    }
  } while (retval == LIVES_RESPONSE_RETRY);

  if (retval == LIVES_RESPONSE_CANCEL) {
    d_print_cancelled();
    return FALSE;
  }

  return ret;
}


static void set_param_and_con_buttons(int key, int mode) {
  weed_plant_t *filter = rte_keymode_get_filter(key + 1, mode);

  int modes = rte_getmodespk();
  int idx = key * modes + mode;

  if (filter != NULL) {
    lives_widget_set_sensitive(conx_buttons[idx], TRUE);
    if (num_in_params(filter, TRUE, TRUE) > 0) lives_widget_set_sensitive(param_buttons[idx], TRUE);
    else lives_widget_set_sensitive(param_buttons[idx], FALSE);
    lives_widget_set_sensitive(combos[idx], TRUE);
    if (combos[idx + 1] != NULL && mode < modes - 1) lives_widget_set_sensitive(combos[idx + 1], TRUE);
  } else {
    lives_widget_set_sensitive(conx_buttons[idx], FALSE);
    lives_widget_set_sensitive(param_buttons[idx], FALSE);
    if (mode == 0 || rte_keymode_get_filter(key + 1, mode - 1) != NULL)
      lives_widget_set_sensitive(combos[idx], TRUE);
    else
      lives_widget_set_sensitive(combos[idx], FALSE);
  }

  type_label_set_text(key, mode);
}


boolean on_load_keymap_clicked(LiVESButton *button, livespointer user_data) {
  // show file errors at this level
  FILE *kfile = NULL;

  LiVESList *list = NULL, *new_list = NULL;

  size_t linelen;
  ssize_t bytes;

  char buff[65536];
  char *msg, *tmp;
  char *whole = lives_strdup(""), *whole2;
  char *hashname, *hashname_new = NULL;

  char *line = NULL;
  char *whashname;

  char *keymap_file = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, "default.keymap", NULL); // keymap
  char *keymap_file2 = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, "default.keymap2", NULL); // perkey defs
  char *keymap_file3 = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, "default.keymap3", NULL); // data connections

  int *def_modes = NULL;
  uint8_t **badkeymap = NULL;

  boolean notfound = FALSE;
  boolean has_error = FALSE;
  boolean eof = FALSE;

  int modes = rte_getmodespk();
  int kfd = -1;
  int version;
  int hlen;
  int retval;

  int key, mode;
  int update = 0;

  register int i;

  if (lives_file_test(keymap_file2, LIVES_FILE_TEST_EXISTS)) {
    // per key defaults ?
    lives_free(keymap_file);
    keymap_file = keymap_file2;
  } else {
    lives_freep((void **)&keymap_file2);
  }

  d_print(_("Loading default keymap from %s..."), keymap_file);

  if (!lives_file_test(keymap_file, LIVES_FILE_TEST_EXISTS)) {
    d_print(_("file not found, skipping.\n"));
    lives_freep((void **)&keymap_file);
    lives_freep((void **)&keymap_file2);
    lives_freep((void **)&keymap_file3);
    return FALSE;
  } else {
    do {
      retval = 0;

      if (keymap_file2 != NULL) {
        if ((kfd = lives_open_buffered_rdonly(keymap_file)) < 0) has_error = TRUE;
      } else {
        if (!(kfile = fopen(keymap_file, "r"))) {
          has_error = TRUE;
        }
      }

      if (has_error) {
        msg = lives_strdup_printf(_("\n\nUnable to read from keymap file\n%s\nError code %d\n"), keymap_file, errno);
        retval = do_abort_cancel_retry_dialog(msg, LIVES_WINDOW(rte_window));
        lives_free(msg);

        if (retval == LIVES_RESPONSE_CANCEL) {
          lives_free(keymap_file);
          d_print_file_error_failed();
          return FALSE;
        }
      }
    } while (retval == LIVES_RESPONSE_RETRY);

    on_clear_all_clicked(NULL, user_data);

    if (ca_canc) {
      // user cancelled
      mainw->error = FALSE;
      d_print_cancelled();
      return FALSE;
    }
  }

  def_modes = (int *)lives_malloc(prefs->rte_keys_virtual * sizint);
  for (i = 0; i < prefs->rte_keys_virtual; i++) def_modes[i] = -1;

  badkeymap = (uint8_t **)lives_malloc(prefs->rte_keys_virtual * sizeof(uint8_t *));
  for (i = 0; i < prefs->rte_keys_virtual; i++) {
    badkeymap[i] = (uint8_t *)lives_calloc(modes, 1);
  }

  if (keymap_file2 == NULL) {
    // version 1 file
    while (fgets(buff, 65536, kfile)) {
      if (*buff) {
        line = (lives_strstrip(buff));
        if ((linelen = lives_strlen(line))) {
          whole2 = lives_strconcat(whole, line, NULL);
          if (whole2 != whole) lives_free(whole);
          whole = whole2;
          if (linelen < (size_t)65535) {
            list = lives_list_append(list, lives_strdup(whole));
            lives_free(whole);
            whole = lives_strdup("");
	    // *INDENT-OFF*
          }}}}
    // *INDENT-ON*

    fclose(kfile);

    if (!strcmp((char *)lives_list_nth_data(list, 0), "LiVES keymap file version 2") ||
        !strcmp((char *)lives_list_nth_data(list, 0), "LiVES keymap file version 1")) update = 1;
    else {
      if (!strcmp((char *)lives_list_nth_data(list, 0), "LiVES keymap file version 3")) update = 2;
      else {
        goto cleanup1;
      }
    }
  } else {
    // newer style
    // read version
    bytes = lives_read_le_buffered(kfd, &version, 4, TRUE);
    if (bytes < sizint) {
      eof = TRUE;
    }
  }

  lives_free(whole);

  for (i = 1; (keymap_file2 == NULL && i < lives_list_length(list)) || (keymap_file2 != NULL && !eof); i++) {
    char **array;

    if (keymap_file2 == NULL) {
      // old style

      line = (char *)lives_list_nth_data(list, i);

      if (get_token_count(line, '|') < 2) {
        d_print(_("Invalid line %d in %s\n"), i, keymap_file);
        continue;
      }

      array = lives_strsplit(line, "|", -1);

      if (!strcmp(array[0], "defaults")) {
        lives_strfreev(array);
        array = lives_strsplit(line, "|", 2);
        lives_freep((void **)&prefs->fxdefsfile);
        prefs->fxdefsfile = lives_strdup(array[1]);
        lives_strfreev(array);
        continue;
      }

      if (!strcmp(array[0], "sizes")) {
        lives_strfreev(array);
        array = lives_strsplit(line, "|", 2);
        lives_freep((void **)&prefs->fxsizesfile);
        prefs->fxsizesfile = lives_strdup(array[1]);
        lives_strfreev(array);
        continue;
      }

      key = atoi(array[0]);

      hashname = lives_strdup(array[1]);
      lives_strfreev(array);

      if (update > 0) {
        if (update == 1) hashname_new = lives_strdup_printf("%d|Weed%s1\n", key, hashname);
        if (update == 2) hashname_new = lives_strdup_printf("%d|Weed%s\n", key, hashname);
        new_list = lives_list_append(new_list, hashname_new);
        lives_free(hashname);
        continue;
      }
    } else {
      // newer style
      // file format is: (4 bytes int)key(4 bytes int)hlen(hlen bytes)hashname

      //read key and hashname
      bytes = lives_read_le_buffered(kfd, &key, 4, TRUE);
      if (bytes < 4) {
        eof = TRUE;
        break;
      }

      bytes = lives_read_le_buffered(kfd, &hlen, 4, TRUE);
      if (bytes < 4) {
        eof = TRUE;
        break;
      }

      hashname = (char *)lives_malloc(hlen + 1);

      if (hashname == NULL) {
        eof = TRUE;
        break;
      }

      bytes = lives_read_buffered(kfd, hashname, hlen, TRUE);
      if (bytes < hlen) {
        g_print("keyl %d wanted %d got %ld\n", i, hlen, bytes);
        eof = TRUE;
        lives_free(hashname);
        break;
      }

      lives_memset(hashname + hlen, 0, 1);

      array = lives_strsplit(hashname, "|", -1);
      lives_free(hashname);
      hashname = lives_strdup(array[0]);
      lives_strfreev(array);
    }

    if (key < 1 || key > prefs->rte_keys_virtual) {
      d_print((tmp = lives_strdup_printf(_("Invalid key %d in %s\n"), key, keymap_file)));
      LIVES_ERROR(tmp);
      lives_free(tmp);
      notfound = TRUE;
      lives_free(hashname);
      if (keymap_file2 != NULL) {
        // read param defaults
        if (!read_perkey_defaults(kfd, -1, -1, version)) break; // file read error
      }
      continue;
    }

    def_modes[key - 1]++;

    if (strncmp(hashname, "Weed", 4) || lives_strlen(hashname) < 5) {
      d_print((tmp = lives_strdup_printf(_("Invalid effect %s in %s\n"), hashname, keymap_file)));
      LIVES_ERROR(tmp);
      lives_free(tmp);
      notfound = TRUE;
      lives_free(hashname);
      badkeymap[key - 1][def_modes[key - 1]]++;
      if (keymap_file2 != NULL) {
        // read param defaults
        if (!read_perkey_defaults(kfd, -1, -1, version)) break; // file read error
      }
      def_modes[key - 1]--;
      continue;
    }

    // ignore "Weed"
    whashname = hashname + 4;

    if ((mode = weed_add_effectkey(key, whashname, TRUE)) == -1) {
      // could not locate effect
      d_print((tmp = lives_strdup_printf(_("Unknown effect %s in\n%s\n"), whashname, keymap_file)));
      LIVES_WARN(tmp);
      lives_free(tmp);
      notfound = TRUE;
      lives_free(hashname);
      badkeymap[key - 1][def_modes[key - 1]]++;
      if (keymap_file2 != NULL) {
        // read param defaults
        if (!read_perkey_defaults(kfd, -1, -1, version)) break; // file read error
      }
      def_modes[key - 1]--;
      continue;
    }

    lives_free(hashname);

    if (mode == -2) {
      d_print((tmp = lives_strdup_printf
                     (_("This version of LiVES cannot mix generators/non-generators on the same key (%d) !\n"), key)));
      LIVES_ERROR(tmp);
      lives_free(tmp);
      badkeymap[key - 1][def_modes[key - 1]]++;
      if (keymap_file2 != NULL) {
        // read param defaults
        if (!read_perkey_defaults(kfd, -1, -1, version)) break; // file read error
      }
      def_modes[key - 1]--;
      continue;
    }
    if (mode == -3) {
      d_print((tmp = lives_strdup_printf(_("Too many effects bound to key %d.\n"), key)));
      LIVES_ERROR(tmp);
      lives_free(tmp);
      if (keymap_file2 != NULL) {
        // read param defaults
        if (!read_perkey_defaults(kfd, -1, -1, version)) break; // file read error
      }
      def_modes[key - 1]--;
      continue;
    }
    if (rte_window != NULL) {
      int idx = (key - 1) * modes + mode;
      int fx_idx = rte_keymode_get_filter_idx(key, mode);

      rtew_combo_set_text(key - 1, mode, (tmp = rte_keymode_get_filter_name(key, mode, FALSE, FALSE)));
      lives_free(tmp);

      if (fx_idx != -1) {
        hashname = (char *)lives_list_nth_data(hash_list, fx_idx);
        lives_widget_object_set_data(LIVES_WIDGET_OBJECT(combos[idx]), "hashname", hashname);
      } else lives_widget_object_set_data(LIVES_WIDGET_OBJECT(combos[idx]), "hashname", empty_string);

      // set parameters button sensitive/insensitive
      set_param_and_con_buttons(key - 1, mode);
    }

    if (keymap_file2 != NULL) {
      // read param defaults
      if (!read_perkey_defaults(kfd, key - 1, def_modes[key - 1], version)) break; // file read error
    }
  }

  if (keymap_file2 == NULL) {
    lives_list_free_all(&list);

    if (update > 0) {
      d_print(_("update required.\n"));
      on_save_keymap_clicked(NULL, new_list);
      lives_list_free_all(&new_list);
      on_load_keymap_clicked(NULL, NULL);
    } else d_print_done();
  } else {
    if (kfd != -1) lives_close_buffered(kfd);
    if (mainw->is_ready) d_print_done();
  }

  if (update == 0) {
    if (lives_file_test(keymap_file3, LIVES_FILE_TEST_EXISTS)) {
      d_print(_("Loading data connection map from %s..."), keymap_file3);

      if (load_datacons(keymap_file3, badkeymap)) d_print_done();
    }

    if (mainw->is_ready) {
      check_clear_all_button();
      if (notfound) do_warning_dialog(_("\n\nSome effects could not be located.\n\n"));
    } else load_rte_defs(); // file errors shown inside

  }

cleanup1:

  if (badkeymap != NULL) {
    for (i = 0; i < prefs->rte_keys_virtual; i++) {
      lives_free(badkeymap[i]);
    }

    lives_free(badkeymap);
  }
  lives_freep((void **)&keymap_file); // frees keymap_file2 if applicable
  lives_freep((void **)&keymap_file3);
  lives_freep((void **)&def_modes);
  if (mainw->ce_thumbs) ce_thumbs_reset_combos();
  if (rte_window != NULL) check_clear_all_button();

  d_print_done();
  return FALSE;
}


void on_rte_info_clicked(LiVESButton * button, livespointer user_data) {
  weed_plant_t *filter;

  LiVESWidget *dialog;

  LiVESWidget *vbox;
  LiVESWidget *hbox;
  LiVESWidget *label;
  LiVESWidget *textview;
  LiVESWidget *scrolledwindow;
  LiVESWidget *ok_button;

  char *filter_name;
  char *filter_author;
  char *filter_copyright;
  char *filter_extra_authors = NULL;
  char *filter_description;
  char *url;
  char *license;
  char *tmp;
  char *type;
  char *plugin_name;
  char *package_name;

  boolean has_desc = FALSE;
  boolean has_url = FALSE;
  boolean has_license = FALSE;
  boolean has_copyright = FALSE;

  int filter_version;
  int weed_error;

  int key_mode = LIVES_POINTER_TO_INT(user_data);
  int modes = rte_getmodespk();
  int key = (int)(key_mode / modes);
  int mode = key_mode - key * modes;
  int window_width = RFX_WINSIZE_H;

  ////////////////////////

  if (!rte_keymode_valid(key + 1, mode, TRUE)) return;

  type = rte_keymode_get_type(key + 1, mode);

  plugin_name = rte_keymode_get_plugin_name(key + 1, mode);
  filter = rte_keymode_get_filter(key + 1, mode);
  filter_name = weed_get_string_value(filter, WEED_LEAF_NAME, &weed_error);
  package_name = weed_filter_get_package_name(filter);
  filter_author = weed_get_string_value(filter, WEED_LEAF_AUTHOR, &weed_error);
  if (weed_plant_has_leaf(filter, WEED_LEAF_EXTRA_AUTHORS)) filter_extra_authors = weed_get_string_value(filter,
        WEED_LEAF_EXTRA_AUTHORS,
        &weed_error);
  if (weed_plant_has_leaf(filter, WEED_LEAF_COPYRIGHT)) {
    filter_copyright = weed_get_string_value(filter, WEED_LEAF_COPYRIGHT, &weed_error);
    has_copyright = TRUE;
  }
  if (weed_plant_has_leaf(filter, WEED_LEAF_DESCRIPTION)) {
    filter_description = weed_get_string_value(filter, WEED_LEAF_DESCRIPTION, &weed_error);
    has_desc = TRUE;
  }
  if (weed_plant_has_leaf(filter, WEED_LEAF_URL)) {
    url = weed_get_string_value(filter, WEED_LEAF_URL, &weed_error);
    has_url = TRUE;
  }

  if (weed_plant_has_leaf(filter, WEED_LEAF_LICENSE)) {
    license = weed_get_string_value(filter, WEED_LEAF_LICENSE, &weed_error);
    has_license = TRUE;
  }

  filter_version = weed_get_int_value(filter, WEED_LEAF_VERSION, &weed_error);

  tmp = lives_strdup_printf(_("Information for %s"), filter_name);

  dialog = lives_standard_dialog_new(tmp, FALSE, RTE_INFO_WIDTH, RTE_INFO_HEIGHT);

  lives_free(tmp);

  vbox = lives_dialog_get_content_area(LIVES_DIALOG(dialog));

  widget_opts.justify = LIVES_JUSTIFY_CENTER;
  label = lives_standard_label_new((tmp = lives_strdup_printf(_("Effect name: %s"), filter_name)));
  lives_free(tmp);
  lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);

  label = lives_standard_label_new((tmp = lives_strdup_printf(_("Type: %s"), type)));
  lives_free(tmp);
  lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);

  label = lives_standard_label_new((tmp = lives_strdup_printf(_("Plugin name: %s"), plugin_name)));
  lives_free(tmp);
  lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);

  if (package_name != NULL) {
    label = lives_standard_label_new((tmp = lives_strdup_printf(_("Package name: %s"), package_name)));
    lives_free(tmp);
    lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);
  }

  label = lives_standard_label_new((tmp = lives_strdup_printf(_("Author: %s"), filter_author)));
  lives_free(tmp);
  lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);

  if (filter_extra_authors != NULL) {
    label = lives_standard_label_new((tmp = lives_strdup_printf(_("and: %s"), filter_extra_authors)));
    lives_free(tmp);
    lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);
  }

  if (has_url) {
    label = lives_standard_label_new((tmp = lives_strdup_printf(_("URL: %s"), url)));
    lives_free(tmp);
    lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);
  }

  label = lives_standard_label_new((tmp = lives_strdup_printf(_("Version: %d"), filter_version)));
  lives_free(tmp);
  lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);

  if (has_desc) {


    hbox = lives_hbox_new(FALSE, widget_opts.packing_width);
    lives_box_pack_start(LIVES_BOX(vbox), hbox, TRUE, FALSE, widget_opts.packing_height);

    label = lives_standard_label_new(_("Description: "));
    lives_box_pack_start(LIVES_BOX(hbox), label, FALSE, FALSE, widget_opts.packing_height);

    widget_opts.justify = LIVES_JUSTIFY_DEFAULT;
    textview = lives_standard_text_view_new(filter_description, NULL);
    widget_opts.justify = LIVES_JUSTIFY_CENTER;

    widget_opts.expand = LIVES_EXPAND_EXTRA_WIDTH | LIVES_EXPAND_DEFAULT_HEIGHT;
    scrolledwindow = lives_standard_scrolled_window_new(window_width * 2, RFX_WINSIZE_V / 2, textview);
    lives_box_pack_start(LIVES_BOX(hbox), scrolledwindow, TRUE, TRUE, widget_opts.packing_height);
    widget_opts.expand = LIVES_EXPAND_DEFAULT;
    if (palette->style & STYLE_1) {
      lives_widget_set_text_color(textview, LIVES_WIDGET_STATE_NORMAL, &palette->normal_fore);
      lives_widget_set_base_color(textview, LIVES_WIDGET_STATE_NORMAL, &palette->normal_back);
      lives_widget_set_bg_color(lives_bin_get_child(LIVES_BIN(scrolledwindow)), LIVES_WIDGET_STATE_NORMAL, &palette->info_base);
    }
  }

  if (has_license) {
    label = lives_standard_label_new((tmp = lives_strdup_printf(_("License: %s"), license)));
    lives_free(tmp);
    lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);
  }

  if (has_copyright) {
    label = lives_standard_label_new((tmp = lives_strdup_printf(_("Copyright: %s"), filter_copyright)));
    lives_free(tmp);
    lives_box_pack_start(LIVES_BOX(vbox), label, TRUE, FALSE, widget_opts.packing_height);
  }

  widget_opts.justify = LIVES_JUSTIFY_DEFAULT;

  add_fill_to_box(LIVES_BOX(vbox));

  ok_button = lives_dialog_add_button_from_stock(LIVES_DIALOG(dialog), LIVES_STOCK_CLOSE, _("_Close Window"),
              LIVES_RESPONSE_OK);

  lives_button_grab_default_special(ok_button);

  lives_signal_connect(LIVES_GUI_OBJECT(ok_button), LIVES_WIDGET_CLICKED_SIGNAL,
                       LIVES_GUI_CALLBACK(lives_general_button_clicked),
                       NULL);

  lives_free(filter_name);
  lives_free(filter_author);
  lives_freep((void **)&filter_extra_authors);
  lives_freep((void **)&package_name);
  if (has_desc) lives_free(filter_description);
  if (has_url) lives_free(url);
  if (has_license) lives_free(license);
  if (has_copyright) lives_free(filter_copyright);
  lives_free(plugin_name);
  lives_free(type);

  lives_widget_show_all(dialog);
  lives_window_center(LIVES_WINDOW(dialog));
}


void on_clear_clicked(LiVESButton * button, livespointer user_data) {
  // this is for the "delete" buttons, c.f. clear_all

  int idx = LIVES_POINTER_TO_INT(user_data);
  int modes = rte_getmodespk();
  int key = (int)(idx / modes);
  int mode = idx - key * modes;

  int newmode;

  register int i;

  weed_delete_effectkey(key + 1, mode);

  pconx_delete(FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD, key, mode, FX_DATA_WILDCARD);
  pconx_delete(key, mode, FX_DATA_WILDCARD, -1, FX_DATA_WILDCARD, FX_DATA_WILDCARD);

  cconx_delete(FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD, key, mode, FX_DATA_WILDCARD);
  cconx_delete(key, mode, FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD);

  newmode = rte_key_getmode(key + 1);

  if (mainw->ce_thumbs) ce_thumbs_set_mode_combo(key, newmode);

  if (rte_window != NULL) {
    rtew_set_mode_radio(key, newmode);
  }

  for (i = mode; i < rte_getmodespk() - 1; i++) {
    pconx_remap_mode(key, i + 1, i);
    cconx_remap_mode(key, i + 1, i);

    if (rte_window != NULL) {
      int fx_idx = rte_keymode_get_filter_idx(key, mode);
      idx = key * modes + i;
      rtew_combo_set_text(key, i, lives_entry_get_text(LIVES_ENTRY(combo_entries[idx + 1])));

      if (fx_idx != -1) {
        char *hashname = (char *)lives_list_nth_data(hash_list, fx_idx);
        lives_widget_object_set_data(LIVES_WIDGET_OBJECT(combos[idx]), "hashname", hashname);
      } else lives_widget_object_set_data(LIVES_WIDGET_OBJECT(combos[idx]), "hashname", empty_string);

      // set parameters button sensitive/insensitive
      set_param_and_con_buttons(key, i);
    }
  }
  idx++;

  if (rte_window != NULL) {
    rtew_combo_set_text(key, i, "");
    lives_widget_object_set_data(LIVES_WIDGET_OBJECT(combos[idx]), "hashname", empty_string);

    // set parameters button sensitive/insensitive
    set_param_and_con_buttons(key, i);
  }

  if (!rte_keymode_valid(key + 1, 0, TRUE)) {
    if (rte_window != NULL) rtew_set_keych(key, FALSE);
    if (mainw->ce_thumbs) ce_thumbs_set_keych(key, FALSE);
  }
  if (rte_window != NULL) check_clear_all_button();

  if (mainw->ce_thumbs) ce_thumbs_reset_combo(key);
}


static void on_datacon_clicked(LiVESButton * button, livespointer user_data) {
  int idx = LIVES_POINTER_TO_INT(user_data);
  int modes = rte_getmodespk();
  int key = (int)(idx / modes);
  int mode = idx - key * modes;

  //if (datacon_dialog!=NULL) on_datacon_cancel_clicked(NULL,NULL);
  datacon_dialog = make_datacon_window(key, mode);
}


void check_string_choice_params(weed_plant_t *inst) {
  int nparams;
  weed_plant_t **in_params = weed_instance_get_in_params(inst, &nparams);
  weed_plant_t *gui, *tgui;
  for (int i = 0; i < nparams; i++) {
    if ((gui = weed_param_get_gui(in_params[i], FALSE)) != NULL && weed_plant_has_leaf(gui, WEED_LEAF_CHOICES)) {
      tgui = weed_paramtmpl_get_gui(weed_param_get_template(in_params[i]), TRUE);
      weed_leaf_copy(tgui, WEED_LEAF_CHOICES, gui, WEED_LEAF_CHOICES);
    }
  }
}


static void on_params_clicked(LiVESButton * button, livespointer user_data) {
  int idx = LIVES_POINTER_TO_INT(user_data);
  int modes = rte_getmodespk();
  int key = (int)(idx / modes);
  int mode = idx - key * modes;

  weed_plant_t *inst;
  lives_rfx_t *rfx;

  filter_mutex_lock(key);
  if ((inst = rte_keymode_get_instance(key + 1, mode)) == NULL) {
    // create a new detached instance for the dialog
    weed_plant_t *filter = rte_keymode_get_filter(key + 1, mode);
    if (filter == NULL) {
      filter_mutex_unlock(key);
      return;
    }
    inst = weed_instance_from_filter(filter);
    weed_set_boolean_value(inst, WEED_LEAF_HOST_NORECORD, WEED_TRUE);

    // do some fiddly stuff to show the key defs.
    filter_mutex_unlock(key);
    weed_reinit_effect(inst, TRUE);
    filter_mutex_lock(key);
    check_string_choice_params(inst);
    apply_key_defaults(inst, key, mode);
    filter_mutex_unlock(key);
    weed_reinit_effect(inst, TRUE);
    filter_mutex_lock(key);
  } else weed_instance_ref(inst);

  filter_mutex_unlock(key);

  if (fx_dialog[1] != NULL) {
    lives_widget_destroy(fx_dialog[1]->dialog);
    rfx = fx_dialog[1]->rfx;
    on_paramwindow_button_clicked(NULL, rfx);
    lives_freep((void **)&fx_dialog[1]);
  }

  rfx = weed_to_rfx(inst, FALSE);
  rfx->min_frames = -1;
  keyw = key;
  modew = mode;

  widget_opts.non_modal = TRUE;
  on_fx_pre_activate(rfx, TRUE, NULL);
  widget_opts.non_modal = FALSE;

  // record the key so we know whose parameters to record later
  weed_set_int_value((weed_plant_t *)rfx->source, WEED_LEAF_HOST_KEY, key);

  fx_dialog[1]->key = key;
  fx_dialog[1]->mode = mode;
  fx_dialog[1]->rfx = rfx;
  weed_instance_unref(inst);
}


boolean on_rtew_delete_event(LiVESWidget * widget, LiVESXEventDelete * event, livespointer user_data) {
  old_rte_keys_virtual = prefs->rte_keys_virtual;
  lives_widget_set_sensitive(mainw->mt_menu, TRUE);
  lives_widget_set_sensitive(mainw->rte_defs_menu, TRUE);
  if (user_data == NULL) {
    // first time around we come here, and just hide the window
    if (mainw->play_window != NULL && !mainw->fs && (prefs->play_monitor == widget_opts.monitor + 1 || capable->nmonitors == 1)) {
      lives_window_set_transient_for(LIVES_WINDOW(mainw->play_window), get_transient_full());
    }
    lives_widget_hide(rte_window);
    rte_window_is_hidden = TRUE;
  } else {
    // when we reshow it we check if the number of fx keys has changed. If so we come back to this branch
    // and recreate the window from scratch
    lives_list_free_all(&hash_list);
    lives_list_free_all(&name_list);
    lives_list_free_all(&extended_name_list);

    lives_free(key_checks);
    lives_free(key_grabs);
    lives_free(mode_radios);
    lives_free(combo_entries);
    lives_free(combos);
    lives_free(ch_fns);
    lives_free(mode_ra_fns);
    lives_free(gr_fns);
    lives_free(nlabels);
    lives_free(type_labels);
    lives_free(info_buttons);
    lives_free(param_buttons);
    lives_free(conx_buttons);
    lives_free(clear_buttons);
  }
  return FALSE;
}


static boolean on_rtew_ok_clicked(LiVESButton * button, livespointer user_data) {
  on_rtew_delete_event(NULL, NULL, NULL);
  return TRUE;
}


LIVES_LOCAL_INLINE void do_mix_error(void) {
  do_error_dialog(_("\n\nThis version of LiVES does not allowing mixing of generators and non-generators on the same key.\n\n"));
  return;
}


enum {
  EXTENDED_NAME_COLUMN,
  NAME_COLUMN,
  HASH_COLUMN,
  NUM_COLUMNS
};

void rtew_combo_set_text(int key, int mode, const char *txt) {
  // key is zero based
  int key_mode = key * rte_getmodespk() + mode;
  lives_entry_set_text(LIVES_ENTRY(combo_entries[key_mode]), txt);
  type_label_set_text(key, mode);
}


void fx_changed(LiVESCombo * combo, livespointer user_data) {
  LiVESTreeIter iter1;
  LiVESTreeModel *model;

  char *txt;
  char *tmp;
  char *hashname1;
  char *hashname2 = (char *)lives_widget_object_get_data(LIVES_WIDGET_OBJECT(combo), "hashname");

  int key_mode = LIVES_POINTER_TO_INT(user_data);
  int modes = rte_getmodespk();
  int key = (int)(key_mode / modes);
  int mode = key_mode - key * modes;

  int error;

  register int i;

  if (lives_combo_get_active(combo) == -1) return; // -1 is returned after we set our own text (without the type)

  lives_combo_get_active_iter(combo, &iter1);
  model = lives_combo_get_model(combo);

  lives_tree_model_get(model, &iter1, HASH_COLUMN, &hashname1, -1);
  if (hashname1 == NULL) {
    lives_entry_set_text(LIVES_ENTRY(combo_entries[key_mode]), (tmp = rte_keymode_get_filter_name(key + 1, mode, FALSE, FALSE)));
    lives_free(tmp);
    return;
  }

  if (!strcmp(hashname1, hashname2)) {
    lives_free(hashname1);
    return;
  }

  if (!rte_keymode_valid(key + 1, mode, TRUE)) {
    for (i = mode - 1; i >= 0; i--) {
      if (rte_keymode_valid(key + 1, i, TRUE)) {
        mode = i + 1;
        i = -1;
      }
      if (i == 0) mode = 0;
    }
  }

  lives_widget_grab_focus(combo_entries[key_mode]);

  if ((error = rte_switch_keymode(key + 1, mode, hashname1)) < 0) {
    lives_entry_set_text(LIVES_ENTRY(combo_entries[key_mode]), (tmp = rte_keymode_get_filter_name(key + 1, mode, FALSE, FALSE)));
    lives_free(tmp);

    if (error == -2) do_mix_error();
    if (error == -1) {
      d_print(_("LiVES could not locate the effect %s.\n"), rte_keymode_get_filter_name(key + 1, mode, TRUE, FALSE));
    }
    return;
  }

  // prevents a segfault
  lives_combo_get_active_iter(combo, &iter1);
  model = lives_combo_get_model(combo);

  lives_tree_model_get(model, &iter1, NAME_COLUMN, &txt, -1);
  rtew_combo_set_text(key, mode, txt);
  lives_free(txt);

  lives_widget_object_set_data(LIVES_WIDGET_OBJECT(combo), "hashname", hashname1);

  // set parameters button sensitive/insensitive
  set_param_and_con_buttons(key, mode);

  check_clear_all_button();

  pconx_delete(FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD, key, mode, FX_DATA_WILDCARD_KEEP_ACTIVATED);
  pconx_delete(key, mode, FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD);

  cconx_delete(FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD, key, mode, FX_DATA_WILDCARD);
  cconx_delete(key, mode, FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD, FX_DATA_WILDCARD);

  if (mainw->ce_thumbs) ce_thumbs_reset_combos();
}

static LiVESTreeStore *tstore = NULL;

static LiVESTreeModel *rte_window_fx_model(void) {
  weed_plant_t *pinfo;
  LiVESTreeIter iter1, iter2, iter3;

  LiVESList *list = extended_name_list;
  LiVESList *pname_list = name_list;
  LiVESList *phash_list = hash_list;

  lives_fx_cat_t cat;

  char *pkg = NULL, *pkgstring, *fxname, *typestr;

  int fx_idx = 0;
  int error;

  if (tstore != NULL) return (LiVESTreeModel *)tstore;

  tstore = lives_tree_store_new(NUM_COLUMNS, LIVES_COL_TYPE_STRING, LIVES_COL_TYPE_STRING, LIVES_COL_TYPE_STRING);
  while (list != NULL) {
    weed_plant_t *filter = get_weed_filter(weed_get_idx_for_hashname((char *)phash_list->data, TRUE));
    int filter_flags = weed_get_int_value(filter, WEED_LEAF_FLAGS, &error);
    if ((weed_filter_hints_unstable(filter) && !prefs->unstable_fx) || ((enabled_in_channels(filter, FALSE) > 1 &&
        !has_video_chans_in(filter, FALSE)) ||
        (weed_plant_has_leaf(filter, WEED_LEAF_HOST_MENU_HIDE) &&
         weed_get_boolean_value(filter, WEED_LEAF_HOST_MENU_HIDE, &error) == WEED_TRUE)
        || (filter_flags & WEED_FILTER_IS_CONVERTER))
        || enabled_in_channels(filter, TRUE) == 1000000) {
      list = list->next;
      fx_idx++;
      pname_list = pname_list->next;
      phash_list = phash_list->next;
      continue; // skip audio transitions, compositors and hidden entries
    }

    fxname = lives_strdup((const char *)pname_list->data);
    cat = weed_filter_categorise(filter,
                                 enabled_in_channels(filter, TRUE),
                                 enabled_out_channels(filter, TRUE));
    typestr = lives_fx_cat_to_text(cat, TRUE);

    pinfo = weed_get_plantptr_value(filter, WEED_LEAF_PLUGIN_INFO, &error);
    if (weed_plant_has_leaf(pinfo, WEED_LEAF_PACKAGE_NAME))
      pkgstring = weed_get_string_value(pinfo, WEED_LEAF_PACKAGE_NAME, &error);
    else pkgstring = NULL;

    if (pkgstring != NULL) {
      // package effect
      if (pkg != NULL && lives_strcmp(pkg, pkgstring)) {
        // new package
        lives_freep((void **)&pkg);
      }

      if (pkg == NULL) {
        // add package to menu
        pkg = pkgstring;

        /* TRANSLATORS: example " - LADSPA plugins -" */
        pkgstring = lives_strdup_printf(_(" - %s plugins -"), pkg);
        lives_tree_store_prepend(tstore, &iter1, NULL);
        lives_tree_store_set(tstore, &iter1, EXTENDED_NAME_COLUMN, pkgstring, -1);
        lives_free(pkgstring);
      }
      // add to package submenu

      // get a new or existing iterator for the category (set in iter2)
      lives_tree_store_find_iter(tstore, EXTENDED_NAME_COLUMN, typestr, &iter1, &iter2);
      lives_tree_store_append(tstore, &iter3, &iter2);
      lives_tree_store_set(tstore, &iter3, EXTENDED_NAME_COLUMN, list->data, NAME_COLUMN, fxname,
                           HASH_COLUMN, lives_list_nth_data(hash_list, fx_idx), -1);
    } else {
      //if (pkg != NULL) lives_freep((void **)&pkg);
      // get a new or existing iterator for the category
      lives_tree_store_find_iter(tstore, EXTENDED_NAME_COLUMN, typestr, NULL, &iter1);

      lives_tree_store_append(tstore, &iter2, &iter1);   /* Acquire an iterator */
      lives_tree_store_set(tstore, &iter2, EXTENDED_NAME_COLUMN, list->data, NAME_COLUMN, fxname,
                           HASH_COLUMN, lives_list_nth_data(hash_list, fx_idx), -1);
    }
    lives_free(fxname);
    lives_free(typestr);

    list = list->next;
    fx_idx++;
    pname_list = pname_list->next;
    phash_list = phash_list->next;
  }

  lives_freep((void **)&pkg);

  return (LiVESTreeModel *)tstore;
}


LiVESWidget *create_rte_window(void) {
  LiVESWidget *irte_window = rte_window;
  LiVESWidget *table;
  LiVESWidget *hbox;
  LiVESWidget *hbox2;

  LiVESWidget *vbox;
  LiVESWidget *label;
  LiVESWidget *combo;
  LiVESWidget *ok_button;
  LiVESWidget *top_vbox;
  LiVESWidget *hbuttonbox;

  LiVESWidget *scrolledwindow;

  LiVESSList *mode_group = NULL;
  LiVESSList *grab_group = NULL;

  LiVESAccelGroup *rtew_accel_group;

  LiVESTreeModel *model;

  char *tmp, *tmp2, *labelt;

  int modes = rte_getmodespk();

  int idx;

  int winsize_h;
  int winsize_v;

  register int i, j;

  ///////////////////////////////////////////////////////////////////////////

  lives_set_cursor_style(LIVES_CURSOR_BUSY, NULL);
  lives_widget_context_update();
  mainw->no_context_update = TRUE;

  winsize_h = GUI_SCREEN_WIDTH - SCR_WIDTH_SAFETY;
  winsize_v = GUI_SCREEN_HEIGHT - SCR_HEIGHT_SAFETY;

  if (irte_window != NULL) {
    if (prefs->rte_keys_virtual != old_rte_keys_virtual) {
      // number of fx keys changed, rebuild the window
      mainw->no_context_update = FALSE;
      return refresh_rte_window();
    }
    goto rte_window_ready;
  }

  key_checks = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * sizeof(LiVESWidget *));
  key_grabs = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * sizeof(LiVESWidget *));
  mode_radios = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(LiVESWidget *));
  combo_entries = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(LiVESWidget *));
  combos = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(LiVESWidget *));
  info_buttons = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(LiVESWidget *));
  param_buttons = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(LiVESWidget *));
  conx_buttons = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(LiVESWidget *));
  clear_buttons = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(LiVESWidget *));
  nlabels = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(LiVESWidget *));
  type_labels = (LiVESWidget **)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(LiVESWidget *));

  ch_fns = (ulong *)lives_malloc((prefs->rte_keys_virtual) * sizeof(ulong));
  gr_fns = (ulong *)lives_malloc((prefs->rte_keys_virtual) * sizeof(ulong));
  mode_ra_fns = (ulong *)lives_malloc((prefs->rte_keys_virtual) * modes * sizeof(ulong));

  irte_window = lives_window_new(LIVES_WINDOW_TOPLEVEL);
  lives_window_set_transient_for(LIVES_WINDOW(irte_window), LIVES_WINDOW(LIVES_MAIN_WINDOW_WIDGET));

  if (palette->style & STYLE_1) {
    lives_widget_set_bg_color(irte_window, LIVES_WIDGET_STATE_NORMAL, &palette->menu_and_bars);
    lives_widget_set_text_color(irte_window, LIVES_WIDGET_STATE_NORMAL, &palette->menu_and_bars_fore);
  }
  lives_window_set_title(LIVES_WINDOW(irte_window), _("Real Time Effect Mapping"));
  lives_window_add_accel_group(LIVES_WINDOW(irte_window), mainw->accel_group);

  table = lives_table_new(prefs->rte_keys_virtual, modes + 1, FALSE);

  lives_table_set_row_spacings(LIVES_TABLE(table), 16 * widget_opts.scale);
  lives_table_set_col_spacings(LIVES_TABLE(table), 4 * widget_opts.scale);

  // dummy button for "no grab", we dont show this...there is a button instead
  dummy_radio = lives_radio_button_new(grab_group);
  grab_group = lives_radio_button_get_group(LIVES_RADIO_BUTTON(dummy_radio));
  lives_widget_set_no_show_all(dummy_radio, TRUE);

  if (name_list == NULL) name_list = weed_get_all_names(FX_LIST_NAME);
  if (extended_name_list == NULL) extended_name_list = weed_get_all_names(FX_LIST_EXTENDED_NAME);
  if (hash_list == NULL) hash_list = weed_get_all_names(FX_LIST_HASHNAME);

  model = rte_window_fx_model();

  for (i = 0; i < prefs->rte_keys_virtual * modes; i++) {
    // create combo entry model
    combos[i] = NULL;
  }

  for (i = 0; i < prefs->rte_keys_virtual; i++) {
    hbox = lives_hbox_new(FALSE, 0);
    lives_table_attach(LIVES_TABLE(table), hbox, i, i + 1, 0, 1,
                       (LiVESAttachOptions)(LIVES_EXPAND | LIVES_FILL),
                       (LiVESAttachOptions)(LIVES_EXPAND | LIVES_FILL), 0, 0);
    lives_container_set_border_width(LIVES_CONTAINER(hbox), widget_opts.border_width);

    if (i < 9) labelt = lives_strdup_printf("%d", i + 1);
    else  {
      switch (i) {
      case 9:
        labelt = (_("minus")); break;
      case 10:
        labelt = (_("equals")); break;
      default:
        labelt = lives_strdup("????");
        break;
      }
    }

    label = lives_standard_label_new((tmp = lives_strdup_printf(_("Ctrl-%s"), labelt)));
    lives_free(labelt);
    lives_free(tmp);

    lives_box_pack_start(LIVES_BOX(hbox), label, TRUE, FALSE, widget_opts.packing_width);

    hbox2 = lives_hbox_new(FALSE, 0);

    key_checks[i] = lives_standard_check_button_new(_("Key active"), (mainw->rte & (GU641 << i)), LIVES_BOX(hbox2), NULL);

    lives_box_pack_start(LIVES_BOX(hbox), hbox2, FALSE, FALSE, widget_opts.packing_width);

    lives_toggle_button_set_active(LIVES_TOGGLE_BUTTON(key_checks[i]), mainw->rte & (GU641 << i));
    lives_widget_object_set_data(LIVES_WIDGET_OBJECT(key_checks[i]), "active",
                                 LIVES_INT_TO_POINTER(lives_toggle_button_get_active(LIVES_TOGGLE_BUTTON(key_checks[i]))));

    ch_fns[i] = lives_signal_connect_after(LIVES_GUI_OBJECT(key_checks[i]), LIVES_WIDGET_TOGGLED_SIGNAL,
                                           LIVES_GUI_CALLBACK(rte_on_off_callback_hook), LIVES_INT_TO_POINTER(i + 1));

    hbox2 = lives_hbox_new(FALSE, 0);
    lives_box_pack_start(LIVES_BOX(hbox), hbox2, FALSE, FALSE, widget_opts.packing_width);

    key_grabs[i] = lives_standard_radio_button_new((tmp = (_("Key grab"))), &grab_group, LIVES_BOX(hbox2),
                   (tmp2 = (_("Grab keyboard for this effect key"))));
    lives_free(tmp);
    lives_free(tmp2);
    lives_toggle_button_set_active(LIVES_TOGGLE_BUTTON(key_grabs[i]), mainw->rte_keys == i);

    gr_fns[i] = lives_signal_connect_after(LIVES_GUI_OBJECT(key_grabs[i]), LIVES_WIDGET_TOGGLED_SIGNAL,
                                           LIVES_GUI_CALLBACK(grabkeys_callback_hook), LIVES_INT_TO_POINTER(i));

    mode_group = NULL;

    for (j = 0; j < modes; j++) {
      idx = i * modes + j;
      hbox = lives_hbox_new(FALSE, 0);
      lives_table_attach(LIVES_TABLE(table), hbox, i, i + 1, j + 1, j + 2,
                         (LiVESAttachOptions)(LIVES_EXPAND | LIVES_FILL),
                         (LiVESAttachOptions)(LIVES_EXPAND | LIVES_FILL), 0, 0);
      lives_container_set_border_width(LIVES_CONTAINER(hbox), widget_opts.border_width);

      hbox2 = lives_hbox_new(FALSE, 0);
      lives_box_pack_start(LIVES_BOX(hbox), hbox2, FALSE, FALSE, widget_opts.packing_width);

      mode_radios[idx] = lives_standard_radio_button_new(_("Mode active"), &mode_group, LIVES_BOX(hbox2), NULL);

      if (rte_key_getmode(i + 1) == j) lives_toggle_button_set_active(LIVES_TOGGLE_BUTTON(mode_radios[idx]), TRUE);

      mode_ra_fns[idx] = lives_signal_connect_after(LIVES_GUI_OBJECT(mode_radios[idx]), LIVES_WIDGET_TOGGLED_SIGNAL,
                         LIVES_GUI_CALLBACK(rtemode_callback_hook), LIVES_INT_TO_POINTER(idx));

      type_labels[idx] = lives_standard_label_new("");

      info_buttons[idx] = lives_standard_button_new_with_label(_("Info"));
      param_buttons[idx] = lives_standard_button_new_with_label(_("Set Parameters"));
      conx_buttons[idx] = lives_standard_button_new_with_label(_("Set Connections"));
      clear_buttons[idx] = lives_standard_button_new_with_label(_("Clear"));

      vbox = lives_vbox_new(FALSE, 0);
      lives_box_pack_start(LIVES_BOX(hbox), vbox, FALSE, FALSE, widget_opts.packing_width);
      lives_container_set_border_width(LIVES_CONTAINER(vbox), widget_opts.border_width);

      hbox = lives_hbox_new(FALSE, 0);

      lives_box_pack_start(LIVES_BOX(vbox), hbox, FALSE, FALSE, widget_opts.packing_height);

      nlabels[idx] = lives_standard_label_new(_("Effect name:"));

      lives_box_pack_start(LIVES_BOX(hbox), nlabels[idx], FALSE, FALSE, widget_opts.packing_width);

      combos[idx] = combo = lives_standard_combo_new_with_model(model, LIVES_BOX(hbox));
      lives_combo_set_entry_text_column(LIVES_COMBO(combo), EXTENDED_NAME_COLUMN);

      lives_widget_object_set_data(LIVES_WIDGET_OBJECT(combo), "hashname", empty_string);
      lives_box_pack_end(LIVES_BOX(hbox), clear_buttons[idx], FALSE, FALSE, widget_opts.packing_width);
      lives_box_pack_end(LIVES_BOX(hbox), info_buttons[idx], FALSE, FALSE, widget_opts.packing_width);

      combo_entries[idx] = lives_combo_get_entry(LIVES_COMBO(combo));

      lives_entry_set_text(LIVES_ENTRY(combo_entries[idx]), (tmp = rte_keymode_get_filter_name(i + 1, j, FALSE, FALSE)));
      lives_free(tmp);

      lives_entry_set_editable(LIVES_ENTRY(combo_entries[idx]), FALSE);

      hbox = lives_hbox_new(FALSE, 0);
      lives_box_pack_start(LIVES_BOX(vbox), hbox, FALSE, FALSE, widget_opts.packing_height);

      lives_signal_connect(LIVES_GUI_OBJECT(combo), LIVES_WIDGET_CHANGED_SIGNAL,
                           LIVES_GUI_CALLBACK(fx_changed), LIVES_INT_TO_POINTER(i * rte_getmodespk() + j));

      lives_signal_connect(LIVES_GUI_OBJECT(info_buttons[idx]), LIVES_WIDGET_CLICKED_SIGNAL,
                           LIVES_GUI_CALLBACK(on_rte_info_clicked), LIVES_INT_TO_POINTER(idx));

      lives_signal_connect(LIVES_GUI_OBJECT(clear_buttons[idx]), LIVES_WIDGET_CLICKED_SIGNAL,
                           LIVES_GUI_CALLBACK(on_clear_clicked), LIVES_INT_TO_POINTER(idx));

      lives_signal_connect(LIVES_GUI_OBJECT(param_buttons[idx]), LIVES_WIDGET_CLICKED_SIGNAL,
                           LIVES_GUI_CALLBACK(on_params_clicked), LIVES_INT_TO_POINTER(idx));

      lives_signal_connect(LIVES_GUI_OBJECT(conx_buttons[idx]), LIVES_WIDGET_CLICKED_SIGNAL,
                           LIVES_GUI_CALLBACK(on_datacon_clicked), LIVES_INT_TO_POINTER(idx));

      lives_box_pack_start(LIVES_BOX(hbox), type_labels[idx], FALSE, FALSE, widget_opts.packing_width);
      lives_box_pack_end(LIVES_BOX(hbox), conx_buttons[idx], FALSE, FALSE, widget_opts.packing_width);
      lives_box_pack_end(LIVES_BOX(hbox), param_buttons[idx], FALSE, FALSE, widget_opts.packing_width);

      // set parameters button sensitive/insensitive
      set_param_and_con_buttons(i, j);
    }
  }

  scrolledwindow = lives_standard_scrolled_window_new(winsize_h, winsize_v, table);

  top_vbox = lives_vbox_new(FALSE, 0);

  lives_box_pack_start(LIVES_BOX(top_vbox), dummy_radio, FALSE, FALSE, 0);
  lives_box_pack_start(LIVES_BOX(top_vbox), scrolledwindow, TRUE, TRUE, widget_opts.packing_height);

  lives_container_add(LIVES_CONTAINER(irte_window), top_vbox);

  hbuttonbox = lives_hbutton_box_new();
  label = add_fill_to_box(LIVES_BOX(hbuttonbox));

  if (palette->style & STYLE_1) {
    lives_widget_set_bg_color(label, LIVES_WIDGET_STATE_NORMAL, &palette->menu_and_bars);
  }

  lives_box_pack_start(LIVES_BOX(top_vbox), hbuttonbox, FALSE, TRUE, widget_opts.packing_height * 2);

  clear_all_button = lives_dialog_add_button_from_stock(NULL, LIVES_STOCK_CLEAR, _("_Clear all effects"),
                     LIVES_RESPONSE_RESET);

  lives_container_add(LIVES_CONTAINER(hbuttonbox), clear_all_button);

  save_keymap_button = lives_dialog_add_button_from_stock(NULL, LIVES_STOCK_SAVE_AS, _("_Save as default keymap"),
                       LIVES_RESPONSE_ACCEPT);

  lives_container_add(LIVES_CONTAINER(hbuttonbox), save_keymap_button);

  load_keymap_button = lives_dialog_add_button_from_stock(NULL, LIVES_STOCK_OPEN, _("_Load default keymap"),
                       LIVES_RESPONSE_BROWSE);

  lives_container_add(LIVES_CONTAINER(hbuttonbox), load_keymap_button);

  ok_button = lives_dialog_add_button_from_stock(NULL, LIVES_STOCK_CLOSE, _("Close _window"),
              LIVES_RESPONSE_OK);

  lives_container_add(LIVES_CONTAINER(hbuttonbox), ok_button);

  label = add_fill_to_box(LIVES_BOX(hbuttonbox));

  if (palette->style & STYLE_1) {
    lives_widget_set_bg_color(label, LIVES_WIDGET_STATE_NORMAL, &palette->menu_and_bars);
  }

  lives_button_box_set_button_width(LIVES_BUTTON_BOX(hbuttonbox), clear_all_button, DEF_BUTTON_WIDTH);
  lives_button_box_set_button_width(LIVES_BUTTON_BOX(hbuttonbox), save_keymap_button, DEF_BUTTON_WIDTH);
  lives_button_box_set_button_width(LIVES_BUTTON_BOX(hbuttonbox), load_keymap_button, DEF_BUTTON_WIDTH);
  lives_button_box_set_button_width(LIVES_BUTTON_BOX(hbuttonbox), ok_button, DEF_BUTTON_WIDTH);

  rtew_accel_group = LIVES_ACCEL_GROUP(lives_accel_group_new());
  lives_window_add_accel_group(LIVES_WINDOW(irte_window), rtew_accel_group);

  lives_widget_add_accelerator(ok_button, LIVES_WIDGET_CLICKED_SIGNAL, rtew_accel_group,
                               LIVES_KEY_Escape, (LiVESXModifierType)0, (LiVESAccelFlags)0);

  lives_signal_connect(LIVES_GUI_OBJECT(irte_window), LIVES_WIDGET_DELETE_EVENT,
                       LIVES_GUI_CALLBACK(on_rtew_ok_clicked),
                       NULL);

  lives_signal_connect(LIVES_GUI_OBJECT(ok_button), LIVES_WIDGET_CLICKED_SIGNAL,
                       LIVES_GUI_CALLBACK(on_rtew_ok_clicked),
                       NULL);

  lives_signal_connect(LIVES_GUI_OBJECT(save_keymap_button), LIVES_WIDGET_CLICKED_SIGNAL,
                       LIVES_GUI_CALLBACK(on_save_keymap_clicked),
                       NULL);

  lives_signal_connect(LIVES_GUI_OBJECT(load_keymap_button), LIVES_WIDGET_CLICKED_SIGNAL,
                       LIVES_GUI_CALLBACK(on_load_keymap_clicked),
                       LIVES_INT_TO_POINTER(1));

  lives_signal_connect(LIVES_GUI_OBJECT(clear_all_button), LIVES_WIDGET_CLICKED_SIGNAL,
                       LIVES_GUI_CALLBACK(on_clear_all_clicked),
                       LIVES_INT_TO_POINTER(1));

rte_window_ready:
  // TODO: ignore button clicks until window is fully shown

  rte_window_is_hidden = FALSE;

  //lives_window_set_modal(LIVES_WINDOW(irte_window), TRUE);
  lives_widget_show_all(irte_window);

  if (prefs->open_maximised) {
    lives_window_maximize(LIVES_WINDOW(irte_window));
  }

  lives_widget_show_now(irte_window);

  lives_widget_set_sensitive(mainw->mt_menu, FALSE);
  lives_widget_set_sensitive(mainw->rte_defs_menu, FALSE);

  lives_set_cursor_style(LIVES_CURSOR_NORMAL, NULL);
  lives_set_cursor_style(LIVES_CURSOR_NORMAL, irte_window);
  mainw->no_context_update = FALSE;
  return irte_window;
}


LiVESWidget *refresh_rte_window(void) {
  if (rte_window != NULL) {
    lives_set_cursor_style(LIVES_CURSOR_BUSY, NULL);
    lives_set_cursor_style(LIVES_CURSOR_BUSY, rte_window);
    lives_widget_context_update();
    on_rtew_delete_event(NULL, NULL, NULL);
    lives_widget_destroy(rte_window);
    rte_window = create_rte_window();
    rte_window_set_interactive(prefs->interactive);
  }
  return rte_window;
}


void on_assign_rte_keys_activate(LiVESMenuItem * menuitem, livespointer user_data) {
  if (rte_window != NULL && !rte_window_is_hidden) {
    on_rtew_delete_event(NULL, NULL, NULL);
  } else {
    rte_window = create_rte_window();
    rte_window_set_interactive(prefs->interactive);
    lives_widget_show(rte_window);
    if (mainw->play_window != NULL && !mainw->fs && (prefs->play_monitor == widget_opts.monitor + 1 || capable->nmonitors == 1)) {
      lives_widget_hide(mainw->play_window);
      lives_window_set_transient_for(LIVES_WINDOW(mainw->play_window), LIVES_WINDOW(rte_window));
      lives_widget_show(mainw->play_window);
    }
  }
}


void rtew_set_keych(int key, boolean on) {
  lives_signal_handler_block(key_checks[key], ch_fns[key]);
  lives_toggle_button_set_active(LIVES_TOGGLE_BUTTON(key_checks[key]), on);
  lives_signal_handler_unblock(key_checks[key], ch_fns[key]);
  lives_widget_object_set_data(LIVES_WIDGET_OBJECT(key_checks[key]), "active", LIVES_INT_TO_POINTER(on));
}


void rtew_set_keygr(int key) {
  if (key >= 0) {
    lives_signal_handler_block(key_grabs[key], gr_fns[key]);
    lives_toggle_button_set_active(LIVES_TOGGLE_BUTTON(key_grabs[key]), TRUE);
    lives_signal_handler_unblock(key_grabs[key], gr_fns[key]);
  } else {
    lives_toggle_button_set_active(LIVES_TOGGLE_BUTTON(dummy_radio), TRUE);
  }
}


void rtew_set_mode_radio(int key, int mode) {
  int modes = rte_getmodespk();
  lives_signal_handler_block(mode_radios[key * modes + mode], mode_ra_fns[key * modes + mode]);
  lives_toggle_button_set_active(LIVES_TOGGLE_BUTTON(mode_radios[key * modes + mode]), TRUE);
  lives_signal_handler_unblock(mode_radios[key * modes + mode], mode_ra_fns[key * modes + mode]);
}


void update_pwindow(int key, int i, LiVESList * list) {
  // called only from weed_set_blend_factor() and from setting param in ce_thumbs

  weed_plant_t *inst;
  lives_rfx_t *rfx;
  int keyw, modew;

  if (fx_dialog[1] != NULL) {
    keyw = fx_dialog[1]->key;
    modew = fx_dialog[1]->mode;
    if (key == keyw) {
      if ((inst = rte_keymode_get_instance(key + 1, modew)) == NULL) return;
      weed_instance_unref(inst);
      rfx = fx_dialog[1]->rfx;
      mainw->block_param_updates = TRUE;
      set_param_from_list(list, &rfx->params[i], 0, TRUE, TRUE);
      mainw->block_param_updates = FALSE;
    }
  }
}


void rte_set_defs_activate(LiVESMenuItem * menuitem, livespointer user_data) {
  int idx = LIVES_POINTER_TO_INT(user_data);
  weed_plant_t *filter = get_weed_filter(idx);
  lives_rfx_t *rfx;

  if (fx_dialog[1] != NULL) {
    rfx = fx_dialog[1]->rfx;
    on_paramwindow_button_clicked(NULL, rfx);
    lives_widget_destroy(fx_dialog[1]->dialog);
    lives_freep((void **)&fx_dialog[1]);
  }

  rfx = weed_to_rfx(filter, TRUE);
  rfx->min_frames = -1;
  on_fx_pre_activate(rfx, TRUE, NULL);
}


void rte_set_key_defs(LiVESButton * button, lives_rfx_t *rfx) {
  int key, mode;
  if (mainw->textwidget_focus != NULL) {
    LiVESWidget *textwidget = (LiVESWidget *)lives_widget_object_get_data(LIVES_WIDGET_OBJECT(mainw->textwidget_focus),
                              "textwidget");
    after_param_text_changed(textwidget, rfx);
  }

  if (rfx->num_params > 0) {
    key = fx_dialog[1]->key;
    mode = fx_dialog[1]->mode;
    set_key_defaults((weed_plant_t *)rfx->source, key, mode);
  }
}


void rte_set_defs_ok(LiVESButton * button, lives_rfx_t *rfx) {
  weed_plant_t *ptmpl, *filter;

  lives_colRGB48_t *rgbp;

  register int i;

  if (mainw->textwidget_focus != NULL) {
    LiVESWidget *textwidget = (LiVESWidget *)lives_widget_object_get_data(LIVES_WIDGET_OBJECT(mainw->textwidget_focus),
                              "textwidget");
    after_param_text_changed(textwidget, rfx);
  }

  if (rfx->num_params > 0) {
    filter = weed_instance_get_filter((weed_plant_t *)rfx->source, TRUE);
    for (i = 0; i < rfx->num_params; i++) {
      ptmpl = weed_filter_in_paramtmpl(filter, i, FALSE);
      if (weed_paramtmpl_value_irrelevant(ptmpl)) continue;
      switch (rfx->params[i].type) {
      case LIVES_PARAM_COLRGB24:
        rgbp = (lives_colRGB48_t *)rfx->params[i].value;
        update_weed_color_value(filter, i, rgbp->red, rgbp->green, rgbp->blue, 0, rfx);
        break;
      case LIVES_PARAM_STRING:
        weed_set_string_value(ptmpl, WEED_LEAF_HOST_DEFAULT, (char *)rfx->params[i].value);
        break;
      case LIVES_PARAM_STRING_LIST:
        weed_set_int_array(ptmpl, WEED_LEAF_HOST_DEFAULT, 1, (int *)rfx->params[i].value);
        break;
      case LIVES_PARAM_NUM:
        if (weed_leaf_seed_type(ptmpl, WEED_LEAF_DEFAULT) == WEED_SEED_DOUBLE)
          weed_set_double_array(ptmpl, WEED_LEAF_HOST_DEFAULT, 1,
                                (double *)rfx->params[i].value);
        else weed_set_int_array(ptmpl, WEED_LEAF_HOST_DEFAULT, 1, (int *)rfx->params[i].value);
        break;
      case LIVES_PARAM_BOOL:
        weed_set_boolean_array(ptmpl, WEED_LEAF_HOST_DEFAULT, 1, (int *)rfx->params[i].value);
        break;
      default:
        break;
      }
    }
  }
}


void rte_set_defs_cancel(LiVESButton * button, lives_rfx_t *rfx) {
  on_paramwindow_button_clicked(button, rfx);
  lives_freep((void **)&fx_dialog[1]);
}


void rte_reset_defs_clicked(LiVESButton * button, lives_rfx_t *rfx) {
  weed_plant_t **ptmpls, **inp, **xinp;
  weed_plant_t **ctmpls;

  weed_plant_t *filter, *inst;

  LiVESList *child_list;

  LiVESWidget *pbox, *cancelbutton;

  boolean is_generic_defs = FALSE;
  boolean add_pcons = FALSE;

  int error;
  int nchans;

  int poffset = 0, ninpar, x;

  register int i;

  cancelbutton = fx_dialog[1]->cancelbutton;

  if (cancelbutton != NULL) is_generic_defs = TRUE;

  inst = (weed_plant_t *)rfx->source;

  filter = weed_instance_get_filter(inst, TRUE);

  if (rfx->num_params > 0) {
    if (is_generic_defs) {
      // for generic, reset from plugin supplied defs
      ptmpls = weed_get_plantptr_array(filter, WEED_LEAF_IN_PARAMETER_TEMPLATES, &error);
      for (i = 0; i < rfx->num_params; i++) {
        if (weed_plant_has_leaf(ptmpls[i], WEED_LEAF_HOST_DEFAULT)) weed_leaf_delete(ptmpls[i], WEED_LEAF_HOST_DEFAULT);
      }
      lives_free(ptmpls);
    }

    inp = weed_params_create(filter, TRUE);

resetdefs1:
    filter = weed_instance_get_filter(inst, FALSE);

    // reset params back to default defaults
    weed_in_parameters_free(inst);

    ninpar = num_in_params(filter, FALSE, FALSE);
    if (ninpar == 0) xinp = NULL;

    xinp = (weed_plant_t **)lives_malloc((ninpar + 1) * sizeof(weed_plant_t *));
    x = 0;
    for (i = poffset; i < poffset + ninpar; i++) xinp[x++] = inp[i];
    xinp[x] = NULL;
    poffset += ninpar;

    weed_set_plantptr_array(inst, WEED_LEAF_IN_PARAMETERS, weed_flagset_array_count(xinp, TRUE), xinp);
    lives_free(xinp);

    if (weed_plant_has_leaf(inst, WEED_LEAF_HOST_NEXT_INSTANCE)) {
      // handle compound fx
      inst = weed_get_plantptr_value(inst, WEED_LEAF_HOST_NEXT_INSTANCE, &error);
      add_pcons = TRUE;
      goto resetdefs1;
    }

    lives_free(inp);

    inst = (weed_plant_t *)rfx->source;
    filter = weed_instance_get_filter(inst, TRUE);

    if (add_pcons) {
      add_param_connections(inst);
    }

    rfx_params_free(rfx);
    lives_free(rfx->params);

    rfx->params = weed_params_to_rfx(rfx->num_params, inst, FALSE);
  }

  if (is_generic_defs) {
    if (weed_plant_has_leaf(filter, WEED_LEAF_HOST_FPS)) weed_leaf_delete(filter, WEED_LEAF_HOST_FPS);

    if (weed_plant_has_leaf(filter, WEED_LEAF_OUT_CHANNEL_TEMPLATES)) {
      ctmpls = weed_get_plantptr_array(filter, WEED_LEAF_OUT_CHANNEL_TEMPLATES, &error);
      nchans = weed_leaf_num_elements(filter, WEED_LEAF_OUT_CHANNEL_TEMPLATES);
      for (i = 0; i < nchans; i++) {
        if (weed_plant_has_leaf(ctmpls[i], WEED_LEAF_HOST_WIDTH)) weed_leaf_delete(ctmpls[i], WEED_LEAF_HOST_WIDTH);
        if (weed_plant_has_leaf(ctmpls[i], WEED_LEAF_HOST_HEIGHT)) weed_leaf_delete(ctmpls[i], WEED_LEAF_HOST_HEIGHT);
      }
    }
  } else {
    int key = fx_dialog[1]->key;
    int mode = fx_dialog[1]->mode;
    set_key_defaults(inst, key, mode);
  }

  if (!LIVES_IS_WIDGET(fx_dialog[1]->dialog)) return;
  pbox = lives_dialog_get_content_area(LIVES_DIALOG(fx_dialog[1]->dialog));

  // redraw the window
  child_list = lives_container_get_children(LIVES_CONTAINER(pbox));
  // remove focus from any widget we are ripping out
  lives_container_set_focus_child(LIVES_CONTAINER(pbox), NULL);
  for (i = 0; i < lives_list_length(child_list); i++) {
    LiVESWidget *widget = (LiVESWidget *)lives_list_nth_data(child_list, i);
    if (lives_widget_is_ancestor(LIVES_WIDGET(button), widget)) continue;
    lives_widget_destroy(widget);
  }

  if (child_list != NULL) lives_list_free(child_list);

  make_param_box(LIVES_VBOX(pbox), rfx);
  lives_widget_show_all(pbox);

  lives_widget_queue_draw(fx_dialog[1]->dialog);
}


void load_default_keymap(void) {
  // called on startup
  char *dir = lives_build_filename(prefs->configdir, LIVES_CONFIG_DIR, NULL);
  char *keymap_file = lives_build_filename(dir, "default.keymap", NULL);
  char *keymap_template = lives_build_filename(prefs->prefix_dir, DATA_DIR, "default.keymap", NULL);
  char *tmp;

  int retval;

  threaded_dialog_spin(0.);

  if (hash_list == NULL) hash_list = weed_get_all_names(FX_LIST_HASHNAME);

  if (prefs->startup_phase != 0 && prefs->startup_phase < 4) {
    do {
      retval = 0;
      if (!lives_file_test(keymap_file, LIVES_FILE_TEST_EXISTS)) {
        if (!lives_file_test(dir, LIVES_FILE_TEST_IS_DIR)) {
          lives_mkdir_with_parents(dir, S_IRWXU);
        }
        lives_cp(keymap_template, keymap_file);
      }
      if (!lives_file_test(keymap_file, LIVES_FILE_TEST_EXISTS)) {
        // give up
        d_print((tmp = lives_strdup_printf
                       (_("Unable to create default keymap file: %s\nPlease make sure the directory\n%s\nis writable.\n"),
                        keymap_file, dir)));

        retval = do_abort_cancel_retry_dialog(tmp, NULL);

        lives_free(tmp);

        if (retval == LIVES_RESPONSE_CANCEL) {
          lives_free(keymap_file);
          lives_free(keymap_template);
          lives_free(dir);

          threaded_dialog_spin(0.);
          return;
        }
      }
    } while (retval == LIVES_RESPONSE_RETRY);
  }

  on_load_keymap_clicked(NULL, NULL);

  lives_free(keymap_file);
  lives_free(keymap_template);
  lives_free(dir);
  threaded_dialog_spin(0.);
}
