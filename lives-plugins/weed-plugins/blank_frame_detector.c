// blank_frame_detector.c
// weed plugin
// (c) G. Finch (salsaman) 2012
//
// released under the GNU GPL 3 or later
// see file COPYING or www.gnu.org for details

///////////////////////////////////////////////////////////////////

static int package_version = 1; // version of this package

//////////////////////////////////////////////////////////////////
#define NEED_PALETTE_CONVERSIONS

#ifndef NEED_LOCAL_WEED_PLUGIN
#include <weed/weed-plugin.h>
#include <weed/weed-plugin-utils.h> // optional
#else
#include "../../libweed/weed-plugin.h"
#include "../../libweed/weed-plugin-utils.h" // optional
#endif

#include "weed-plugin-utils.c" // optional

/////////////////////////////////////////////////////////////

typedef struct {
  int count;
} _sdata;

////////////////////////////////////////////////////////////

static weed_error_t bfd_init(weed_plant_t *inst) {
  int error;
  weed_plant_t **out_params = weed_get_plantptr_array(inst, "out_parameters", &error);
  weed_plant_t *blank = out_params[0];
  _sdata *sdata;

  weed_set_boolean_value(blank, "value", WEED_FALSE);

  sdata = (_sdata *)weed_malloc(sizeof(_sdata));

  if (sdata == NULL) return WEED_ERROR_MEMORY_ALLOCATION;

  sdata->count = 0;

  weed_set_voidptr_value(inst, "plugin_internal", sdata);

  weed_free(out_params);

  return WEED_SUCCESS;
}


static weed_error_t bfd_deinit(weed_plant_t *inst) {
  int error;
  _sdata *sdata = (_sdata *)weed_get_voidptr_value(inst, "plugin_internal", &error);

  if (sdata != NULL) weed_free(sdata);

  return WEED_SUCCESS;
}


static weed_error_t bfd_process(weed_plant_t *inst, weed_timecode_t timestamp) {
  int error;
  weed_plant_t *in_channel = weed_get_plantptr_value(inst, "in_channels", &error);
  unsigned char *src = (unsigned char *)weed_get_voidptr_value(in_channel, "pixel_data", &error);
  int width = weed_get_int_value(in_channel, "width", &error);
  int height = weed_get_int_value(in_channel, "height", &error);
  int pal = weed_get_int_value(in_channel, "current_palette", &error);
  int irowstride = weed_get_int_value(in_channel, "rowstrides", &error);
  weed_plant_t **in_params = weed_get_plantptr_array(inst, "in_parameters", &error);
  weed_plant_t **out_params = weed_get_plantptr_array(inst, "out_parameters", &error);
  _sdata *sdata = (_sdata *)weed_get_voidptr_value(inst, "plugin_internal", &error);
  weed_plant_t *blank = out_params[0];
  int threshold = weed_get_int_value(in_params[0], "value", &error);
  int fcount = weed_get_int_value(in_params[1], "value", &error);
  int psize = 4;
  int luma;
  int start = 0;
  unsigned char *end = src + height * irowstride;
  register int i;

  for (; src < end; src += irowstride) {
    for (i = start; i < width; i += psize) {
      luma = calc_luma(&src[i], pal, weed_get_int_value(in_channel, WEED_LEAF_YUV_CLAMPING, NULL));
      if (luma > threshold) {
        // is not blank
        sdata->count = -1;
        break;
      }
    }
  }

  // is blank, but we need to check count

  sdata->count++;

  if (sdata->count >= fcount) weed_set_boolean_value(blank, "value", WEED_TRUE);
  else weed_set_boolean_value(blank, "value", WEED_FALSE);

  weed_free(in_params);
  weed_free(out_params);

  return WEED_SUCCESS;
}


WEED_SETUP_START(200, 200) {
  int palette_list[] = {WEED_PALETTE_BGR24, WEED_PALETTE_RGB24, WEED_PALETTE_RGBA32, WEED_PALETTE_BGRA32,
                        WEED_PALETTE_ARGB32, WEED_PALETTE_YUVA8888, WEED_PALETTE_YUV888, WEED_PALETTE_YUV444P,
                        WEED_PALETTE_YUVA4444P, WEED_PALETTE_YUV422P, WEED_PALETTE_YUV420P, WEED_PALETTE_YVU420P,
                        WEED_PALETTE_UYVY, WEED_PALETTE_YUYV,
                        WEED_PALETTE_END
                       };
  weed_plant_t *out_params[] = {weed_out_param_switch_init("blank", WEED_FALSE), NULL};
  weed_plant_t *in_params[] = {weed_integer_init("threshold", "Luma _threshold", 0, 0, 255),
                               weed_integer_init("fcount", "Frame _count", 1, 1, 1000), NULL
                              };
  weed_plant_t *in_chantmpls[] = {weed_channel_template_init("in channel 0", 0, palette_list), NULL};
  weed_plant_t *filter_class = weed_filter_class_init("blank_frame_detector", "salsaman", 1, 0, &bfd_init,
                               &bfd_process, &bfd_deinit, in_chantmpls, NULL, in_params, out_params);

  weed_plugin_info_add_filter_class(plugin_info, filter_class);
  weed_set_string_value(filter_class, "description",
                        "Counts successive frames whose maximum luma value is below the threshold value.\n"
                        "If the count surpasses the frame count limit the sets its output parameter to TRUE\n"
                        "otherwise it will be FALSE");

  weed_set_int_value(plugin_info, "version", package_version);

}
WEED_SETUP_END;

