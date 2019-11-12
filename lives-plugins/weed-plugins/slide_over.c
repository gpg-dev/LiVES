// simple_blend.c
// weed plugin
// (c) G. Finch (salsaman) 2005
//
// released under the GNU GPL 3 or later
// see file COPYING or www.gnu.org for details

///////////////////////////////////////////////////////////////////

static int package_version = 1; // version of this package

//////////////////////////////////////////////////////////////////

#define NEED_RANDOM

#ifndef NEED_LOCAL_WEED_PLUGIN
#include <weed/weed-plugin.h>
#include <weed/weed-plugin-utils.h> // optional
#else
#include "../../libweed/weed-plugin.h"
#include "../../libweed/weed-plugin-utils.h" // optional
#endif

#include "weed-plugin-utils.c"

/////////////////////////////////////////////////////////////

static inline int pick_direction(uint32_t fastrand_val) {
  return ((fastrand_val >> 24) & 0x03) + 1;
}


static weed_error_t sover_init(weed_plant_t *inst) {
  int dirpref;
  weed_plant_t **in_params = weed_get_plantptr_array(inst, "in_parameters", NULL);

  if (weed_get_boolean_value(in_params[1], "value", NULL) == WEED_TRUE) dirpref = 0;
  else if (weed_get_boolean_value(in_params[2], "value", NULL) == WEED_TRUE) dirpref = 1; // left to right
  else if (weed_get_boolean_value(in_params[3], "value", NULL) == WEED_TRUE) dirpref = 2; // right to left
  else if (weed_get_boolean_value(in_params[4], "value", NULL) == WEED_TRUE) dirpref = 3; // top to bottom
  else dirpref = 4; // bottom to top

  weed_set_int_value(inst, "plugin_direction", dirpref);
  return WEED_SUCCESS;
}


static weed_error_t sover_process(weed_plant_t *inst, weed_timecode_t timecode) {
  weed_plant_t **in_channels = weed_get_plantptr_array(inst, "in_channels", NULL), *out_channel = weed_get_plantptr_value(inst,
                               "out_channels",
                               NULL);
  unsigned char *src1 = weed_get_voidptr_value(in_channels[0], "pixel_data", NULL);
  unsigned char *src2 = weed_get_voidptr_value(in_channels[1], "pixel_data", NULL);
  unsigned char *dst = weed_get_voidptr_value(out_channel, "pixel_data", NULL);
  int width = weed_get_int_value(in_channels[0], "width", NULL);
  int height = weed_get_int_value(in_channels[0], "height", NULL);
  int irowstride1 = weed_get_int_value(in_channels[0], "rowstrides", NULL);
  int irowstride2 = weed_get_int_value(in_channels[1], "rowstrides", NULL);
  int orowstride = weed_get_int_value(out_channel, "rowstrides", NULL);
  weed_plant_t **in_params;

  register int j;

  int transval;
  int dirn;
  int mvlower, mvupper;
  int bound;

  in_params = weed_get_plantptr_array(inst, "in_parameters", NULL);
  transval = weed_get_int_value(in_params[0], "value", NULL);
  dirn = weed_get_int_value(inst, "plugin_direction", NULL);
  mvlower = weed_get_boolean_value(in_params[6], "value", NULL);
  mvupper = weed_get_boolean_value(in_params[7], "value", NULL);

  if (dirn == 0) {
    dirn = pick_direction(fastrand(0)); // random
    weed_set_int_value(inst, "plugin_direction", dirn);
  }

  // upper is src1, lower is src2
  // if mvupper, src1 moves, otherwise it stays fixed
  // if mvlower, src2 moves, otherwise it stays fixed

  // direction tells which way bound moves
  // bound is dividing line between src1 and src2

  switch (dirn) {
  case 3:
    // top to bottom
    bound = (float)height * (1. - transval / 255.); // how much of src1 to show
    if (mvupper) src1 += irowstride1 * (height - bound); // if mvupper, slide a part off the top
    for (j = 0; j < bound; j++) {
      weed_memcpy(dst, src1, width * 3);
      src1 += irowstride1;
      if (!mvlower) src2 += irowstride2; // if !mvlower, cover part of src2
      dst += orowstride;
    }
    for (j = bound; j < height; j++) {
      weed_memcpy(dst, src2, width * 3);
      src2 += irowstride2;
      dst += orowstride;
    }
    break;
  case 4:
    // bottom to top
    bound = (float)height * (transval / 255.);
    if (mvlower) src2 += irowstride2 * (height - bound); // if mvlower we slide in src2 from the top
    if (!mvupper) src1 += irowstride1 * bound;
    for (j = 0; j < bound; j++) {
      weed_memcpy(dst, src2, width * 3);
      src2 += irowstride2;
      dst += orowstride;
    }
    for (j = bound; j < height; j++) {
      weed_memcpy(dst, src1, width * 3);
      src1 += irowstride1;
      dst += orowstride;
    }
    break;
  case 1:
    // left to right
    bound = (float)width * (1. - transval / 255.);
    for (j = 0; j < height; j++) {
      weed_memcpy(dst, src1 + (width - bound) * 3 * mvupper, bound * 3);
      weed_memcpy(dst + bound * 3, src2 + bound * 3 * !mvlower, (width - bound) * 3);
      src1 += irowstride1;
      src2 += irowstride2;
      dst += orowstride;
    }
    break;
  case 2:
    // right to left
    bound = (float)width * (transval / 255.);
    for (j = 0; j < height; j++) {
      weed_memcpy(dst, src2 + (width - bound) * 3 * mvlower, bound * 3);
      weed_memcpy(dst + bound * 3, src1 + !mvupper * bound * 3, (width - bound) * 3);
      src1 += irowstride1;
      src2 += irowstride2;
      dst += orowstride;
    }
    break;
  }

  weed_free(in_params);
  weed_free(in_channels);
  return WEED_SUCCESS;
}


WEED_SETUP_START(200, 200) {
  int palette_list[] = {WEED_PALETTE_BGR24, WEED_PALETTE_RGB24, WEED_PALETTE_END};
  weed_plant_t *in_chantmpls[] = {weed_channel_template_init("in channel 0", 0, palette_list),
                                  weed_channel_template_init("in channel 1", 0, palette_list), NULL
                                 };
  weed_plant_t *out_chantmpls[] = {weed_channel_template_init("out channel 0", 0, palette_list), NULL};
  weed_plant_t *in_params[] = {weed_integer_init("amount", "Transition _value", 0, 0, 255),
                               weed_radio_init("dir_rand", "_Random", 1, 1),
                               weed_radio_init("dir_r2l", "_Right to left", 0, 1),
                               weed_radio_init("dir_l2r", "_Left to right", 0, 1),
                               weed_radio_init("dir_b2t", "_Bottom to top", 0, 1),
                               weed_radio_init("dir_t2b", "_Top to bottom", 0, 1),
                               weed_switch_init("mlower", "_Slide lower clip", WEED_TRUE),
                               weed_switch_init("mupper", "_Slide upper clip", WEED_FALSE), NULL
                              };

  weed_plant_t *filter_class = weed_filter_class_init("slide over", "salsaman", 1, 0, sover_init, sover_process, NULL,
                               in_chantmpls, out_chantmpls, in_params, NULL);

  weed_plant_t *gui = weed_filter_class_get_gui(filter_class);
  char *rfx_strings[] = {"layout|p0|", "layout|hseparator|", "layout|fill|\"Slide direction\"|fill|",
                         "layout|p1|", "layout|p2|p3|", "layout|p4|p5|", "layout|hseparator|"
                        };

  weed_set_string_value(gui, "layout_scheme", "RFX");
  weed_set_string_value(gui, "rfx_delim", "|");
  weed_set_string_array(gui, "rfx_strings", 7, rfx_strings);

  weed_set_boolean_value(in_params[0], "transition", WEED_TRUE);

  weed_set_int_value(in_params[1], "flags", WEED_PARAMETER_REINIT_ON_VALUE_CHANGE);
  weed_set_int_value(in_params[2], "flags", WEED_PARAMETER_REINIT_ON_VALUE_CHANGE);
  weed_set_int_value(in_params[3], "flags", WEED_PARAMETER_REINIT_ON_VALUE_CHANGE);
  weed_set_int_value(in_params[4], "flags", WEED_PARAMETER_REINIT_ON_VALUE_CHANGE);
  weed_set_int_value(in_params[5], "flags", WEED_PARAMETER_REINIT_ON_VALUE_CHANGE);

  weed_plugin_info_add_filter_class(plugin_info, filter_class);

  weed_set_int_value(plugin_info, "version", package_version);
}
WEED_SETUP_END;

