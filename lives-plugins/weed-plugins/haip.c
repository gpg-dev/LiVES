// haip.c
// weed plugin
// (c) G. Finch (salsaman) 2006 - 2012
//
// released under the GNU GPL 3 or later
// see file COPYING or www.gnu.org for details

///////////////////////////////////////////////////////////////////

static int package_version = 1; // version of this package

//////////////////////////////////////////////////////////////////

#define NEED_PALETTE_CONVERSIONS
#define NEED_RANDOM

#ifndef NEED_LOCAL_WEED_PLUGIN
#include <weed/weed-plugin.h>
#include <weed/weed-utils.h> // optional
#include <weed/weed-plugin-utils.h> // optional
#else
#include "../../libweed/weed-plugin.h"
#include "../../libweed/weed-utils.h" // optional
#include "../../libweed/weed-plugin-utils.h" // optional
#endif

#include "weed-plugin-utils.c" // optional

/////////////////////////////////////////////////////////////

typedef struct {
  int x;
  int y;
  int *px;
  int *py;
  int *wt;
  uint64_t fastrand_val;
  int old_width;
  int old_height;
} _sdata;

static int ress[8];

static weed_error_t haip_init(weed_plant_t *inst) {
  _sdata *sdata = weed_malloc(sizeof(_sdata));
  if (sdata == NULL) return WEED_ERROR_MEMORY_ALLOCATION;
  else {
    weed_plant_t **in_params = weed_get_in_params(inst, NULL);
    int num_wurms = weed_param_get_value_int(in_params[0]);
    weed_free(in_params);

    sdata->x = sdata->y = -1;

    weed_set_voidptr_value(inst, "plugin_internal", sdata);

    sdata->px = weed_malloc(num_wurms * sizeof(int));
    sdata->py = weed_malloc(num_wurms * sizeof(int));
    sdata->wt = weed_malloc(num_wurms * sizeof(int));

    for (int i = 0; i < num_wurms; i++) {
      sdata->px[i] = sdata->py[i] = -1;
    }

    sdata->old_width = sdata->old_height = -1;
    sdata->fastrand_val = fastrand(0);
    return WEED_SUCCESS;
  }
}


static weed_error_t haip_deinit(weed_plant_t *inst) {
  _sdata *sdata = weed_get_voidptr_value(inst, "plugin_internal", NULL);
  if (sdata) {
    if (sdata->wt) weed_free(sdata->wt);
    if (sdata->px) weed_free(sdata->px);
    if (sdata->py) weed_free(sdata->py);
    weed_free(sdata);
    weed_set_voidptr_value(inst, "plugin_internal", NULL);
  }
  return WEED_SUCCESS;
}


static int make_eight_table(unsigned char *pt, int row, int luma, int adj, int pal) {
  int n = 0;
  for (n = 0; n < 8; n++) ress[n] = -1;
  n = 0;
  if (calc_luma(&pt[-row - 3], pal, 0) >= (luma - adj)) {
    ress[n] = 0;
    n++;
  }
  if (calc_luma(&pt[-row], pal, 0) >= (luma - adj)) {
    ress[n] = 1;
    n++;
  }
  if (calc_luma(&pt[-row + 3], pal, 0) >= (luma - adj)) {
    ress[n] = 2;
    n++;
  }
  if (calc_luma(&pt[-3], pal, 0) >= (luma - adj)) {
    ress[n] = 3;
    n++;
  }
  if (calc_luma(&pt[3], pal, 0) >= (luma - adj)) {
    ress[n] = 4;
    n++;
  }
  if (calc_luma(&pt[row - 3], pal, 0) >= (luma - adj)) {
    ress[n] = 5;
    n++;
  }
  if (calc_luma(&pt[row], pal, 0) >= (luma - adj)) {
    ress[n] = 6;
    n++;
  }
  if (calc_luma(&pt[row + 3], pal, 0) >= (luma - adj)) {
    ress[n] = 7;
    n++;
  }
  return n;
}


static int select_dir(_sdata *sdata) {
  int num_choices = 1;
  int i;
  int mychoice;
  float fval;

  for (i = 0; i < 8; i++) {
    if (ress[i] != -1) num_choices++;
  }

  if (num_choices == 0) return 1;
  sdata->fastrand_val = fastrand(sdata->fastrand_val);
  fval = (float)((sdata->fastrand_val >> 16) & 0XFFFF) / 65535.;
  mychoice = (int)(fval * (float)(num_choices));

  switch (ress[mychoice]) {
  case 0:
    sdata->x--;
    sdata->y--;
    break;
  case 1:
    sdata->y--;
    break;
  case 2:
    sdata->x++;
    sdata->y--;
    break;
  case 3:
    sdata->x--;
    break;
  case 4:
    sdata->x++;
    break;
  case 5:
    sdata->x--;
    sdata->y++;
    break;
  case 6:
    sdata->y++;
    break;
  case 7:
    sdata->x++;
    sdata->y++;
    break;
  }
  return 0;
}


static inline void nine_fill(unsigned char *new_data, int row, unsigned char o0, unsigned char o1, unsigned char o2) {
  // fill nine pixels with the centre colour
  new_data[-row - 3] = new_data[-row] = new_data[-row + 3] = new_data[-3] = new_data[0] =
                                          new_data[3] = new_data[row - 3] = new_data[row] = new_data[row + 3] = o0;
  new_data[-row - 2] = new_data[-row + 1] = new_data[-row + 4] = new_data[-2] = new_data[1] =
                         new_data[4] = new_data[row - 2] = new_data[row + 1] = new_data[row + 4] = o1;
  new_data[-row - 1] = new_data[-row + 2] = new_data[-row + 5] = new_data[-1] = new_data[2] =
                         new_data[5] = new_data[row - 1] = new_data[row + 2] = new_data[row + 5] = o2;
}


static inline void black_fill(unsigned char *new_data, int row) {
  // fill nine pixels with black
  nine_fill(new_data, row, 0, 0, 0);
}


static inline void white_fill(unsigned char *new_data, int row) {
  // fill nine pixels with white
  nine_fill(new_data, row, 255, 255, 255);
}


static void proc_pt(unsigned char *dest, unsigned char *src, int x, int y, int orows,
                    int irows, int wt) {
  size_t offs;
  switch (wt) {
  case 0:
    black_fill(&dest[orows * y + x * 3], orows);
    break;
  case 1:
    white_fill(&dest[orows * y + x * 3], orows);
    break;
  case 2:
    offs = irows * y + x * 3;
    nine_fill(&dest[orows * y + x * 3], orows, src[offs], src[offs + 1], src[offs + 2]);
    break;
  }
}


static weed_error_t haip_process(weed_plant_t *inst, weed_timecode_t timestamp) {
  _sdata *sdata;

  weed_plant_t *in_channel = weed_get_in_channel(inst, 0),
                *out_channel = weed_get_out_channel(inst, 0);

  unsigned char *src = weed_channel_get_pixel_data(in_channel);
  unsigned char *dst = weed_channel_get_pixel_data(out_channel);
  weed_plant_t **in_params = weed_get_in_params(inst, NULL);
  int num_wurms = weed_param_get_value_int(in_params[0]);
  unsigned char *pt;

  int width = weed_channel_get_width(in_channel), width3 = width * 3;
  int height = weed_channel_get_height(in_channel);
  int irowstride = weed_channel_get_stride(in_channel);
  int orowstride = weed_channel_get_stride(out_channel);
  int palette = weed_channel_get_palette(in_channel);

  int count;
  int luma, adj;

  uint64_t fastrand_val;
  float scalex, scaley;

  register int i;

  weed_free(in_params);

  sdata = weed_get_voidptr_value(inst, "plugin_internal", NULL);
  sdata->fastrand_val = fastrand_val = fastrand(0);

  for (i = 0; i < height; i++) {
    weed_memcpy(&dst[i * orowstride], &src[i * irowstride], width3);
  }

  if (sdata->old_width == -1) {
    sdata->old_width = width;
    sdata->old_height = height;
  }

  scalex = (float)width / (float)sdata->old_width;
  scaley = (float)height / (float)sdata->old_height;

  for (i = 0; i < num_wurms; i++) {
    count = 1000;
    if (sdata->px[i] == -1) {
      sdata->fastrand_val = fastrand(sdata->fastrand_val);
      sdata->px[i] = (int)(((sdata->fastrand_val & 0xFF) / 255.*(width - 2))) + 1;
      sdata->fastrand_val = fastrand(sdata->fastrand_val);
      sdata->py[i] = (int)(((sdata->fastrand_val & 0xFF) / 255.*(height - 2))) + 1;
      sdata->fastrand_val = fastrand(sdata->fastrand_val);
      sdata->wt[i] = (int)(((sdata->fastrand_val & 0xFF) / 255.*2));
    }

    sdata->x = (float)sdata->px[i] * scalex;
    sdata->y = (float)sdata->py[i] * scaley;

    while (count > 0) {
      if (sdata->x < 1) sdata->x++;
      if (sdata->x > width - 2) sdata->x = width - 2;
      if (sdata->y < 1) sdata->y++;
      if (sdata->y > height - 2) sdata->y = height - 2;

      proc_pt(dst, src, sdata->x, sdata->y, orowstride, irowstride, sdata->wt[i]);

      if (sdata->x < 1) sdata->x++;
      if (sdata->x > width - 2) sdata->x = width - 2;
      if (sdata->y < 1) sdata->y++;
      if (sdata->y > height - 2) sdata->y = height - 2;
      pt = &src[sdata->y * irowstride + sdata->x * 3];

      luma = calc_luma(pt, palette, 0);
      adj = 0;

      make_eight_table(pt, irowstride, luma, adj, palette);
      if (((count << 7) >> 7) == count) select_dir(sdata);
      count--;
    }
    sdata->px[i] = sdata->x;
    sdata->py[i] = sdata->y;
  }

  sdata->old_width = width;
  sdata->old_height = height;

  return WEED_SUCCESS;
}


WEED_SETUP_START(200, 200) {
  int palette_list[] = {WEED_PALETTE_RGB24, WEED_PALETTE_BGR24, WEED_PALETTE_END};
  weed_plant_t *in_params[] = {weed_integer_init("nwurms", "Number of Wurms", 200, 1, 4092), NULL};
  weed_plant_t *in_chantmpls[] = {weed_channel_template_init("in channel 0", 0), NULL};
  weed_plant_t *out_chantmpls[] = {weed_channel_template_init("out channel 0", 0), NULL};
  weed_plant_t *filter_class = weed_filter_class_init("haip", "salsaman", 1, 0, palette_list,
                               haip_init, haip_process, haip_deinit,
                               in_chantmpls, out_chantmpls, in_params, NULL);
  weed_paramtmpl_set_flags(in_params[0], WEED_PARAMETER_REINIT_ON_VALUE_CHANGE);
  weed_plugin_info_add_filter_class(plugin_info, filter_class);
  weed_set_int_value(plugin_info, WEED_LEAF_VERSION, package_version);
}
WEED_SETUP_END;
