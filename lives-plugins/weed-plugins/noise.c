// noise.c
// Weed plugin
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


static weed_error_t noise_process(weed_plant_t *inst, weed_timecode_t timestamp) {
  weed_plant_t *in_channel = weed_get_plantptr_value(inst, "in_channels", NULL), *out_channel = weed_get_plantptr_value(inst,
                             "out_channels",
                             NULL);

  unsigned char *src = weed_get_voidptr_value(in_channel, "pixel_data", NULL);
  unsigned char *dst = weed_get_voidptr_value(out_channel, "pixel_data", NULL);
  int width = weed_get_int_value(in_channel, "width", NULL) * 3;
  int height = weed_get_int_value(in_channel, "height", NULL);
  int irowstride = weed_get_int_value(in_channel, "rowstrides", NULL);
  int orowstride = weed_get_int_value(out_channel, "rowstrides", NULL);
  unsigned char *end = src + height * irowstride;

  uint32_t fastrand_val = fastrand(0);

  register int j;

  // new threading arch
  if (weed_plant_has_leaf(out_channel, "offset")) {
    int offset = weed_get_int_value(out_channel, "offset", NULL);
    int dheight = weed_get_int_value(out_channel, "height", NULL);

    src += offset * irowstride;
    dst += offset * orowstride;
    end = src + dheight * irowstride;
  }

  for (; src < end; src += irowstride) {
    for (j = 0; j < width; j++) {
      dst[j] = src[j] + (((fastrand_val = fastrand(fastrand_val)) & 0xF8000000) >> 27) - 16;
    }
    dst += orowstride;
  }
  return WEED_SUCCESS;
}


WEED_SETUP_START(200, 200) {
  int palette_list[] = {WEED_PALETTE_BGR24, WEED_PALETTE_RGB24, WEED_PALETTE_END};

  weed_plant_t *in_chantmpls[] = {weed_channel_template_init("in channel 0", 0, palette_list), NULL};
  weed_plant_t *out_chantmpls[] = {weed_channel_template_init("out channel 0", WEED_CHANNEL_CAN_DO_INPLACE, palette_list), NULL};
  int filter_flags = WEED_FILTER_HINT_MAY_THREAD;

  weed_plant_t *filter_class  = weed_filter_class_init("noise", "salsaman", 1, filter_flags, NULL, noise_process, NULL,
                                in_chantmpls, out_chantmpls, NULL, NULL);

  weed_plugin_info_add_filter_class(plugin_info, filter_class);
  weed_set_int_value(plugin_info, "version", package_version);
}
WEED_SETUP_END;
