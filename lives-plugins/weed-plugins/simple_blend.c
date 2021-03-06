// simple_blend.c
// weed plugin
// (c) G. Finch (salsaman) 2005 - 2020
//
// released under the GNU GPL 3 or later
// see file COPYING or www.gnu.org for details

///////////////////////////////////////////////////////////////////

static int package_version = 1; // version of this package

//////////////////////////////////////////////////////////////////

#define NEED_PALETTE_CONVERSIONS
#define NEED_PALETTE_UTILS

#ifndef NEED_LOCAL_WEED_PLUGIN
#include <weed/weed-plugin.h>
#include <weed/weed-utils.h> // optional
#include <weed/weed-plugin-utils.h> // optional
#else
#include "../../libweed/weed-plugin.h"
#include "../../libweed/weed-utils.h" // optional
#include "../../libweed/weed-plugin-utils.h" // optional
#endif

#include "weed-plugin-utils.c"

/////////////////////////////////////////////////////////////

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct _sdata {
  volatile uint8_t obf;
  uint8_t blend[256][256];
} _sdata;


static inline void make_blend_table(_sdata *sdata, uint8_t bf, uint8_t bfn) {
  for (int i = 0; i < 256; i++) {
    for (int j = 0; j < 256; j++) sdata->blend[i][j] = (uint8_t)((bf * i + bfn * j) >> 8);
  }
}


/////////////////////////////////////////////////////////////////////////

static weed_error_t chroma_init(weed_plant_t *inst) {
  _sdata *sdata = weed_malloc(sizeof(_sdata));
  if (!sdata) return WEED_ERROR_MEMORY_ALLOCATION;
  sdata->obf = 0;
  make_blend_table(sdata, 0, 255);
  weed_set_voidptr_value(inst, "plugin_internal", sdata);
  return WEED_SUCCESS;
}


static weed_error_t chroma_deinit(weed_plant_t *inst) {
  _sdata *sdata = weed_get_voidptr_value(inst, "plugin_internal", NULL);
  if (sdata) weed_free(sdata);
  weed_set_voidptr_value(inst, "plugin_internal", NULL);
  return WEED_SUCCESS;
}


static weed_error_t common_process(int type, weed_plant_t *inst, weed_timecode_t timecode) {
  _sdata *sdata = NULL;

  weed_plant_t **in_channels = weed_get_in_channels(inst, NULL),
                 *out_channel = weed_get_out_channel(inst, 0);
  weed_plant_t *in_param;

  uint8_t *src1 = weed_channel_get_pixel_data(in_channels[0]);
  uint8_t *src2 = weed_channel_get_pixel_data(in_channels[1]);
  uint8_t *dst = weed_channel_get_pixel_data(out_channel);

  uint8_t blendneg, blend_factor;

  int pal = weed_channel_get_palette(out_channel);
  int bf, start = 0, row = 0, offset = 0, psize = pixel_size(pal);
  int width = weed_channel_get_width(out_channel) * psize;
  int height = weed_channel_get_height(out_channel);
  int irowstride1 = weed_channel_get_stride(in_channels[0]);
  int irowstride2 = weed_channel_get_stride(in_channels[1]);
  int orowstride = weed_channel_get_stride(out_channel);
  int inplace = (src1 == dst);

  uint8_t *end = dst + height * orowstride;

  if (pal == WEED_PALETTE_ARGB32) start = 1;

  in_param = weed_get_in_param(inst, 0);
  bf = weed_param_get_value_int(in_param);

  blend_factor = (uint8_t)bf;
  blendneg = 0xFF - blend_factor;

  if (weed_is_threading(inst)) {
    offset = weed_channel_get_offset(out_channel);
    src1 += offset * irowstride1;
    src2 += offset * irowstride2;
    dst += offset * orowstride;
    end += offset * orowstride;
  }

  if (type == 0) {
    sdata = (_sdata *)weed_get_voidptr_value(inst, "plugin_internal", NULL);
    if (sdata->obf != blend_factor) {
      /// ensure that only one thread updates the blend table
      if (offset == 0) {
        make_blend_table(sdata, blend_factor, blendneg);
        sdata->obf = blend_factor;
      } else while (sdata->obf != blend_factor) usleep(10);
    }
  }

  for (; dst < end; dst += orowstride) {
    for (int j = start; j < width; j += psize) {
      switch (type) {
      case 0:
        // chroma blend
        dst[j] = sdata->blend[src2[j]][src1[j]];
        dst[j + 1] = sdata->blend[src2[j + 1]][src1[j + 1]];
        dst[j + 2] = sdata->blend[src2[j + 2]][src1[j + 2]];
        break;
      case 4:
        // avg luma overlay
        if (j > start && j < width - 1 && row > 0 && row < height - 1) {
          uint8_t av_luma = (calc_luma(&src1[j], pal, 0) + calc_luma(&src1[j - 1], pal, 0)
                             + calc_luma(&src1[j + 1], pal, 0)
                             + calc_luma(&src1[j - irowstride1], pal, 0)
                             + calc_luma(&src1[j - 1 - irowstride1], pal, 0)
                             + calc_luma(&src1[j + 1 - irowstride1], pal, 0)
                             + calc_luma(&src1[j + irowstride1], pal, 0)
                             + calc_luma(&src1[j - 1 + irowstride1], pal, 0)
                             + calc_luma(&src1[j + 1 + irowstride1], pal, 0)) / 9;
          if (av_luma < (blend_factor)) weed_memcpy(&dst[j], &src2[j], 3);
          else if (!inplace) weed_memcpy(&dst[j], &src1[j], 3);
          row++;
          break;
        }
      case 1:
        // luma overlay
        if (calc_luma(&src1[j], pal, 0) < (blend_factor)) weed_memcpy(&dst[j], &src2[j], 3);
        else if (!inplace) weed_memcpy(&dst[j], &src1[j], 3);
        break;
      case 2:
        // luma underlay
        if (calc_luma(&src2[j], pal, 0) > (blendneg)) weed_memcpy(&dst[j], &src2[j], 3);
        else if (!inplace) weed_memcpy(&dst[j], &src1[j], 3);
        break;
      case 3:
        // neg lum overlay
        if (calc_luma(&src1[j], pal, 0) > (blendneg)) weed_memcpy(&dst[j], &src2[j], 3);
        else if (!inplace) weed_memcpy(&dst[j], &src1[j], 3);
        break;
      }
    }
    src1 += irowstride1;
    src2 += irowstride2;
  }
  weed_free(in_channels);
  return WEED_SUCCESS;
}


static weed_error_t chroma_process(weed_plant_t *inst, weed_timecode_t timestamp) {
  return common_process(0, inst, timestamp);
}

static weed_error_t lumo_process(weed_plant_t *inst, weed_timecode_t timestamp) {
  return common_process(1, inst, timestamp);
}

static weed_error_t lumu_process(weed_plant_t *inst, weed_timecode_t timestamp) {
  return common_process(2, inst, timestamp);
}

static weed_error_t nlumo_process(weed_plant_t *inst, weed_timecode_t timestamp) {
  return common_process(3, inst, timestamp);
}

static weed_error_t avlumo_process(weed_plant_t *inst, weed_timecode_t timestamp) {
  return common_process(4, inst, timestamp);
}


WEED_SETUP_START(200, 200) {
  weed_plant_t **clone1, **clone2, **clone3;
  int palette_list[] = ALL_RGB_PALETTES;
  weed_plant_t *in_chantmpls[] = {weed_channel_template_init("in channel 0", 0),
                                  weed_channel_template_init("in channel 1", 0), NULL
                                 };
  weed_plant_t *out_chantmpls[] = {weed_channel_template_init("out channel 0",
                                   WEED_CHANNEL_CAN_DO_INPLACE), NULL
                                  };
  weed_plant_t *in_params1[] = {weed_integer_init("amount", "Blend _amount", 128, 0, 255), NULL};
  weed_plant_t *in_params2[] = {weed_integer_init("threshold", "luma _threshold", 64, 0, 255), NULL};

  weed_plant_t *filter_class
    = weed_filter_class_init("chroma blend", "salsaman", 1,
                             WEED_FILTER_HINT_MAY_THREAD | WEED_FILTER_PREF_LINEAR_GAMMA, palette_list,
                             chroma_init, chroma_process, chroma_deinit,
                             in_chantmpls, out_chantmpls, in_params1, NULL);

  weed_paramtmpl_declare_transition(in_params1[0]);
  weed_paramtmpl_declare_transition(in_params2[0]);

  weed_plugin_info_add_filter_class(plugin_info, filter_class);

  filter_class = weed_filter_class_init("luma overlay", "salsaman", 1,
                                        WEED_FILTER_HINT_MAY_THREAD, palette_list,
                                        NULL, lumo_process, NULL,
                                        (clone1 = weed_clone_plants(in_chantmpls)),
                                        (clone2 = weed_clone_plants(out_chantmpls)),
                                        in_params2, NULL);

  weed_plugin_info_add_filter_class(plugin_info, filter_class);
  weed_free(clone1);
  weed_free(clone2);

  filter_class = weed_filter_class_init("luma underlay", "salsaman", 1,
                                        WEED_FILTER_HINT_MAY_THREAD, palette_list,
                                        NULL, lumu_process, NULL,
                                        (clone1 = weed_clone_plants(in_chantmpls)),
                                        (clone2 = weed_clone_plants(out_chantmpls)),
                                        (clone3 = weed_clone_plants(in_params2)), NULL);

  weed_plugin_info_add_filter_class(plugin_info, filter_class);
  weed_free(clone1);
  weed_free(clone2);
  weed_free(clone3);

  filter_class = weed_filter_class_init("negative luma overlay", "salsaman", 1,
                                        WEED_FILTER_HINT_MAY_THREAD, palette_list,
                                        NULL, nlumo_process, NULL,
                                        (clone1 = weed_clone_plants(in_chantmpls)),
                                        (clone2 = weed_clone_plants(out_chantmpls)),
                                        (clone3 = weed_clone_plants(in_params2)), NULL);

  weed_plugin_info_add_filter_class(plugin_info, filter_class);
  weed_free(clone1);
  weed_free(clone2);
  weed_free(clone3);

  filter_class = weed_filter_class_init("averaged luma overlay", "salsaman", 1,
                                        WEED_FILTER_PREF_LINEAR_GAMMA, palette_list,
                                        NULL, &avlumo_process, NULL,
                                        (clone1 = weed_clone_plants(in_chantmpls)),
                                        (clone2 = weed_clone_plants(out_chantmpls)),
                                        (clone3 = weed_clone_plants(in_params2)), NULL);

  weed_plugin_info_add_filter_class(plugin_info, filter_class);
  weed_free(clone1);
  weed_free(clone2);
  weed_free(clone3);

  weed_plugin_set_package_version(plugin_info, package_version);
}
WEED_SETUP_END;
