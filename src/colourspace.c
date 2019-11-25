// colourspace.c
// LiVES
// (c) G. Finch 2004 - 2019 <salsaman+lives@gmail.com>
// Released under the GPL 3 or later
// see file ../COPYING for licensing details

// code for palette conversions

/*
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA.
 *
 */

// *
// TODO -
//      - resizing of single plane (including bicubic) (maybe just triplicate the values and pretend it's RGB)
//      - external plugins for palette conversion, resizing
//      - convert yuv subspace and sampling type
//      - RGB(A) float, YUV10, etc.

#include <math.h>

#ifdef USE_SWSCALE

#include <libswscale/swscale.h>

// for libweed-compat.h
#define HAVE_AVCODEC
#define HAVE_AVUTIL

#endif // USE_SWSCALE

#include "main.h"

#include "cvirtual.h"
#include "effects-weed.h"

#ifdef USE_SWSCALE
#define N_SWS_CTX 16

boolean swscale_ctx_list_inited = FALSE;

struct _swscale_ctx {
  int iwidth;
  int iheight;
  int width;
  int height;
#ifdef FF_API_PIX_FMT
  enum PixelFormat ipixfmt;
  enum PixelFormat opixfmt;
#else
  enum AVPixelFormat ipixfmt;
  enum AVPixelFormat opixfmt;
#endif
  int flags;
  struct SwsContext *ctx;
};

static struct SwsContext *swscale = NULL;

//static struct _swscale_ctx swscale_ctx[N_SWS_CTX];

#endif // USE_SWSCALE

#define USE_THREADS 1

static pthread_t cthreads[MAX_FX_THREADS];

static boolean unal_inited = FALSE;

#ifdef GUI_GTK
// from gdk-pixbuf.c
/* Always align rows to 32-bit boundaries */
# define get_pixbuf_rowstride_value(rowstride) ((rowstride + 3) & ~3)
#else
# define get_pixbuf_rowstride_value(rowstride) (rowstride)
#endif

#ifdef GUI_GTK
// from gdkpixbuf
#define get_last_pixbuf_rowstride_value(width, nchans) (width * (((nchans << 3) + 7) >> 3))
#else
#define get_last_pixbuf_rowstride_value(width, nchans) (width * nchans)
#endif


static void lives_free_buffer(uint8_t *pixels, livespointer data) {
  lives_free(pixels);
}

#define CLAMP0255(a)  ((unsigned char)((((-a) >> 31) & a) | (255 - a) >> 31) )

/* precomputed tables */

// generic
static int *Y_R;
static int *Y_G;
static int *Y_B;
static int *Cb_R;
static int *Cb_G;
static int *Cb_B;
static int *Cr_R;
static int *Cr_G;
static int *Cr_B;

// clamped Y'CbCr
static int Y_Rc[256];
static int Y_Gc[256];
static int Y_Bc[256];
static int Cb_Rc[256];
static int Cb_Gc[256];
static int Cb_Bc[256];
static int Cr_Rc[256];
static int Cr_Gc[256];
static int Cr_Bc[256];

// unclamped Y'CbCr
static int Y_Ru[256];
static int Y_Gu[256];
static int Y_Bu[256];
static int Cb_Ru[256];
static int Cb_Gu[256];
static int Cb_Bu[256];
static int Cr_Ru[256];
static int Cr_Gu[256];
static int Cr_Bu[256];

// clamped BT.709
static int HY_Rc[256];
static int HY_Gc[256];
static int HY_Bc[256];
static int HCb_Rc[256];
static int HCb_Gc[256];
static int HCb_Bc[256];
static int HCr_Rc[256];
static int HCr_Gc[256];
static int HCr_Bc[256];

// unclamped BT.709
static int HY_Ru[256];
static int HY_Gu[256];
static int HY_Bu[256];
static int HCb_Ru[256];
static int HCb_Gu[256];
static int HCb_Bu[256];
static int HCr_Ru[256];
static int HCr_Gu[256];
static int HCr_Bu[256];

static boolean conv_RY_inited = FALSE;

// generic
static int *RGB_Y;
static int *R_Cr;
static int *G_Cb;
static int *G_Cr;
static int *B_Cb;

// clamped Y'CbCr
static int RGB_Yc[256];
static int R_Crc[256];
static int G_Cbc[256];
static int G_Crc[256];
static int B_Cbc[256];

// unclamped Y'CbCr
static int RGB_Yu[256];
static int R_Cru[256];
static int G_Cru[256];
static int G_Cbu[256];
static int B_Cbu[256];

// clamped BT.709
static int HRGB_Yc[256];
static int HR_Crc[256];
static int HG_Crc[256];
static int HG_Cbc[256];
static int HB_Cbc[256];

// unclamped BT.709
static int HRGB_Yu[256];
static int HR_Cru[256];
static int HG_Cru[256];
static int HG_Cbu[256];
static int HB_Cbu[256];

static boolean conv_YR_inited = FALSE;

static short min_Y, max_Y, min_UV, max_UV;

// averaging
static uint8_t *cavg;
static uint8_t cavgc[256][256];
static uint8_t cavgu[256][256];
static uint8_t cavgrgb[256][256];
static boolean avg_inited = FALSE;

// pre-post multiply alpha

static int unal[256][256];
static int al[256][256];
static int unalcy[256][256];
static int alcy[256][256];
static int unalcuv[256][256];
static int alcuv[256][256];

// clamping and subspace converters

// generic
static uint8_t *Y_to_Y;
static uint8_t *U_to_U;
static uint8_t *V_to_V;

// same subspace, clamped to unclamped
static uint8_t Yclamped_to_Yunclamped[256];
static uint8_t UVclamped_to_UVunclamped[256];

// same subspace, unclamped to clamped
static uint8_t Yunclamped_to_Yclamped[256];
static uint8_t UVunclamped_to_UVclamped[256];

static boolean conv_YY_inited = FALSE;

// gamma correction

uint8_t gamma_lut[256];
int current_gamma_from = WEED_GAMMA_UNKNOWN;
int current_gamma_to = WEED_GAMMA_UNKNOWN;

/* Updates the gamma look-up-table. */

static inline void update_gamma_lut(int gamma_from, int gamma_to) {
  register int i;
  float gamma = (float)prefs->screen_gamma, inv_gamma = 0.;
  float a, x = 0.;

  gamma_lut[0] = 0;

  if (gamma_to == WEED_GAMMA_MONITOR) {
    inv_gamma = 1. / gamma;
  } else if (current_gamma_from == gamma_from && current_gamma_to == gamma_to) return;

  for (i = 1; i < 256; ++i) {
    a = (float)i / 255.;

    switch (gamma_to) {
    // simple power law transformation
    case WEED_GAMMA_MONITOR:
      //if (fwd)
      x = powf(a, inv_gamma);
      //else
      //x = powf(a, gamma);
      break;

    // rec 709 gamma
    case WEED_GAMMA_BT709:
      switch (gamma_from) {
      case WEED_GAMMA_SRGB:
        a = (a <= 0.04045) ? a / 12.92 : powf((a + 0.055) / 1.055, 2.4);
      case WEED_GAMMA_LINEAR:
        x = (a <= 0.018) ? 4.5 * a : 1.099 * powf(a, 0.45) - 0.099;
        break;
      default:
        x = a;
        break;
      }
      break;

    // sRGB gamma
    case WEED_GAMMA_SRGB:
      switch (gamma_from) {
      case WEED_GAMMA_BT709:
        a = (a <= 0.081) ? a / 4.5 : powf((a + 0.099) / 1.099, 1. / 0.45);
      case WEED_GAMMA_LINEAR:
        x = (a <= 0.0031308) ? 12.92 * a : 1.055 * powf(a, 1.0 / 2.4) - 0.055;
        break;
      default:
        break;
      }
      break;

    case WEED_GAMMA_LINEAR:
      if (gamma_from == WEED_GAMMA_BT709)
        x = (a <= 0.081) ? a / 4.5 : powf((a + 0.099) / 1.099, 1. / 0.45);
      else {
        if (gamma_from == WEED_GAMMA_SRGB)
          x = (a <= 0.04045) ? a / 12.92 : powf((a + 0.055) / 1.055, 2.4);
      }
      break;

    default:
      x = a;
      break;
    }
    gamma_lut[i] = CLAMP0255((int32_t)(255. * x + .5));
  }

  current_gamma_from = gamma_from;
  current_gamma_to = gamma_to;
}


static void init_RGB_to_YUV_tables(void) {
  register int i;
  // Digital Y'UV proper [ITU-R BT.601-5] for digital NTSC (NTSC analog uses YIQ I think)
  // a.k.a CCIR 601, aka bt470bg (with gamma = 2.8 ?), bt470m (gamma = 2.2), aka SD
  // uses Kr = 0.299 and Kb = 0.114
  // offs U,V = 128

  // (I call this subspace YUV_SUBSPACE_YCBCR)

  // this is used for e.g. theora encoding, and for most video cards

  // input is linear RGB, output is gamma corrected Y'UV

  // bt.709 (HD)

  // input is linear RGB, output is gamma corrected Y'UV

  // except for bt2020 which gamma corrects the Y (only) after conversion (?)

  // there is also smpte 170 / smpte 240 (NTSC), bt.1886 (?), smpte2084, and bt2020

  // bt.1886 : gamma 2.4

  // bt2020: UHD, 10/12 bit colour

  double fac;

  for (i = 0; i < 256; i++) {
    Y_Rc[i] = myround(KR_YCBCR * (double)i
                      * CLAMP_FACTOR_Y * SCALE_FACTOR);   // Kr
    Y_Gc[i] = myround((1. - KR_YCBCR - KB_YCBCR) * (double)i
                      * CLAMP_FACTOR_Y * SCALE_FACTOR);   // Kb
    Y_Bc[i] = myround((KB_YCBCR * (double)i
                       * CLAMP_FACTOR_Y + YUV_CLAMP_MIN) * SCALE_FACTOR);

    fac = .5 / (1. - KB_YCBCR); // .564

    Cb_Rc[i] = myround(-fac * KR_YCBCR * (double)i
                       * CLAMP_FACTOR_UV  * SCALE_FACTOR); // -.16736
    Cb_Gc[i] = myround(-fac * (1. - KB_YCBCR - KR_YCBCR)  * (double)i
                       * CLAMP_FACTOR_UV * SCALE_FACTOR); // -.331264
    Cb_Bc[i] = myround((0.5 * (double)i
                        * CLAMP_FACTOR_UV + UV_BIAS) * SCALE_FACTOR);

    fac = .5 / (1. - KR_YCBCR); // .7133

    Cr_Rc[i] = myround((0.5 * (double)i
                        * CLAMP_FACTOR_UV + UV_BIAS) * SCALE_FACTOR);
    Cr_Gc[i] = myround(-fac * (1. - KB_YCBCR - KR_YCBCR) * (double)i
                       * CLAMP_FACTOR_UV * SCALE_FACTOR);
    Cr_Bc[i] = myround(-fac * KB_YCBCR * (double)i
                       * CLAMP_FACTOR_UV * SCALE_FACTOR);
  }

  for (i = 0; i < 256; i++) {
    Y_Ru[i] = myround(KR_YCBCR * (double)i
                      * SCALE_FACTOR);   // Kr
    Y_Gu[i] = myround((1. - KR_YCBCR - KB_YCBCR) * (double)i
                      * SCALE_FACTOR);   // Kb
    Y_Bu[i] = myround(KB_YCBCR * (double)i
                      * SCALE_FACTOR);

    fac = .5 / (1. - KB_YCBCR); // .564

    Cb_Ru[i] = myround(-fac * KR_YCBCR * (double)i
                       * SCALE_FACTOR); // -.16736
    Cb_Gu[i] = myround(-fac * (1. - KB_YCBCR - KR_YCBCR)  * (double)i
                       * SCALE_FACTOR); // -.331264
    Cb_Bu[i] = myround((0.5 * (double)i
                        + UV_BIAS) * SCALE_FACTOR);

    fac = .5 / (1. - KR_YCBCR); // .7133

    Cr_Ru[i] = myround((0.5 * (double)i
                        + UV_BIAS) * SCALE_FACTOR);
    Cr_Gu[i] = myround(-fac * (1. - KB_YCBCR - KR_YCBCR) * (double)i
                       * SCALE_FACTOR);
    Cr_Bu[i] = myround(-fac * KB_YCBCR * (double)i
                       * SCALE_FACTOR);
  }

  // Different values are used for hdtv, I call this subspace YUV_SUBSPACE_BT709

  // Kr = 0.2126
  // Kb = 0.0722

  // converting from one subspace to another is not recommended.

  for (i = 0; i < 256; i++) {
    HY_Rc[i] = myround(KR_BT709 * (double)i
                       * CLAMP_FACTOR_Y * SCALE_FACTOR);   // Kr
    HY_Gc[i] = myround((1. - KR_BT709 - KB_BT709) * (double)i
                       * CLAMP_FACTOR_Y * SCALE_FACTOR);   // Kb
    HY_Bc[i] = myround((KB_BT709 * (double)i
                        * CLAMP_FACTOR_Y + YUV_CLAMP_MIN) * SCALE_FACTOR);

    fac = .5 / (1. - KB_BT709); // .5389

    HCb_Rc[i] = myround(-fac * KR_BT709 * (double)i
                        * CLAMP_FACTOR_UV  * SCALE_FACTOR); // -.16736
    HCb_Gc[i] = myround(-fac * (1. - KB_BT709 - KR_BT709)  * (double)i
                        * CLAMP_FACTOR_UV * SCALE_FACTOR); // -.331264
    HCb_Bc[i] = myround((0.5 * (double)i
                         * CLAMP_FACTOR_UV + UV_BIAS) * SCALE_FACTOR);

    fac = .5 / (1. - KR_BT709); // .635

    HCr_Rc[i] = myround((0.5 * (double)i
                         * CLAMP_FACTOR_UV + UV_BIAS) * SCALE_FACTOR);
    HCr_Gc[i] = myround(-fac * (1. - KB_BT709 - KR_BT709) * (double)i
                        * CLAMP_FACTOR_UV * SCALE_FACTOR);
    HCr_Bc[i] = myround(-fac * KB_BT709 * (double)i
                        * CLAMP_FACTOR_UV * SCALE_FACTOR);

  }

  for (i = 0; i < 256; i++) {
    HY_Ru[i] = myround(KR_BT709 * (double)i
                       * SCALE_FACTOR);   // Kr
    HY_Gu[i] = myround((1. - KR_BT709 - KB_BT709) * (double)i
                       * SCALE_FACTOR);   // Kb
    HY_Bu[i] = myround(KB_BT709 * (double)i
                       * SCALE_FACTOR);

    fac = .5 / (1. - KB_BT709); // .5389

    HCb_Ru[i] = myround(-fac * KR_BT709 * (double)i
                        * SCALE_FACTOR); // -.16736
    HCb_Gu[i] = myround(-fac * (1. - KB_BT709 - KR_BT709)  * (double)i
                        * SCALE_FACTOR); // -.331264
    HCb_Bu[i] = myround((0.5 * (double)i
                         + UV_BIAS) * SCALE_FACTOR);

    fac = .5 / (1. - KR_BT709); // .635

    HCr_Ru[i] = myround((0.5 * (double)i
                         + UV_BIAS) * SCALE_FACTOR);
    HCr_Gu[i] = myround(-fac * (1. - KB_BT709 - KR_BT709) * (double)i
                        * SCALE_FACTOR);
    HCr_Bu[i] = myround(-fac * KB_BT709 * (double)i
                        * SCALE_FACTOR);
  }

  conv_RY_inited = TRUE;
}


static void init_YUV_to_RGB_tables(void) {
  register int i;

  // These values are for what I call YUV_SUBSPACE_YCBCR

  /* clip Y values under 16 */
  for (i = 0; i < YUV_CLAMP_MIN; i++) {
    RGB_Yc[i] = 0;
  }
  for (; i < Y_CLAMP_MAX; i++) {
    RGB_Yc[i] = myround(((double)i - YUV_CLAMP_MIN) / (Y_CLAMP_MAX - YUV_CLAMP_MIN) * 255. * SCALE_FACTOR);
  }
  /* clip Y values above 235 */
  for (; i < 256; i++) {
    RGB_Yc[i] = 255 * SCALE_FACTOR;
  }

  /* clip Cb/Cr values below 16 */
  for (i = 0; i < YUV_CLAMP_MIN; i++) {
    R_Crc[i] = 0;
    G_Crc[i] = 0;
    G_Cbc[i] = 0;
    B_Cbc[i] = 0;
  }
  for (; i < UV_CLAMP_MAX; i++) {
    R_Crc[i] = myround(2. * (1. - KR_YCBCR) * ((((double)i - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                       SCALE_FACTOR); // 2*(1-Kr)
    G_Crc[i] = myround(-.5 / (1. - KR_YCBCR) * ((((double)i - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                       SCALE_FACTOR);
    G_Cbc[i] = myround(-.5 / (1. - KB_YCBCR) * ((((double)i - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                       SCALE_FACTOR);
    B_Cbc[i] = myround(2. * (1. - KB_YCBCR) * ((((double)i - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                       SCALE_FACTOR); // 2*(1-Kb)
  }
  /* clip Cb/Cr values above 240 */
  for (; i < 256; i++) {
    R_Crc[i] = myround(2. * (1. - KR_YCBCR) * (((UV_CLAMP_MAX - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                       SCALE_FACTOR); // 2*(1-Kr)
    G_Crc[i] = myround(-.5 / (1. - KR_YCBCR) * (((UV_CLAMP_MAX - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                       SCALE_FACTOR);
    G_Cbc[i] = myround(-.5 / (1. - KB_YCBCR) * (((UV_CLAMP_MAX - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                       SCALE_FACTOR);
    B_Cbc[i] = myround(2. * (1. - KB_YCBCR) * (((UV_CLAMP_MAX - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                       SCALE_FACTOR); // 2*(1-Kb)
  }

  // unclamped Y'CbCr
  for (i = 0; i <= 255; i++) {
    RGB_Yu[i] = i * SCALE_FACTOR;
  }

  for (i = 0; i <= 255; i++) {
    R_Cru[i] = myround(2. * (1. - KR_YCBCR) * ((double)i - UV_BIAS) * SCALE_FACTOR); // 2*(1-Kr)
    G_Cru[i] = myround(-.5 / (1. - KR_YCBCR) * ((double)i - UV_BIAS) * SCALE_FACTOR);
    G_Cbu[i] = myround(-.5 / (1. - KB_YCBCR) * ((double)i - UV_BIAS) * SCALE_FACTOR);
    B_Cbu[i] = myround(2. * (1. - KB_YCBCR) * ((double)i - UV_BIAS) * SCALE_FACTOR); // 2*(1-Kb)
  }

  // These values are for what I call YUV_SUBSPACE_BT709

  /* clip Y values under 16 */
  for (i = 0; i < YUV_CLAMP_MIN; i++) {
    HRGB_Yc[i] = 0;
  }
  for (; i < Y_CLAMP_MAX; i++) {
    HRGB_Yc[i] = myround(((double)i - YUV_CLAMP_MIN) / (Y_CLAMP_MAX - YUV_CLAMP_MIN) * 255. * SCALE_FACTOR);
  }
  /* clip Y values above 235 */
  for (; i < 256; i++) {
    HRGB_Yc[i] = 255 * SCALE_FACTOR;
  }

  /* clip Cb/Cr values below 16 */
  for (i = 0; i < YUV_CLAMP_MIN; i++) {
    HR_Crc[i] = 0;
    HG_Crc[i] = 0;
    HG_Cbc[i] = 0;
    HB_Cbc[i] = 0;
  }
  for (; i < UV_CLAMP_MAX; i++) {
    HR_Crc[i] = myround(2. * (1. - KR_BT709) * ((((double)i - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                        SCALE_FACTOR); // 2*(1-Kr)
    HG_Crc[i] = myround(-.5 / (1. - KR_BT709) * ((((double)i - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                        SCALE_FACTOR);
    HG_Cbc[i] = myround(-.5 / (1. - KB_BT709) * ((((double)i - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                        SCALE_FACTOR);
    HB_Cbc[i] = myround(2. * (1. - KB_BT709) * ((((double)i - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN) * 255.) - UV_BIAS) *
                        SCALE_FACTOR); // 2*(1-Kb)
  }
  /* clip Cb/Cr values above 240 */
  for (; i < 256; i++) {
    HR_Crc[i] = myround(2. * (1. - KR_BT709) * (255. - UV_BIAS) * SCALE_FACTOR); // 2*(1-Kr)
    HG_Crc[i] = myround(-.5 / (1. - KR_BT709) * (255. - UV_BIAS) * SCALE_FACTOR);
    HG_Cbc[i] = myround(-.5 / (1. - KB_BT709) * (255. - UV_BIAS) * SCALE_FACTOR);
    HB_Cbc[i] = myround(2. * (1. - KB_BT709) * (255. - UV_BIAS) * SCALE_FACTOR); // 2*(1-Kb)
  }

  // unclamped Y'CbCr
  for (i = 0; i <= 255; i++) {
    HRGB_Yu[i] = i * SCALE_FACTOR;
  }

  for (i = 0; i <= 255; i++) {
    HR_Crc[i] = myround(2. * (1. - KR_BT709) * ((double)i - UV_BIAS) * SCALE_FACTOR); // 2*(1-Kr)
    HG_Crc[i] = myround(-.5 / (1. - KR_BT709) * ((double)i - UV_BIAS) * SCALE_FACTOR);
    HG_Cbc[i] = myround(-.5 / (1. - KB_BT709) * ((double)i - UV_BIAS) * SCALE_FACTOR);
    HB_Cbc[i] = myround(2. * (1. - KB_BT709) * ((double)i - UV_BIAS) * SCALE_FACTOR); // 2*(1-Kb)
  }
}


static void init_YUV_to_YUV_tables(void) {
  register int i;

  // init clamped -> unclamped, same subspace
  for (i = 0; i < YUV_CLAMP_MIN; i++) {
    Yclamped_to_Yunclamped[i] = 0;
  }
  for (; i < Y_CLAMP_MAX; i++) {
    Yclamped_to_Yunclamped[i] = myround((i - YUV_CLAMP_MIN) * 255. / (Y_CLAMP_MAX - YUV_CLAMP_MIN));
  }
  for (; i < 256; i++) {
    Yclamped_to_Yunclamped[i] = 255;
  }

  for (i = 0; i < YUV_CLAMP_MIN; i++) {
    UVclamped_to_UVunclamped[i] = 0;
  }
  for (; i < UV_CLAMP_MAX; i++) {
    UVclamped_to_UVunclamped[i] = myround((i - YUV_CLAMP_MIN) * 255. / (UV_CLAMP_MAX - YUV_CLAMP_MIN));
  }
  for (; i < 256; i++) {
    UVclamped_to_UVunclamped[i] = 255;
  }

  for (i = 0; i < 256; i++) {
    Yunclamped_to_Yclamped[i] = myround((i / 255.) * (Y_CLAMP_MAX - YUV_CLAMP_MIN) + YUV_CLAMP_MIN);
    UVunclamped_to_UVclamped[i] = myround((i / 255.) * (UV_CLAMP_MAX - YUV_CLAMP_MIN) + YUV_CLAMP_MIN);
  }

  conv_YY_inited = TRUE;
}


static void init_average(void) {
  short a, b, c;
  int x, y;
  for (x = 0; x < 256; x++) {
    for (y = 0; y < 256; y++) {
      a = (short)(x - 128);
      b = (short)(y - 128);
      if ((c = (a + b - ((a * b) >> 8) + 128)) > UV_CLAMP_MAXI) c = UV_CLAMP_MAXI;
      cavgc[x][y] = (uint8_t)(c > YUV_CLAMP_MINI ? c : YUV_CLAMP_MINI); // this is fine because headroom==footroom==16
      if ((c = (a + b - ((a * b) >> 8) + 128)) > 255) c = 255;
      cavgu[x][y] = (uint8_t)(c > 0 ? c : 0);
      cavgrgb[x][y] = (x + y) / 2;
    }
  }
  avg_inited = TRUE;
}


static void init_unal(void) {
  // premult to postmult and vice-versa

  register int i, j;

  for (i = 0; i < 256; i++) { //alpha val
    for (j = 0; j < 256; j++) { // val to be converted
      unal[i][j] = (float)j * 255. / (float)i;
      al[i][j] = (float)j * (float)i / 255.;

      // clamped versions
      unalcy[i][j] = ((j - YUV_CLAMP_MIN) / (Y_CLAMP_MAX - YUV_CLAMP_MIN)) / (float)i;
      alcy[i][j] = ((j - YUV_CLAMP_MIN) / (Y_CLAMP_MAX - YUV_CLAMP_MIN)) * (float)i;
      unalcuv[i][j] = ((j - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN)) / (float)i;
      alcuv[i][j] = ((j - YUV_CLAMP_MIN) / (UV_CLAMP_MAX - YUV_CLAMP_MIN)) * (float)i;
    }
  }
  unal_inited = TRUE;
}


static void set_conversion_arrays(int clamping, int subspace) {
  // set conversion arrays for RGB <-> YUV, also min/max YUV values
  // depending on clamping and subspace

  switch (subspace) {
  case WEED_YUV_SUBSPACE_YUV: // assume YCBCR
  case WEED_YUV_SUBSPACE_YCBCR:
    if (clamping == WEED_YUV_CLAMPING_CLAMPED) {
      Y_R = Y_Rc;
      Y_G = Y_Gc;
      Y_B = Y_Bc;

      Cr_R = Cr_Rc;
      Cr_G = Cr_Gc;
      Cr_B = Cr_Bc;

      Cb_R = Cb_Rc;
      Cb_G = Cb_Gc;
      Cb_B = Cb_Bc;

      RGB_Y = RGB_Yc;

      R_Cr = R_Crc;
      G_Cr = G_Crc;

      G_Cb = G_Cbc;
      B_Cb = B_Cbc;
    } else {
      Y_R = Y_Ru;
      Y_G = Y_Gu;
      Y_B = Y_Bu;

      Cr_R = Cr_Ru;
      Cr_G = Cr_Gu;
      Cr_B = Cr_Bu;

      Cb_R = Cb_Ru;
      Cb_G = Cb_Gu;
      Cb_B = Cb_Bu;

      RGB_Y = RGB_Yu;

      R_Cr = R_Cru;
      G_Cr = G_Cru;
      G_Cb = G_Cbu;
      B_Cb = B_Cbu;
    }
    break;
  case WEED_YUV_SUBSPACE_BT709:
    if (clamping == WEED_YUV_CLAMPING_CLAMPED) {
      Y_R = HY_Rc;
      Y_G = HY_Gc;
      Y_B = HY_Bc;

      Cr_R = HCr_Rc;
      Cr_G = HCr_Gc;
      Cr_B = HCr_Bc;

      Cb_R = HCb_Rc;
      Cb_G = HCb_Gc;
      Cb_B = HCb_Bc;

      RGB_Y = HRGB_Yc;

      R_Cr = HR_Crc;
      G_Cr = HG_Crc;
      G_Cb = HG_Cbc;
      B_Cb = HB_Cbc;
    } else {
      Y_R = HY_Ru;
      Y_G = HY_Gu;
      Y_B = HY_Bu;

      Cr_R = HCr_Ru;
      Cr_G = HCr_Gu;
      Cr_B = HCr_Bu;

      Cb_R = HCb_Ru;
      Cb_G = HCb_Gu;
      Cb_B = HCb_Bu;

      RGB_Y = HRGB_Yu;

      R_Cr = HR_Cru;
      G_Cr = HG_Cru;
      G_Cb = HG_Cbu;
      B_Cb = HB_Cbu;
    }
    break;
  }

  if (!avg_inited) init_average();

  if (clamping == WEED_YUV_CLAMPING_CLAMPED) {
    min_Y = min_UV = YUV_CLAMP_MIN;
    max_Y = Y_CLAMP_MAX;
    max_UV = UV_CLAMP_MAX;
    cavg = (uint8_t *)cavgc;
  } else {
    min_Y = min_UV = 0;
    max_Y = max_UV = 255;
    cavg = (uint8_t *)cavgu;
  }
}


static void get_YUV_to_YUV_conversion_arrays(int iclamping, int isubspace, int oclamping, int osubspace) {
  // get conversion arrays for YUV -> YUV depending on in/out clamping and subspace
  // currently only clamped <-> unclamped conversions are catered for, subspace conversions are not yet done
  char *errmsg = NULL;
  if (!conv_YY_inited) init_YUV_to_YUV_tables();

  switch (isubspace) {
  case WEED_YUV_SUBSPACE_YUV:
    LIVES_WARN("YUV subspace input not specified, assuming Y'CbCr");
  case WEED_YUV_SUBSPACE_YCBCR:
    switch (osubspace) {
    case WEED_YUV_SUBSPACE_YUV:
      LIVES_WARN("YUV subspace output not specified, assuming Y'CbCr");
    case WEED_YUV_SUBSPACE_YCBCR:
      if (iclamping == WEED_YUV_CLAMPING_CLAMPED) {
        //Y'CbCr clamped -> Y'CbCr unclamped
        Y_to_Y = Yclamped_to_Yunclamped;
        U_to_U = V_to_V = UVclamped_to_UVunclamped;
      } else {
        //Y'CbCr unclamped -> Y'CbCr clamped
        Y_to_Y = Yunclamped_to_Yclamped;
        U_to_U = V_to_V = UVunclamped_to_UVclamped;
      }
      break;
    // TODO - other subspaces
    default:
      errmsg = lives_strdup_printf("Invalid YUV subspace conversion %d to %d", isubspace, osubspace);
      LIVES_ERROR(errmsg);
    }
    break;
  case WEED_YUV_SUBSPACE_BT709:
    switch (osubspace) {
    case WEED_YUV_SUBSPACE_YUV:
      LIVES_WARN("YUV subspace output not specified, assuming BT709");
    case WEED_YUV_SUBSPACE_BT709:
      if (iclamping == WEED_YUV_CLAMPING_CLAMPED) {
        //BT.709 clamped -> BT.709 unclamped
        Y_to_Y = Yclamped_to_Yunclamped;
        U_to_U = V_to_V = UVclamped_to_UVunclamped;
      } else {
        //BT.709 unclamped -> BT.709 clamped
        Y_to_Y = Yunclamped_to_Yclamped;
        U_to_U = V_to_V = UVunclamped_to_UVclamped;
      }
      break;
    // TODO - other subspaces
    default:
      errmsg = lives_strdup_printf("Invalid YUV subspace conversion %d to %d", isubspace, osubspace);
      LIVES_ERROR(errmsg);
    }
    break;
  default:
    errmsg = lives_strdup_printf("Invalid YUV subspace conversion %d to %d", isubspace, osubspace);
    LIVES_ERROR(errmsg);
    break;
  }
  if (errmsg != NULL) lives_free(errmsg);
}

//////////////////////////
// pixel conversions

static uint8_t avg_chroma(size_t x, size_t y) GNU_HOT;
static void rgb2yuv(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t *y, uint8_t *u, uint8_t *v) GNU_HOT;
static void rgb2uyvy(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, uyvy_macropixel *uyvy) GNU_FLATTEN GNU_HOT;
static void rgb2yuyv(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, yuyv_macropixel *yuyv) GNU_FLATTEN GNU_HOT;
static void rgb2_411(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1,
                     uint8_t r2, uint8_t g2, uint8_t b2, uint8_t r3, uint8_t g3, uint8_t b3, yuv411_macropixel *yuv) GNU_HOT;
static void yuv2rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) GNU_HOT;
static void uyvy2rgb(uyvy_macropixel *uyvy, uint8_t *r0, uint8_t *g0, uint8_t *b0,
                     uint8_t *r1, uint8_t *g1, uint8_t *b1) GNU_FLATTEN GNU_HOT;
static void yuyv2rgb(yuyv_macropixel *yuyv, uint8_t *r0, uint8_t *g0, uint8_t *b0,
                     uint8_t *r1, uint8_t *g1, uint8_t *b1) GNU_FLATTEN GNU_HOT;
static void yuv888_2_rgb(uint8_t *yuv, uint8_t *rgb, boolean add_alpha) GNU_FLATTEN GNU_HOT;
static void yuva8888_2_rgba(uint8_t *yuva, uint8_t *rgba, boolean del_alpha) GNU_FLATTEN GNU_HOT;
static void yuv888_2_bgr(uint8_t *yuv, uint8_t *bgr, boolean add_alpha) GNU_FLATTEN GNU_HOT;
static void yuva8888_2_bgra(uint8_t *yuva, uint8_t *bgra, boolean del_alpha) GNU_FLATTEN GNU_HOT;
static void yuv888_2_argb(uint8_t *yuv, uint8_t *argb) GNU_FLATTEN GNU_HOT;
static void yuva8888_2_argb(uint8_t *yuva, uint8_t *argb) GNU_FLATTEN GNU_HOT;
static void uyvy_2_yuv422(uyvy_macropixel *uyvy, uint8_t *y0, uint8_t *u0, uint8_t *v0, uint8_t *y1) GNU_HOT;
static void yuyv_2_yuv422(yuyv_macropixel *yuyv, uint8_t *y0, uint8_t *u0, uint8_t *v0, uint8_t *y1) GNU_HOT;


LIVES_INLINE uint8_t avg_chroma(size_t x, size_t y) {
  // cavg == cavgc for clamped, cavgu for unclamped
  return *(cavg + (x << 8) + y);
}

LIVES_INLINE void rgb2yuv(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t *y, uint8_t *u, uint8_t *v) {
  register short a;
  if ((a = ((Y_R[r0] + Y_G[g0] + Y_B[b0]) >> FP_BITS)) > max_Y) a = max_Y;
  *y = a < min_Y ? min_Y : a;
  if ((a = ((Cb_R[r0] + Cb_G[g0] + Cb_B[b0]) >> FP_BITS)) > max_UV) a = max_UV;
  *u = a < min_UV ? min_UV : a;
  if ((a = ((Cr_R[r0] + Cr_G[g0] + Cr_B[b0]) >> FP_BITS)) > max_UV) a = max_UV;
  *v = a < min_UV ? min_UV : a;
}


LIVES_INLINE void rgb2uyvy(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, uyvy_macropixel *uyvy) {
  register short a;
  if ((a = ((Y_R[r0] + Y_G[g0] + Y_B[b0]) >> FP_BITS)) > max_Y) uyvy->y0 = max_Y;
  else uyvy->y0 = a < min_Y ? min_Y : a;
  if ((a = ((Y_R[r1] + Y_G[g1] + Y_B[b1]) >> FP_BITS)) > max_Y) uyvy->y1 = max_Y;
  else uyvy->y1 = a < min_Y ? min_Y : a;

  uyvy->u0 = avg_chroma((Cb_R[r0] + Cb_G[g0] + Cb_B[b0]) >> FP_BITS,
                        (Cb_R[r1] + Cb_G[g1] + Cb_B[b1]) >> FP_BITS);

  uyvy->v0 = avg_chroma((Cr_R[r0] + Cr_G[g0] + Cr_B[b0]) >> FP_BITS,
                        (Cr_R[r1] + Cr_G[g1] + Cr_B[b1]) >> FP_BITS);
}


LIVES_INLINE void rgb2yuyv(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, yuyv_macropixel *yuyv) {
  register short a;
  if ((a = ((Y_R[r0] + Y_G[g0] + Y_B[b0]) >> FP_BITS)) > max_Y) yuyv->y0 = max_Y;
  else yuyv->y0 = a < min_Y ? min_Y : a;
  if ((a = ((Y_R[r1] + Y_G[g1] + Y_B[b1]) >> FP_BITS)) > max_Y) yuyv->y1 = max_Y;
  else yuyv->y1 = a < min_Y ? min_Y : a;

  yuyv->u0 = avg_chroma((Cb_R[r0] + Cb_G[g0] + Cb_B[b0]) >> FP_BITS,
                        (Cb_R[r1] + Cb_G[g1] + Cb_B[b1]) >> FP_BITS);

  yuyv->v0 = avg_chroma((Cr_R[r0] + Cr_G[g0] + Cr_B[b0]) >> FP_BITS,
                        (Cr_R[r1] + Cr_G[g1] + Cr_B[b1]) >> FP_BITS);
}


LIVES_INLINE void rgb2_411(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1,
                           uint8_t r2, uint8_t g2, uint8_t b2, uint8_t r3, uint8_t g3, uint8_t b3, yuv411_macropixel *yuv) {
  register int a;
  if ((a = ((Y_R[r0] + Y_G[g0] + Y_B[b0]) >> FP_BITS)) > max_Y) yuv->y0 = max_Y;
  else yuv->y0 = a < min_Y ? min_Y : a;
  if ((a = ((Y_R[r1] + Y_G[g1] + Y_B[b1]) >> FP_BITS)) > max_Y) yuv->y1 = max_Y;
  else yuv->y1 = a < min_Y ? min_Y : a;
  if ((a = ((Y_R[r2] + Y_G[g2] + Y_B[b2]) >> FP_BITS)) > max_Y) yuv->y2 = max_Y;
  else yuv->y2 = a < min_Y ? min_Y : a;
  if ((a = ((Y_R[r3] + Y_G[g3] + Y_B[b3]) >> FP_BITS)) > max_Y) yuv->y3 = max_Y;
  else yuv->y3 = a < min_Y ? min_Y : a;

  if ((a = ((((Cr_R[r0] + Cr_G[g0] + Cr_B[b0]) >> FP_BITS) + ((Cr_R[r1] + Cr_G[g1] + Cr_B[b1]) >> FP_BITS) +
             ((Cr_R[r2] + Cr_G[g2] + Cr_B[b2]) >> FP_BITS) + ((Cr_R[r3] + Cr_G[g3] + Cr_B[b3]) >> FP_BITS)) >> 2)) > max_UV) yuv->v2 = max_UV;
  else yuv->v2 = a < min_UV ? min_UV : a;
  if ((a = ((((Cb_R[r0] + Cb_G[g0] + Cb_B[b0]) >> FP_BITS) + ((Cb_R[r1] + Cb_G[g1] + Cb_B[b1]) >> FP_BITS) +
             ((Cb_R[r2] + Cb_G[g2] + Cb_B[b2]) >> FP_BITS) + ((Cb_R[r3] + Cb_G[g3] + Cb_B[b3]) >> FP_BITS)) >> 2)) > max_UV) yuv->u2 = max_UV;
  else yuv->u2 = a < min_UV ? min_UV : a;
}


LIVES_INLINE void yuv2rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
  *r = CLAMP0255((int32_t)((RGB_Y[y] + R_Cr[v]) >> FP_BITS));
  *g = CLAMP0255((int32_t)((RGB_Y[y] + G_Cb[u] + G_Cr[v]) >> FP_BITS));
  *b = CLAMP0255((int32_t)((RGB_Y[y] + B_Cb[u]) >> FP_BITS));
}


LIVES_INLINE void uyvy2rgb(uyvy_macropixel *uyvy, uint8_t *r0, uint8_t *g0, uint8_t *b0,
                           uint8_t *r1, uint8_t *g1, uint8_t *b1) {
  yuv2rgb(uyvy->y0, uyvy->u0, uyvy->v0, r0, g0, b0);
  yuv2rgb(uyvy->y1, uyvy->u0, uyvy->v0, r1, g1, b1);
  //if (uyvy->y0>240||uyvy->u0>240||uyvy->v0>240||uyvy->y1>240) lives_printerr("got unclamped !\n");
}


LIVES_INLINE void yuyv2rgb(yuyv_macropixel *yuyv, uint8_t *r0, uint8_t *g0, uint8_t *b0,
                           uint8_t *r1, uint8_t *g1, uint8_t *b1) {
  yuv2rgb(yuyv->y0, yuyv->u0, yuyv->v0, r0, g0, b0);
  yuv2rgb(yuyv->y1, yuyv->u0, yuyv->v0, r1, g1, b1);
}


LIVES_INLINE void yuv888_2_rgb(uint8_t *yuv, uint8_t *rgb, boolean add_alpha) {
  yuv2rgb(yuv[0], yuv[1], yuv[2], &(rgb[0]), &(rgb[1]), &(rgb[2]));
  if (add_alpha) rgb[3] = 255;
}


LIVES_INLINE void yuva8888_2_rgba(uint8_t *yuva, uint8_t *rgba, boolean del_alpha) {
  yuv2rgb(yuva[0], yuva[1], yuva[2], &(rgba[0]), &(rgba[1]), &(rgba[2]));
  if (!del_alpha) rgba[3] = yuva[3];
}


LIVES_INLINE void yuv888_2_bgr(uint8_t *yuv, uint8_t *bgr, boolean add_alpha) {
  yuv2rgb(yuv[0], yuv[1], yuv[2], &(bgr[2]), &(bgr[1]), &(bgr[0]));
  if (add_alpha) bgr[3] = 255;
}


LIVES_INLINE void yuva8888_2_bgra(uint8_t *yuva, uint8_t *bgra, boolean del_alpha) {
  yuv2rgb(yuva[0], yuva[1], yuva[2], &(bgra[2]), &(bgra[1]), &(bgra[0]));
  if (!del_alpha) bgra[3] = yuva[3];
}


LIVES_INLINE void yuv888_2_argb(uint8_t *yuv, uint8_t *argb) {
  argb[0] = 255;
  yuv2rgb(yuv[0], yuv[1], yuv[2], &(argb[1]), &(argb[2]), &(argb[3]));
}


LIVES_INLINE void yuva8888_2_argb(uint8_t *yuva, uint8_t *argb) {
  argb[0] = yuva[3];
  yuv2rgb(yuva[0], yuva[1], yuva[2], &(argb[1]), &(argb[2]), &(argb[3]));
}


LIVES_INLINE void uyvy_2_yuv422(uyvy_macropixel *uyvy, uint8_t *y0, uint8_t *u0, uint8_t *v0, uint8_t *y1) {
  *u0 = uyvy->u0;
  *y0 = uyvy->y0;
  *v0 = uyvy->v0;
  *y1 = uyvy->y1;
}


LIVES_INLINE void yuyv_2_yuv422(yuyv_macropixel *yuyv, uint8_t *y0, uint8_t *u0, uint8_t *v0, uint8_t *y1) {
  *y0 = yuyv->y0;
  *u0 = yuyv->u0;
  *y1 = yuyv->y1;
  *v0 = yuyv->v0;
}

/////////////////////////////////////////////////
//utilities


LIVES_GLOBAL_INLINE boolean weed_palette_is_painter_palette(int pal) {
#ifdef LIVES_PAINTER_IS_CAIRO
  if (pal == WEED_PALETTE_A8 || pal == WEED_PALETTE_A1) return TRUE;
  if (capable->byte_order == LIVES_BIG_ENDIAN) {
    if (pal == WEED_PALETTE_ARGB32) return TRUE;
  } else {
    if (pal == WEED_PALETTE_BGRA32) return TRUE;
  }
#endif
  return FALSE;
}


boolean weed_palette_is_lower_quality(int p1, int p2) {
  // return TRUE if p1 is lower quality than p2
  // we don't yet handle float palettes, or RGB or alpha properly

  // currently only works well for YUV palettes

  if ((weed_palette_is_alpha(p1) && !weed_palette_is_alpha(p2)) ||
      (weed_palette_is_alpha(p2) && !weed_palette_is_alpha(p1))) return TRUE; // invalid conversion

  if (weed_palette_is_rgb(p1) && weed_palette_is_rgb(p2)) return FALSE;

  switch (p2) {
  case WEED_PALETTE_YUVA8888:
    if (p1 != WEED_PALETTE_YUVA8888 && p1 != WEED_PALETTE_YUVA4444P) return TRUE;
    break;
  case WEED_PALETTE_YUVA4444P:
    if (p1 != WEED_PALETTE_YUVA8888 && p1 != WEED_PALETTE_YUVA4444P) return TRUE;
    break;
  case WEED_PALETTE_YUV888:
    if (p1 != WEED_PALETTE_YUVA8888 && p1 != WEED_PALETTE_YUVA4444P && p1 != WEED_PALETTE_YUV444P && p1 != WEED_PALETTE_YUVA4444P)
      return TRUE;
    break;
  case WEED_PALETTE_YUV444P:
    if (p1 != WEED_PALETTE_YUVA8888 && p1 != WEED_PALETTE_YUVA4444P && p1 != WEED_PALETTE_YUV444P && p1 != WEED_PALETTE_YUVA4444P)
      return TRUE;
    break;

  case WEED_PALETTE_YUV422P:
  case WEED_PALETTE_UYVY8888:
  case WEED_PALETTE_YUYV8888:
    if (p1 != WEED_PALETTE_YUVA8888 && p1 != WEED_PALETTE_YUVA4444P && p1 != WEED_PALETTE_YUV444P &&
        p1 != WEED_PALETTE_YUVA4444P && p1 != WEED_PALETTE_YUV422P && p1 != WEED_PALETTE_UYVY8888 && p1 != WEED_PALETTE_YUYV8888)
      return TRUE;
    break;

  case WEED_PALETTE_YUV420P:
  case WEED_PALETTE_YVU420P:
    if (p1 == WEED_PALETTE_YUV411) return TRUE;
    break;
  case WEED_PALETTE_A8:
    if (p1 == WEED_PALETTE_A1) return TRUE;
  }
  return FALSE; // TODO
}

/////////////////////////////////////////////////////////

LIVES_GLOBAL_INLINE boolean lives_pixbuf_is_all_black(LiVESPixbuf *pixbuf) {
  int width = lives_pixbuf_get_width(pixbuf);
  int height = lives_pixbuf_get_height(pixbuf);
  int rstride = lives_pixbuf_get_rowstride(pixbuf);
  boolean has_alpha = lives_pixbuf_get_has_alpha(pixbuf);
  const uint8_t *pdata = lives_pixbuf_get_pixels_readonly(pixbuf);
  uint8_t a, b, c;
  int offs = 0;
  int psize = has_alpha ? 4 : 3;
  register int i, j;

  width *= psize;

  for (j = 0; j < height; j++) {
    for (i = offs; i < width; i += psize) {
      /** return FALSE if r >= 32, b >= 32 and g >= 24
      here we use a, b, and c for the first 3 bytes of the pixel. Since a and c are symmetric and we ignore byte 4,
      this will work for RGB, BGR, RGBA and BGRA (we could also check ARGB by setting offs to 1).

      Algorithm:
      (a & 0x1F) ^ a - nonzero iff a >= 32
      (c & 0x1F) ^ c - nonzero iff c >= 32

      ((a & c) & 0x1F) ^ (a & c) - nonzero only if both are true

      (b & 0x1F) ^ b  - nonzero iff b >= 32
      ((b << 1) & 0x1F) ^ (b << 1)  - nonzero iff b >= 16
      ((b << 2) & 0x1F) ^ (b << 2)  - nonzero iff b >= 8
      b & 0x0F - masks any values >= 32
       */
      a = pdata[i];
      b = pdata[i + 1];
      c = pdata[i + 2];

      if (((a & 0x1F) ^ a) & ((c & 0x1F) ^ c) & (((b & 0x1F) ^ b) | ((((b << 1) & 0x1F) ^ (b  << 1))
          & ((((b & 0x0F) << 2) & 0x1F) ^ ((b & 0x0F) << 2))))) return FALSE;
    }
    pdata += rstride;
  }
  return TRUE;
}


void pixel_data_planar_from_membuf(void **pixel_data, void *data, size_t size, int palette, boolean contig) {
  // convert contiguous memory block planes to planar data
  // size is the byte size of the Y plane (width*height in pixels)

  switch (palette) {
  case WEED_PALETTE_YUV444P:
    if (contig) lives_memcpy(pixel_data[0], data, size * 3);
    else {
      lives_memcpy(pixel_data[0], data, size);
      lives_memcpy(pixel_data[1], (uint8_t *)data + size, size);
      lives_memcpy(pixel_data[2], (uint8_t *)data + size * 2, size);
    }
    break;
  case WEED_PALETTE_YUVA4444P:
    if (contig) lives_memcpy(pixel_data[0], data, size * 4);
    else {
      lives_memcpy(pixel_data[0], data, size);
      lives_memcpy(pixel_data[1], (uint8_t *)data + size, size);
      lives_memcpy(pixel_data[2], (uint8_t *)data + size * 2, size);
      lives_memcpy(pixel_data[3], (uint8_t *)data + size * 2, size);
    }
    break;
  case WEED_PALETTE_YUV422P:
    if (contig) lives_memcpy(pixel_data[0], data, size * 2);
    else {
      lives_memcpy(pixel_data[0], data, size);
      lives_memcpy(pixel_data[1], (uint8_t *)data + size, size / 2);
      lives_memcpy(pixel_data[2], (uint8_t *)data + size * 3 / 2, size / 2);
    }
    break;
  case WEED_PALETTE_YUV420P:
  case WEED_PALETTE_YVU420P:
    if (contig) lives_memcpy(pixel_data[0], data, size * 3 / 2);
    else {
      lives_memcpy(pixel_data[0], data, size);
      lives_memcpy(pixel_data[1], (uint8_t *)data + size, size / 4);
      lives_memcpy(pixel_data[2], (uint8_t *)data + size * 5 / 4, size / 4);
    }
    break;
  }
}


///////////////////////////////////////////////////////////
// frame conversions

static void convert_yuv888_to_rgb_frame(uint8_t *src, int hsize, int vsize, int irowstride,
                                        int orowstride, uint8_t *dest, boolean add_alpha,
                                        boolean clamped, int thread_id) {
  register int x, y, i;
  size_t offs = 3;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    uint8_t *end = src + vsize * irowstride;
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].out_alpha = add_alpha;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuv888_to_rgb_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuv888_to_rgb_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (add_alpha) offs = 4;
  orowstride -= offs * hsize;
  irowstride -= hsize * 3;

  for (y = 0; y < vsize; y++) {
    for (x = 0; x < hsize; x++) {
      yuv888_2_rgb(src, dest, add_alpha);
      src += 3;
      dest += offs;
    }
    dest += orowstride;
    src += irowstride;
  }
}


void *convert_yuv888_to_rgb_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuv888_to_rgb_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                              ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->out_alpha,
                              ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuva8888_to_rgba_frame(uint8_t *src, int hsize, int vsize, int irowstride,
    int orowstride, uint8_t *dest, boolean del_alpha, boolean clamped,
    int thread_id) {
  register int x, y, i;

  size_t offs = 4;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    uint8_t *end = src + vsize * irowstride;
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].out_alpha = !del_alpha;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuva8888_to_rgba_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuva8888_to_rgba_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (del_alpha) offs = 3;
  orowstride -= offs * hsize;
  irowstride -= hsize * 4;

  for (y = 0; y < vsize; y++) {
    for (x = 0; x < hsize; x++) {
      yuva8888_2_rgba(src, dest, del_alpha);
      src += 4;
      dest += offs;
    }
    dest += orowstride;
    src += irowstride;
  }
}


void *convert_yuva8888_to_rgba_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuva8888_to_rgba_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                                 ccparams->orowstrides[0], (uint8_t *)ccparams->dest, !ccparams->out_alpha,
                                 ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuv888_to_bgr_frame(uint8_t *src, int hsize, int vsize, int irowstride,
                                        int orowstride, uint8_t *dest, boolean add_alpha, boolean clamped,
                                        int thread_id) {
  register int x, y, i;
  size_t offs = 3;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    uint8_t *end = src + vsize * irowstride;
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].out_alpha = add_alpha;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuv888_to_bgr_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuv888_to_bgr_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (add_alpha) offs = 4;
  orowstride -= offs * hsize;
  irowstride -= hsize * 3;

  for (y = 0; y < vsize; y++) {
    for (x = 0; x < hsize; x++) {
      yuv888_2_bgr(src, dest, add_alpha);
      src += 3;
      dest += offs;
    }
    dest += orowstride;
    src += irowstride;
  }
}


void *convert_yuv888_to_bgr_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuv888_to_bgr_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                              ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->out_alpha,
                              ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuva8888_to_bgra_frame(uint8_t *src, int hsize, int vsize, int irowstride,
    int orowstride, uint8_t *dest, boolean del_alpha, boolean clamped,
    int thread_id) {
  register int x, y, i;

  size_t offs = 4;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    uint8_t *end = src + vsize * irowstride;
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].out_alpha = !del_alpha;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuva8888_to_bgra_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuva8888_to_bgra_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (del_alpha) offs = 3;
  orowstride -= offs * hsize;
  irowstride -= 4 * hsize;

  for (y = 0; y < vsize; y++) {
    for (x = 0; x < hsize; x++) {
      yuva8888_2_bgra(src, dest, del_alpha);
      src += 4;
      dest += offs;
    }
    dest += orowstride;
    src += irowstride;
  }
}


void *convert_yuva8888_to_bgra_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuva8888_to_bgra_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                                 ccparams->orowstrides[0], (uint8_t *)ccparams->dest, !ccparams->out_alpha,
                                 ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuv888_to_argb_frame(uint8_t *src, int hsize, int vsize, int irowstride,
    int orowstride, uint8_t *dest,
    boolean clamped, int thread_id) {
  register int x, y, i;
  size_t offs = 4;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    uint8_t *end = src + vsize * irowstride;
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuv888_to_argb_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuv888_to_argb_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  orowstride -= offs * hsize;
  irowstride -= hsize * 3;

  for (y = 0; y < vsize; y++) {
    for (x = 0; x < hsize; x++) {
      yuv888_2_argb(src, dest);
      src += 3;
      dest += 4;
    }
    dest += orowstride;
    src += irowstride;
  }
}


void *convert_yuv888_to_argb_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuv888_to_argb_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                               ccparams->orowstrides[0], (uint8_t *)ccparams->dest,
                               ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuva8888_to_argb_frame(uint8_t *src, int hsize, int vsize, int irowstride,
    int orowstride, uint8_t *dest, boolean clamped,
    int thread_id) {
  register int x, y, i;

  size_t offs = 4;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    uint8_t *end = src + vsize * irowstride;
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuva8888_to_rgba_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuva8888_to_rgba_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  orowstride -= offs * hsize;
  irowstride -= hsize * 4;

  for (y = 0; y < vsize; y++) {
    for (x = 0; x < hsize; x++) {
      yuva8888_2_argb(src, dest);
      src += 4;
      dest += 4;
    }
    dest += orowstride;
    src += irowstride;
  }
}


void *convert_yuva8888_to_argb_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuva8888_to_argb_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                                 ccparams->orowstrides[0], (uint8_t *)ccparams->dest,
                                 ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuv420p_to_rgb_frame(uint8_t **src, int width, int height, int *istrides, int orowstride,
    uint8_t *dest, boolean add_alpha, boolean is_422, int subspace,
    boolean clamped) {
  register int i, j;
  uint8_t *s_y = src[0], *s_u = src[1], *s_v = src[2];
  boolean chroma = FALSE;
  int widthx;
  int opsize = 3, opsize2;
  int irow = istrides[0] - width;
  uint8_t y, u, v;

  if (add_alpha) opsize = 4;

  widthx = width * opsize;
  opsize2 = opsize * 2;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, subspace);

  for (i = 0; i < height; i++) {
    y = *(s_y++);
    u = s_u[0];
    v = s_v[0];

    yuv2rgb(y, u, v, &dest[0], &dest[1], &dest[2]);

    if (add_alpha) dest[3] = dest[opsize + 3] = 255;

    y = *(s_y++);
    dest += opsize;

    yuv2rgb(y, u, v, &dest[0], &dest[1], &dest[2]);
    dest -= opsize;

    for (j = opsize2; j < widthx; j += opsize2) {
      // process two pixels at a time, and we average the first colour pixel with the last from the previous 2
      // we know we can do this because Y must be even width

      // implements jpeg style subsampling : TODO - mpeg and dvpal style

      y = *(s_y++);
      u = s_u[(j / opsize) / 2];
      v = s_v[(j / opsize) / 2];

      yuv2rgb(y, u, v, &dest[j], &dest[j + 1], &dest[j + 2]);

      dest[j - opsize] = cavgrgb[dest[j - opsize]][dest[j]];
      dest[j - opsize + 1] = cavgrgb[dest[j - opsize + 1]][dest[j + 1]];
      dest[j - opsize + 2] = cavgrgb[dest[j - opsize + 2]][dest[j + 2]];

      y = *(s_y++);
      yuv2rgb(y, u, v, &dest[j + opsize], &dest[j + opsize + 1], &dest[j + opsize + 2]);

      if (add_alpha) dest[j + 3] = dest[j + 7] = 255;

      if (!is_422 && !chroma && i > 0) {
        // pass 2
        // average two src rows
        dest[j - orowstride] = cavgrgb[dest[j - orowstride]][dest[j]];
        dest[j + 1 - orowstride] = cavgrgb[dest[j + 1 - orowstride]][dest[j + 1]];
        dest[j + 2 - orowstride] = cavgrgb[dest[j + 2 - orowstride]][dest[j + 2]];
        dest[j - opsize - orowstride] = cavgrgb[dest[j - opsize - orowstride]][dest[j - opsize]];
        dest[j - opsize + 1 - orowstride] = cavgrgb[dest[j - opsize + 1 - orowstride]][dest[j - opsize + 1]];
        dest[j - opsize + 2 - orowstride] = cavgrgb[dest[j - opsize + 2 - orowstride]][dest[j - opsize + 2]];
      }
    }
    if (is_422 || chroma) {
      if (i > 0) {
        dest[j - opsize - orowstride] = cavgrgb[dest[j - opsize - orowstride]][dest[j - opsize]];
        dest[j - opsize + 1 - orowstride] = cavgrgb[dest[j - opsize + 1 - orowstride]][dest[j - opsize + 1]];
        dest[j - opsize + 2 - orowstride] = cavgrgb[dest[j - opsize + 2 - orowstride]][dest[j - opsize + 2]];
      }
      s_u += istrides[1];
      s_v += istrides[2];
    }
    s_y += irow;
    chroma = !chroma;
    dest += orowstride;
  }
}


static void convert_yuv420p_to_bgr_frame(uint8_t **src, int width, int height, int *istrides, int orowstride,
    uint8_t *dest, boolean add_alpha, boolean is_422, int subspace,
    boolean clamped) {
  // TODO - handle dvpal in sampling type
  register int i, j;
  uint8_t *s_y = src[0], *s_u = src[1], *s_v = src[2];
  boolean chroma = FALSE;
  int widthx;
  int irow = istrides[0] - width;
  int opsize = 3, opsize2;
  uint8_t y, u, v;

  if (add_alpha) opsize = 4;

  widthx = width * opsize;
  opsize2 = opsize * 2;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  for (i = 0; i < height; i++) {
    y = *(s_y++);
    u = s_u[0];
    v = s_v[0];

    yuv2rgb(y, u, v, &dest[2], &dest[1], &dest[0]);

    if (add_alpha) dest[3] = dest[opsize + 3] = 255;

    y = *(s_y++);
    dest += opsize;

    yuv2rgb(y, u, v, &dest[2], &dest[1], &dest[0]);
    dest -= opsize;

    for (j = opsize2; j < widthx; j += opsize2) {
      // process two pixels at a time, and we average the first colour pixel with the last from the previous 2
      // we know we can do this because Y must be even width

      // implements jpeg style subsampling : TODO - mpeg and dvpal style

      y = *(s_y++);
      u = s_u[(j / opsize) / 2];
      v = s_v[(j / opsize) / 2];

      yuv2rgb(y, u, v, &dest[j + 2], &dest[j + 1], &dest[j]);

      dest[j - opsize] = cavgrgb[dest[j - opsize]][dest[j]];
      dest[j - opsize + 1] = cavgrgb[dest[j - opsize + 1]][dest[j + 1]];
      dest[j - opsize + 2] = cavgrgb[dest[j - opsize + 2]][dest[j + 2]];

      y = *(s_y++);
      yuv2rgb(y, u, v, &dest[j + opsize + 2], &dest[j + opsize + 1], &dest[j + opsize]);

      if (add_alpha) dest[j + 3] = dest[j + 7] = 255;

      if (!is_422 && !chroma && i > 0) {
        // pass 2
        // average two src rows
        dest[j - orowstride] = cavgrgb[dest[j - orowstride]][dest[j]];
        dest[j + 1 - orowstride] = cavgrgb[dest[j + 1 - orowstride]][dest[j + 1]];
        dest[j + 2 - orowstride] = cavgrgb[dest[j + 2 - orowstride]][dest[j + 2]];
        dest[j - opsize - orowstride] = cavgrgb[dest[j - opsize - orowstride]][dest[j - opsize]];
        dest[j - opsize + 1 - orowstride] = cavgrgb[dest[j - opsize + 1 - orowstride]][dest[j - opsize + 1]];
        dest[j - opsize + 2 - orowstride] = cavgrgb[dest[j - opsize + 2 - orowstride]][dest[j - opsize + 2]];
      }
    }
    if (is_422 || chroma) {
      if (i > 0) {
        // TODO
        dest[j - opsize - orowstride] = cavgrgb[dest[j - opsize - orowstride]][dest[j - opsize]];
        dest[j - opsize + 1 - orowstride] = cavgrgb[dest[j - opsize + 1 - orowstride]][dest[j - opsize + 1]];
        dest[j - opsize + 2 - orowstride] = cavgrgb[dest[j - opsize + 2 - orowstride]][dest[j - opsize + 2]];
      }
      s_u += istrides[1];
      s_v += istrides[2];
    }
    s_y += irow;
    chroma = !chroma;
    dest += orowstride;
  }
}


static void convert_yuv420p_to_argb_frame(uint8_t **src, int width, int height, int *istrides, int orowstride,
    uint8_t *dest, boolean is_422, int subspace,
    boolean clamped) {
  // TODO - handle dvpal in sampling type
  register int i, j;
  uint8_t *s_y = src[0], *s_u = src[1], *s_v = src[2];
  boolean chroma = FALSE;
  int widthx;
  int irow = istrides[0] - width;
  int opsize = 4, opsize2;
  uint8_t y, u, v;

  widthx = width * opsize;
  opsize2 = opsize * 2;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  for (i = 0; i < height; i++) {
    y = *(s_y++);
    u = s_u[0];
    v = s_v[0];

    yuv2rgb(y, u, v, &dest[1], &dest[2], &dest[3]);

    dest[0] = dest[4] = 255;

    y = *(s_y++);
    dest += opsize;

    yuv2rgb(y, u, v, &dest[0], &dest[1], &dest[2]);
    dest -= opsize;

    for (j = opsize2; j < widthx; j += opsize2) {
      // process two pixels at a time, and we average the first colour pixel with the last from the previous 2
      // we know we can do this because Y must be even width

      // implements jpeg style subsampling : TODO - mpeg and dvpal style

      y = *(s_y++);
      u = s_u[(j / opsize) / 2];
      v = s_v[(j / opsize) / 2];

      yuv2rgb(y, u, v, &dest[j + 1], &dest[j + 2], &dest[j + 3]);

      dest[j - opsize + 1] = cavgrgb[dest[j - opsize + 1]][dest[j + 1]];
      dest[j - opsize + 2] = cavgrgb[dest[j - opsize + 2]][dest[j + 2]];
      dest[j - opsize + 3] = cavgrgb[dest[j - opsize + 3]][dest[j + 3]];

      y = *(s_y++);
      yuv2rgb(y, u, v, &dest[j + opsize + 1], &dest[j + opsize + 2], &dest[j + opsize + 3]);

      dest[j] = dest[j + 4] = 255;

      if (!is_422 && !chroma && i > 0) {
        // pass 2
        // average two src rows
        dest[j + 1 - orowstride] = cavgrgb[dest[j + 1 - orowstride]][dest[j + 1]];
        dest[j + 2 - orowstride] = cavgrgb[dest[j + 2 - orowstride]][dest[j + 2]];
        dest[j + 3 - orowstride] = cavgrgb[dest[j + 3 - orowstride]][dest[j + 3]];
        dest[j - opsize + 1 - orowstride] = cavgrgb[dest[j - opsize + 1 - orowstride]][dest[j - opsize + 1]];
        dest[j - opsize + 2 - orowstride] = cavgrgb[dest[j - opsize + 2 - orowstride]][dest[j - opsize + 2]];
        dest[j - opsize + 3 - orowstride] = cavgrgb[dest[j - opsize + 3 - orowstride]][dest[j - opsize + 3]];
      }
    }
    if (is_422 || chroma) {
      if (i > 0) {
        dest[j - opsize + 1 - orowstride] = cavgrgb[dest[j - opsize + 1 - orowstride]][dest[j - opsize + 1]];
        dest[j - opsize + 2 - orowstride] = cavgrgb[dest[j - opsize + 2 - orowstride]][dest[j - opsize + 2]];
        dest[j - opsize + 3 - orowstride] = cavgrgb[dest[j - opsize + 3 - orowstride]][dest[j - opsize + 3]];
      }
      s_u += istrides[1];
      s_v += istrides[2];
    }
    s_y += irow;
    chroma = !chroma;
    dest += orowstride;
  }
}


static void convert_rgb_to_uyvy_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride,
                                      uyvy_macropixel *u, boolean has_alpha, boolean clamped, int thread_id) {
  // for odd sized widths, cut the rightmost pixel
  int hs3, ipsize = 3, ipsize2;
  uint8_t *end;
  register int i;

  int x = 3, y = 4, z = 5;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  end = rgbdata + (rowstride * vsize) - 5;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = u + dheight * i * (hsize >> 1);

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].in_alpha = has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_rgb_to_uyvy_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_rgb_to_uyvy_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (has_alpha) {
    z++;
    y++;
    x++;
    ipsize = 4;
  }

  ipsize2 = ipsize * 2;
  hs3 = ((hsize >> 1) * ipsize2) - (ipsize2 - 1);

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < hs3; i += ipsize2) {
      // convert 6 RGBRGB bytes to 4 UYVY bytes
      rgb2uyvy(rgbdata[i], rgbdata[i + 1], rgbdata[i + 2], rgbdata[i + x], rgbdata[i + y], rgbdata[i + z], u++);
    }
  }
}


void *convert_rgb_to_uyvy_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_rgb_to_uyvy_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            (uyvy_macropixel *)ccparams->dest, ccparams->in_alpha, ccparams->out_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_rgb_to_yuyv_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride,
                                      yuyv_macropixel *u, boolean has_alpha, boolean clamped, int thread_id) {
  // for odd sized widths, cut the rightmost pixel
  int hs3, ipsize = 3, ipsize2;
  uint8_t *end = rgbdata + (rowstride * vsize) - 5;
  register int i;

  int x = 3, y = 4, z = 5;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = u + dheight * i * (hsize >> 1);

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].in_alpha = has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_rgb_to_yuyv_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_rgb_to_yuyv_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (has_alpha) {
    z++;
    y++;
    x++;
    ipsize = 4;
  }

  ipsize2 = ipsize * 2;
  hs3 = ((hsize >> 1) * ipsize2) - (ipsize2 - 1);

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < hs3; i += ipsize2) {
      // convert 6 RGBRGB bytes to 4 YUYV bytes
      rgb2yuyv(rgbdata[i], rgbdata[i + 1], rgbdata[i + 2], rgbdata[i + x], rgbdata[i + y], rgbdata[i + z], u++);
    }
  }
}


void *convert_rgb_to_yuyv_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_rgb_to_yuyv_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            (yuyv_macropixel *)ccparams->dest, ccparams->in_alpha, ccparams->out_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_bgr_to_uyvy_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride,
                                      uyvy_macropixel *u, boolean has_alpha, boolean clamped, int thread_id) {
  // for odd sized widths, cut the rightmost pixel
  int hs3, ipsize = 3, ipsize2;
  uint8_t *end = rgbdata + (rowstride * vsize) - 5;
  register int i;

  int x = 3, y = 4, z = 5;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = u + dheight * i * (hsize >> 1);

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].in_alpha = has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_bgr_to_uyvy_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_bgr_to_uyvy_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (has_alpha) {
    z++;
    y++;
    x++;
    ipsize = 4;
  }

  ipsize2 = ipsize * 2;
  hs3 = ((hsize >> 1) * ipsize2) - (ipsize2 - 1);

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < hs3; i += ipsize2) {
      // convert 6 RGBRGB bytes to 4 UYVY bytes
      rgb2uyvy(rgbdata[i + 2], rgbdata[i + 1], rgbdata[i], rgbdata[i + z], rgbdata[i + y], rgbdata[i + x], u++);
    }
  }
}


void *convert_bgr_to_uyvy_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_bgr_to_uyvy_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            (uyvy_macropixel *)ccparams->dest, ccparams->in_alpha, ccparams->out_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_bgr_to_yuyv_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride,
                                      yuyv_macropixel *u, boolean has_alpha, boolean clamped, int thread_id) {
  // for odd sized widths, cut the rightmost pixel
  int hs3, ipsize = 3, ipsize2;

  uint8_t *end = rgbdata + (rowstride * vsize) - 5;
  register int i;

  int x = 3, y = 4, z = 5;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = u + dheight * i * (hsize >> 1);

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].in_alpha = has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_bgr_to_yuyv_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_bgr_to_yuyv_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (has_alpha) {
    z++;
    y++;
    x++;
    ipsize = 4;
  }

  ipsize2 = ipsize * 2;
  hs3 = ((hsize >> 1) * ipsize2) - (ipsize2 - 1);

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < hs3; i += ipsize2) {
      // convert 6 RGBRGB bytes to 4 UYVY bytes
      rgb2yuyv(rgbdata[i + 2], rgbdata[i + 1], rgbdata[i], rgbdata[i + z], rgbdata[i + y], rgbdata[i + x], u++);
    }
  }
}


void *convert_bgr_to_yuyv_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_bgr_to_yuyv_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            (yuyv_macropixel *)ccparams->dest, ccparams->in_alpha, ccparams->out_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_argb_to_uyvy_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride,
                                       uyvy_macropixel *u, boolean clamped, int thread_id) {
  // for odd sized widths, cut the rightmost pixel
  int hs3, ipsize = 4, ipsize2;
  uint8_t *end;
  register int i;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  end = rgbdata + (rowstride * vsize) - 5;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = u + dheight * i * (hsize >> 1);

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_argb_to_uyvy_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_argb_to_uyvy_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  ipsize2 = ipsize * 2;
  hs3 = ((hsize >> 1) * ipsize2) - (ipsize2 - 1);

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < hs3; i += ipsize2) {
      // convert 6 RGBRGB bytes to 4 UYVY bytes
      rgb2uyvy(rgbdata[i + 1], rgbdata[i + 2], rgbdata[i + 3], rgbdata[i + 5], rgbdata[i + 6], rgbdata[i + 7], u++);
    }
  }
}


void *convert_argb_to_uyvy_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_argb_to_uyvy_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                             (uyvy_macropixel *)ccparams->dest, ccparams->out_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_argb_to_yuyv_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride,
                                       yuyv_macropixel *u, boolean clamped, int thread_id) {
  // for odd sized widths, cut the rightmost pixel
  int hs3, ipsize = 4, ipsize2;
  uint8_t *end;
  register int i;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  end = rgbdata + (rowstride * vsize) - 5;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = u + dheight * i * (hsize >> 1);

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_argb_to_yuyv_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_argb_to_yuyv_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  ipsize2 = ipsize * 2;
  hs3 = ((hsize >> 1) * ipsize2) - (ipsize2 - 1);

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < hs3; i += ipsize2) {
      // convert 6 RGBRGB bytes to 4 UYVY bytes
      rgb2yuyv(rgbdata[i + 1], rgbdata[i + 2], rgbdata[i + 3], rgbdata[i + 5], rgbdata[i + 6], rgbdata[i + 7], u++);
    }
  }
}


void *convert_argb_to_yuyv_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_argb_to_yuyv_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                             (yuyv_macropixel *)ccparams->dest, ccparams->out_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_rgb_to_yuv_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride, int orowst,
                                     uint8_t *u, boolean in_has_alpha, boolean out_has_alpha,
                                     boolean clamped, int thread_id) {
  int ipsize = 3, opsize = 3;
  int iwidth;
  uint8_t *end = rgbdata + (rowstride * vsize);
  register int i;
  int orow;
  uint8_t in_alpha = 255;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = u + dheight * i * orowst;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].orowstrides[0] = orowst;
        ccparams[i].in_alpha = in_has_alpha;
        ccparams[i].out_alpha = out_has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_rgb_to_yuv_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_rgb_to_yuv_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (in_has_alpha) ipsize = 4;

  if (out_has_alpha) opsize = 4;

  hsize = (hsize >> 1) << 1;
  iwidth = hsize * ipsize;
  orow = orowst - hsize * opsize;

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < iwidth; i += ipsize) {
      if (in_has_alpha) in_alpha = rgbdata[i + 3];
      if (out_has_alpha) u[3] = in_alpha;
      rgb2yuv(rgbdata[i], rgbdata[i + 1], rgbdata[i + 2], &(u[0]), &(u[1]), &(u[2]));
      u += opsize;
    }
    u += orow;
  }
}


void *convert_rgb_to_yuv_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_rgb_to_yuv_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                           ccparams->orowstrides[0],
                           (uint8_t *)ccparams->dest, ccparams->in_alpha, ccparams->out_alpha, ccparams->out_clamped,
                           ccparams->thread_id);
  return NULL;
}


static void convert_rgb_to_yuvp_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride, int orow,
                                      uint8_t **yuvp, boolean in_has_alpha, boolean out_has_alpha, boolean clamped,
                                      int thread_id) {
  int ipsize = 3;
  int iwidth;
  uint8_t *end = rgbdata + (rowstride * vsize);
  register int i;
  uint8_t in_alpha = 255, *a = NULL;

  uint8_t *y, *u, *v;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  y = yuvp[0];
  u = yuvp[1];
  v = yuvp[2];
  if (out_has_alpha) a = yuvp[3];

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;

        ccparams[i].destp[0] = y + dheight * i * orow;
        ccparams[i].destp[1] = u + dheight * i * orow;
        ccparams[i].destp[2] = v + dheight * i * orow;
        if (out_has_alpha) ccparams[i].destp[3] = a + dheight * i * hsize;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].orowstrides[0] = orow;
        ccparams[i].in_alpha = in_has_alpha;
        ccparams[i].out_alpha = out_has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_rgb_to_yuvp_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_rgb_to_yuvp_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (in_has_alpha) ipsize = 4;

  hsize = (hsize >> 1) << 1;
  iwidth = hsize * ipsize;
  orow -= hsize;

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < iwidth; i += ipsize) {
      if (in_has_alpha) in_alpha = rgbdata[i + 3];
      if (out_has_alpha) *(a++) = in_alpha;
      rgb2yuv(rgbdata[i], rgbdata[i + 1], rgbdata[i + 2], y, u, v);
      y++;
      u++;
      v++;
    }
    y += orow;
    u += orow;
    v += orow;
  }
}


void *convert_rgb_to_yuvp_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_rgb_to_yuvp_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            ccparams->orowstrides[0],
                            (uint8_t **)ccparams->destp, ccparams->in_alpha, ccparams->out_alpha, ccparams->out_clamped,
                            ccparams->thread_id);
  return NULL;
}


static void convert_bgr_to_yuv_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride, int orowst,
                                     uint8_t *u, boolean in_has_alpha, boolean out_has_alpha,
                                     boolean clamped, int thread_id) {
  int ipsize = 3, opsize = 3;
  int iwidth;
  uint8_t *end = rgbdata + (rowstride * vsize);
  int orow;
  register int i;
  uint8_t in_alpha = 255;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;
        ccparams[i].dest = u + dheight * i * orowst;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].in_alpha = in_has_alpha;
        ccparams[i].out_alpha = out_has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_bgr_to_yuv_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_bgr_to_yuv_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (in_has_alpha) ipsize = 4;

  if (out_has_alpha) opsize = 4;

  hsize = (hsize >> 1) << 1;
  iwidth = hsize * ipsize;
  orow = orowst - hsize * opsize;

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < iwidth; i += ipsize) {
      if (in_has_alpha) in_alpha = rgbdata[i + 3];
      if (out_has_alpha) u[3] = in_alpha;
      rgb2yuv(rgbdata[i + 2], rgbdata[i + 1], rgbdata[i], &(u[0]), &(u[1]), &(u[2]));
      u += opsize;
    }
    u += orow;
  }
}


void *convert_bgr_to_yuv_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_bgr_to_yuv_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                           ccparams->orowstrides[0],
                           (uint8_t *)ccparams->dest, ccparams->in_alpha, ccparams->out_alpha, ccparams->out_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_bgr_to_yuvp_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride, int orow,
                                      uint8_t **yuvp, boolean in_has_alpha, boolean out_has_alpha, boolean clamped,
                                      int thread_id) {
  // TESTED !

  int ipsize = 3;
  int iwidth;
  uint8_t *end = rgbdata + (rowstride * vsize);
  register int i;
  uint8_t in_alpha = 255, *a = NULL;

  uint8_t *y, *u, *v;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  y = yuvp[0];
  u = yuvp[1];
  v = yuvp[2];
  if (out_has_alpha) a = yuvp[3];

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;

        ccparams[i].destp[0] = y + dheight * i * orow;
        ccparams[i].destp[1] = u + dheight * i * orow;
        ccparams[i].destp[2] = v + dheight * i * orow;
        if (out_has_alpha) ccparams[i].destp[3] = a + dheight * i * hsize;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].orowstrides[0] = orow;
        ccparams[i].in_alpha = in_has_alpha;
        ccparams[i].out_alpha = out_has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_bgr_to_yuvp_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_bgr_to_yuvp_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (in_has_alpha) ipsize = 4;

  hsize = (hsize >> 1) << 1;
  iwidth = hsize * ipsize;
  orow -= hsize;

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < iwidth; i += ipsize) {
      if (in_has_alpha) in_alpha = rgbdata[i + 3];
      if (out_has_alpha) *(a++) = in_alpha;
      rgb2yuv(rgbdata[i + 2], rgbdata[i + 1], rgbdata[i], &(y[0]), &(u[0]), &(v[0]));
      y++;
      u++;
      v++;
    }
    y += orow;
    u += orow;
    v += orow;
  }
}


void *convert_bgr_to_yuvp_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_bgr_to_yuvp_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            ccparams->orowstrides[0],
                            (uint8_t **)ccparams->destp, ccparams->in_alpha, ccparams->out_alpha, ccparams->out_clamped,
                            ccparams->thread_id);
  return NULL;
}


static void convert_argb_to_yuv_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride, int orowst,
                                      uint8_t *u, boolean out_has_alpha,
                                      boolean clamped, int thread_id) {
  int ipsize = 4, opsize = 3;
  int iwidth, orow;
  uint8_t *end = rgbdata + (rowstride * vsize);
  register int i;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;

        ccparams[i].dest = u + dheight * i * orowst;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].orowstrides[0] = orowst;
        ccparams[i].out_alpha = out_has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_rgb_to_yuv_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_rgb_to_yuv_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (out_has_alpha) opsize = 4;

  hsize = (hsize >> 1) << 1;
  iwidth = hsize * ipsize;
  orow = orowst - hsize * opsize;

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < iwidth; i += ipsize) {
      if (out_has_alpha) u[3] = rgbdata[i];
      rgb2yuv(rgbdata[i + 1], rgbdata[i + 2], rgbdata[i + 3], &(u[0]), &(u[1]), &(u[2]));
      u += opsize;
    }
    u += orow;
  }
}


void *convert_argb_to_yuv_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_argb_to_yuv_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            ccparams->orowstrides[0],
                            (uint8_t *)ccparams->dest, ccparams->out_alpha, ccparams->out_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_argb_to_yuvp_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride, int orow,
                                       uint8_t **yuvp, boolean out_has_alpha, boolean clamped,
                                       int thread_id) {
  int ipsize = 4;
  int iwidth;
  uint8_t *end = rgbdata + (rowstride * vsize);
  register int i;
  uint8_t *a = NULL;
  uint8_t *y, *u, *v;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  y = yuvp[0];
  u = yuvp[1];
  v = yuvp[2];
  if (out_has_alpha) a = yuvp[3];

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED,
                          WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)vsize / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((rgbdata + dheight * i * rowstride) < end) {
        ccparams[i].src = rgbdata + dheight * i * rowstride;
        ccparams[i].hsize = hsize;

        ccparams[i].destp[0] = y + dheight * i * orow;
        ccparams[i].destp[1] = u + dheight * i * orow;
        ccparams[i].destp[2] = v + dheight * i * orow;
        if (out_has_alpha) ccparams[i].destp[3] = a + dheight * i * hsize;

        if (dheight * (i + 1) > (vsize - 4)) {
          dheight = vsize - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = rowstride;
        ccparams[i].orowstrides[0] = orow;
        ccparams[i].out_alpha = out_has_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_argb_to_yuvp_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_argb_to_yuvp_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED,
                        WEED_YUV_SUBSPACE_YCBCR);
#endif

  hsize = (hsize >> 1) << 1;
  iwidth = hsize * ipsize;
  orow -= hsize;

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < iwidth; i += ipsize) {
      if (out_has_alpha) *(a++) = rgbdata[i];
      rgb2yuv(rgbdata[i + 1], rgbdata[i + 2], rgbdata[i + 3], y, u, v);
      y++;
      u++;
      v++;
    }
    y += orow;
    u += orow;
    v += orow;
  }
}


void *convert_argb_to_yuvp_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_argb_to_yuvp_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                             ccparams->orowstrides[0],
                             (uint8_t **)ccparams->destp, ccparams->out_alpha, ccparams->out_clamped,
                             ccparams->thread_id);
  return NULL;
}


static void convert_rgb_to_yuv420_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride, int *ostrides,
                                        uint8_t **dest, boolean is_422, boolean has_alpha, int subspace, boolean clamped) {
  // for odd sized widths, cut the rightmost pixel
  // TODO - handle different out sampling types
  int hs3;

  uint8_t *y, *Cb, *Cr;
  uyvy_macropixel u;
  register int i, j;
  boolean chroma_row = TRUE;

  int ipsize = 3, ipsize2;
  size_t hhsize;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, subspace);

  if (has_alpha) ipsize = 4;

  // ensure width and height are both divisible by two
  hsize = (hsize >> 1) << 1;
  vsize = (vsize >> 1) << 1;

  y = dest[0];
  Cb = dest[1];
  Cr = dest[2];

  hhsize = hsize >> 1;
  ipsize2 = ipsize * 2;
  hs3 = (hsize * ipsize) - (ipsize2 - 1);

  for (i = 0; i < vsize; i++) {
    for (j = 0; j < hs3; j += ipsize2) {
      // mpeg style, Cb and Cr are co-located
      // convert 6 RGBRGB bytes to 4 UYVY bytes

      // TODO: for mpeg use rgb2yuv and write alternate u and v

      rgb2uyvy(rgbdata[j], rgbdata[j + 1], rgbdata[j + 2], rgbdata[j + ipsize], rgbdata[j + ipsize + 1], rgbdata[j + ipsize + 2], &u);

      *(y++) = u.y0;
      *(y++) = u.y1;
      *(Cb++) = u.u0;
      *(Cr++) = u.v0;

      if (!is_422 && chroma_row && i > 0) {
        // average two rows
        Cb[-1 - ostrides[1]] = avg_chroma(Cb[-1], Cb[-1 - ostrides[1]]);
        Cr[-1 - ostrides[1]] = avg_chroma(Cr[-1], Cr[-1 - ostrides[1]]);
      }

    }
    if (!is_422) {
      if (chroma_row) {
        Cb -= hhsize;
        Cr -= hhsize;
      }
      chroma_row = !chroma_row;
    }
    rgbdata += rowstride;
  }
}


static void convert_argb_to_yuv420_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride, int *ostrides,
    uint8_t **dest, boolean is_422, int subspace, boolean clamped) {
  // for odd sized widths, cut the rightmost pixel
  // TODO - handle different out sampling types
  int hs3;

  uint8_t *y, *Cb, *Cr;
  uyvy_macropixel u;
  register int i, j;
  boolean chroma_row = TRUE;

  int ipsize = 4, ipsize2;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, subspace);

  // ensure width and height are both divisible by two
  hsize = (hsize >> 1) << 1;
  vsize = (vsize >> 1) << 1;

  y = dest[0];
  Cb = dest[1];
  Cr = dest[2];

  ipsize2 = ipsize * 2;
  hs3 = (hsize * ipsize) - (ipsize2 - 1);

  for (i = 0; i < vsize; i++) {
    for (j = 0; j < hs3; j += ipsize2) {
      // mpeg style, Cb and Cr are co-located
      // convert 6 RGBRGB bytes to 4 UYVY bytes

      // TODO: for mpeg use rgb2yuv and write alternate u and v

      rgb2uyvy(rgbdata[j + 1], rgbdata[j + 2], rgbdata[j + 3], rgbdata[j + 1 + ipsize], rgbdata[j + 2 + ipsize + 1],
               rgbdata[j + 3 + ipsize + 2], &u);

      *(y++) = u.y0;
      *(y++) = u.y1;
      *(Cb++) = u.u0;
      *(Cr++) = u.v0;

      if (!is_422 && chroma_row && i > 0) {
        // average two rows
        Cb[-1 - ostrides[1]] = avg_chroma(Cb[-1], Cb[-1 - ostrides[1]]);
        Cr[-1 - ostrides[2]] = avg_chroma(Cr[-1], Cr[-1 - ostrides[2]]);
      }

    }
    if (!is_422) {
      if (chroma_row) {
        Cb -= ostrides[1];
        Cr -= ostrides[2];
      }
      chroma_row = !chroma_row;
    }
    rgbdata += rowstride;
  }
}


static void convert_bgr_to_yuv420_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride, int *ostrides,
                                        uint8_t **dest, boolean is_422, boolean has_alpha, int subspace, boolean clamped) {
  // for odd sized widths, cut the rightmost pixel
  // TODO - handle different out sampling types
  int hs3;

  uint8_t *y, *Cb, *Cr;
  uyvy_macropixel u;
  register int i, j;
  int chroma_row = TRUE;
  int ipsize = 3, ipsize2;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, subspace);

  if (has_alpha) ipsize = 4;

  // ensure width and height are both divisible by two
  hsize = (hsize >> 1) << 1;
  vsize = (vsize >> 1) << 1;

  y = dest[0];
  Cb = dest[1];
  Cr = dest[2];

  ipsize2 = ipsize * 2;
  hs3 = (hsize * ipsize) - (ipsize2 - 1);
  for (i = 0; i < vsize; i++) {
    for (j = 0; j < hs3; j += ipsize2) {
      // convert 6 RGBRGB bytes to 4 UYVY bytes
      rgb2uyvy(rgbdata[j + 2], rgbdata[j + 1], rgbdata[j], rgbdata[j + ipsize + 2], rgbdata[j + ipsize + 1], rgbdata[j + ipsize], &u);

      *(y++) = u.y0;
      *(y++) = u.y1;
      *(Cb++) = u.u0;
      *(Cr++) = u.v0;

      if (!is_422 && chroma_row && i > 0) {
        // average two rows
        Cb[-1 - ostrides[1]] = avg_chroma(Cb[-1], Cb[-1 - ostrides[1]]);
        Cr[-1 - ostrides[2]] = avg_chroma(Cr[-1], Cr[-1 - ostrides[2]]);
      }
    }
    if (!is_422) {
      if (chroma_row) {
        Cb -= ostrides[1];
        Cr -= ostrides[1];
      }
      chroma_row = !chroma_row;
    }
    rgbdata += rowstride;
  }
}


static void convert_yuv422p_to_uyvy_frame(uint8_t **src, int width, int height, uint8_t *dest) {
  // TODO - handle different in sampling types
  uint8_t *src_y = src[0];
  uint8_t *src_u = src[1];
  uint8_t *src_v = src[2];
  uint8_t *end = src_y + width * height;

  while (src_y < end) {
    *(dest++) = *(src_u++);
    *(dest++) = *(src_y++);
    *(dest++) = *(src_v++);
    *(dest++) = *(src_y++);
  }
}


static void convert_yuv422p_to_yuyv_frame(uint8_t **src, int width, int height, uint8_t *dest) {
  // TODO - handle different in sampling types

  uint8_t *src_y = src[0];
  uint8_t *src_u = src[1];
  uint8_t *src_v = src[2];
  uint8_t *end = src_y + width * height;

  while (src_y < end) {
    *(dest++) = *(src_u++);
    *(dest++) = *(src_y++);
    *(dest++) = *(src_v++);
    *(dest++) = *(src_y++);
  }
}


static void convert_rgb_to_yuv411_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride,
                                        yuv411_macropixel *u, boolean has_alpha, boolean clamped) {
  // for odd sized widths, cut the rightmost one, two or three pixels. Widths should be divisible by 4.
  // TODO - handle different out sampling types
  int hs3 = (int)(hsize >> 2) * 12, ipstep = 12;

  uint8_t *end;
  register int i;

  int x = 3, y = 4, z = 5, a = 6, b = 7, c = 8, d = 9, e = 10, f = 11;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (has_alpha) {
    z++;
    y++;
    x++;
    a += 2;
    b += 2;
    c += 2;
    d += 3;
    e += 3;
    f += 3;
    hs3 = (int)(hsize >> 2) * 16;
    ipstep = 16;
  }
  end = rgbdata + (rowstride * vsize) + 1 - ipstep;
  hs3 -= (ipstep - 1);

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < hs3; i += ipstep) {
      // convert 12 RGBRGBRGBRGB bytes to 6 UYYVYY bytes
      rgb2_411(rgbdata[i], rgbdata[i + 1], rgbdata[i + 2], rgbdata[i + x], rgbdata[i + y], rgbdata[i + z], rgbdata[i + a], rgbdata[i + b],
               rgbdata[i + c], rgbdata[i + d],
               rgbdata[i + e], rgbdata[i + f], u++);
    }
  }
}


static void convert_bgr_to_yuv411_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride,
                                        yuv411_macropixel *u, boolean has_alpha, boolean clamped) {
  // for odd sized widths, cut the rightmost one, two or three pixels
  // TODO - handle different out sampling types
  int hs3 = (int)(hsize >> 2) * 12, ipstep = 12;

  uint8_t *end;
  register int i;

  int x = 3, y = 4, z = 5, a = 6, b = 7, c = 8, d = 9, e = 10, f = 11;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (has_alpha) {
    z++;
    y++;
    x++;
    a += 2;
    b += 2;
    c += 2;
    d += 3;
    e += 3;
    f += 3;
    hs3 = (int)(hsize >> 2) * 16;
    ipstep = 16;
  }
  end = rgbdata + (rowstride * vsize) + 1 - ipstep;
  hs3 -= (ipstep - 1);

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < hs3; i += ipstep) {
      // convert 12 RGBRGBRGBRGB bytes to 6 UYYVYY bytes
      rgb2_411(rgbdata[i + 2], rgbdata[i + 1], rgbdata[i], rgbdata[i + z], rgbdata[i + y], rgbdata[i + x], rgbdata[i + c], rgbdata[i + b],
               rgbdata[i + a], rgbdata[i + f],
               rgbdata[i + e], rgbdata[i + d], u++);
    }
  }
}


static void convert_argb_to_yuv411_frame(uint8_t *rgbdata, int hsize, int vsize, int rowstride,
    yuv411_macropixel *u, boolean clamped) {
  // for odd sized widths, cut the rightmost one, two or three pixels. Widths should be divisible by 4.
  // TODO - handle different out sampling types
  int hs3 = (int)(hsize >> 2) * 12, ipstep = 12;

  uint8_t *end;
  register int i;

  if (LIVES_UNLIKELY(!conv_RY_inited)) init_RGB_to_YUV_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  hs3 = (int)(hsize >> 2) * 16;
  ipstep = 16;

  end = rgbdata + (rowstride * vsize) + 1 - ipstep;
  hs3 -= (ipstep - 1);

  for (; rgbdata < end; rgbdata += rowstride) {
    for (i = 0; i < hs3; i += ipstep) {
      // convert 12 RGBRGBRGBRGB bytes to 6 UYYVYY bytes
      rgb2_411(rgbdata[i + 1], rgbdata[i + 2], rgbdata[i + 3], rgbdata[i + 5], rgbdata[i + 6], rgbdata[i + 7], rgbdata[i + 9], rgbdata[i + 10],
               rgbdata[i + 11],
               rgbdata[i + 13], rgbdata[i + 14], rgbdata[i + 15], u++);
    }
  }
}


static void convert_uyvy_to_rgb_frame(uyvy_macropixel *src, int width, int height, int orowstride,
                                      uint8_t *dest, boolean add_alpha, boolean clamped, int thread_id) {
  register int i, j;
  int psize = 6;
  int a = 3, b = 4, c = 5;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((dheight * i) < height) {
        ccparams[i].src = src + dheight * i * width;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].out_alpha = add_alpha;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_uyvy_to_rgb_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_uyvy_to_rgb_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (add_alpha) {
    psize = 8;
    a = 4;
    b = 5;
    c = 6;
  }

  orowstride -= width * psize;
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      uyvy2rgb(src, &dest[0], &dest[1], &dest[2], &dest[a], &dest[b], &dest[c]);
      if (add_alpha) dest[3] = dest[7] = 255;
      dest += psize;
      src++;
    }
    dest += orowstride;
  }
}


void *convert_uyvy_to_rgb_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_uyvy_to_rgb_frame((uyvy_macropixel *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->orowstrides[0],
                            (uint8_t *)ccparams->dest, ccparams->out_alpha, ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_uyvy_to_bgr_frame(uyvy_macropixel *src, int width, int height, int orowstride,
                                      uint8_t *dest, boolean add_alpha, boolean clamped, int thread_id) {
  register int i, j;
  int psize = 6;

  int a = 3, b = 4, c = 5;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((dheight * i) < height) {
        ccparams[i].src = src + dheight * i * width;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].out_alpha = add_alpha;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_uyvy_to_bgr_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_uyvy_to_bgr_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (add_alpha) {
    psize = 8;
    a = 4;
    b = 5;
    c = 6;
  }

  orowstride -= width * psize;
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      uyvy2rgb(src, &dest[2], &dest[1], &dest[0], &dest[c], &dest[b], &dest[a]);
      if (add_alpha) dest[3] = dest[7] = 255;
      dest += psize;
      src++;
    }
    dest += orowstride;
  }
}


void *convert_uyvy_to_bgr_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_uyvy_to_bgr_frame((uyvy_macropixel *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->orowstrides[0],
                            (uint8_t *)ccparams->dest, ccparams->out_alpha, ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_uyvy_to_argb_frame(uyvy_macropixel *src, int width, int height, int orowstride,
                                       uint8_t *dest, boolean clamped, int thread_id) {
  register int i, j;
  int psize = 8;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((dheight * i) < height) {
        ccparams[i].src = src + dheight * i * width;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_uyvy_to_argb_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_uyvy_to_argb_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  orowstride -= width * psize;
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      uyvy2rgb(src, &dest[1], &dest[2], &dest[3], &dest[5], &dest[6], &dest[7]);
      dest[0] = dest[4] = 255;
      dest += psize;
      src++;
    }
    dest += orowstride;
  }
}


void *convert_uyvy_to_argb_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_uyvy_to_argb_frame((uyvy_macropixel *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->orowstrides[0],
                             (uint8_t *)ccparams->dest, ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuyv_to_rgb_frame(yuyv_macropixel *src, int width, int height, int orowstride,
                                      uint8_t *dest, boolean add_alpha, boolean clamped, int thread_id) {
  register int i, j;
  int psize = 6;
  int a = 3, b = 4, c = 5;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((dheight * i) < height) {
        ccparams[i].src = src + dheight * i * width;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].out_alpha = add_alpha;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuyv_to_rgb_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuyv_to_rgb_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (add_alpha) {
    psize = 8;
    a = 4;
    b = 5;
    c = 6;
  }

  orowstride -= width * psize;
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      yuyv2rgb(src, &dest[0], &dest[1], &dest[2], &dest[a], &dest[b], &dest[c]);
      if (add_alpha) dest[3] = dest[7] = 255;
      dest += psize;
      src++;
    }
    dest += orowstride;
  }
}


void *convert_yuyv_to_rgb_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuyv_to_rgb_frame((yuyv_macropixel *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->orowstrides[0],
                            (uint8_t *)ccparams->dest, ccparams->out_alpha, ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuyv_to_bgr_frame(yuyv_macropixel *src, int width, int height, int orowstride,
                                      uint8_t *dest, boolean add_alpha, boolean clamped, int thread_id) {
  register int i, j;
  int psize = 6;
  int a = 3, b = 4, c = 5;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((dheight * i) < height) {
        ccparams[i].src = src + dheight * i * width;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].out_alpha = add_alpha;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuyv_to_bgr_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuyv_to_bgr_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (add_alpha) {
    psize = 8;
    a = 4;
    b = 5;
    c = 6;
  }

  orowstride -= width * psize;
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      yuyv2rgb(src, &dest[2], &dest[1], &dest[0], &dest[c], &dest[b], &dest[a]);
      if (add_alpha) dest[3] = dest[7] = 255;
      dest += psize;
      src++;
    }
    dest += orowstride;
  }
}


void *convert_yuyv_to_bgr_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuyv_to_bgr_frame((yuyv_macropixel *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->orowstrides[0],
                            (uint8_t *)ccparams->dest, ccparams->out_alpha, ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuyv_to_argb_frame(yuyv_macropixel *src, int width, int height, int orowstride,
                                       uint8_t *dest, boolean clamped, int thread_id) {
  register int i, j;
  int psize = 8;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((dheight * i) < height) {
        ccparams[i].src = src + dheight * i * width;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].in_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuyv_to_argb_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuyv_to_argb_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  orowstride -= width * psize;
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      yuyv2rgb(src, &dest[1], &dest[2], &dest[3], &dest[5], &dest[6], &dest[7]);
      dest[0] = dest[4] = 255;
      dest += psize;
      src++;
    }
    dest += orowstride;
  }
}


void *convert_yuyv_to_argb_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuyv_to_argb_frame((yuyv_macropixel *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->orowstrides[0],
                             (uint8_t *)ccparams->dest, ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuv420_to_uyvy_frame(uint8_t **src, int width, int height, uyvy_macropixel *dest, boolean clamped) {
  register int i = 0, j;
  uint8_t *y, *u, *v, *end;
  int hwidth = width >> 1;
  boolean chroma = TRUE;

  // TODO - handle different in sampling types
  if (!avg_inited) init_average();

  y = src[0];
  u = src[1];
  v = src[2];

  end = y + width * height;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  while (y < end) {
    for (j = 0; j < hwidth; j++) {
      dest->u0 = u[0];
      dest->y0 = y[0];
      dest->v0 = v[0];
      dest->y1 = y[1];

      if (chroma && i > 0) {
        dest[-hwidth].u0 = avg_chroma(dest[-hwidth].u0, u[0]);
        dest[-hwidth].v0 = avg_chroma(dest[-hwidth].v0, v[0]);
      }

      dest++;
      y += 2;
      u++;
      v++;
    }
    if (chroma) {
      u -= hwidth;
      v -= hwidth;
    }
    chroma = !chroma;
    i++;
  }
}


static void convert_yuv420_to_yuyv_frame(uint8_t **src, int width, int height, yuyv_macropixel *dest, boolean clamped) {
  register int i = 0, j;
  uint8_t *y, *u, *v, *end;
  int hwidth = width >> 1;
  boolean chroma = TRUE;

  // TODO - handle different in sampling types
  if (!avg_inited) init_average();

  y = src[0];
  u = src[1];
  v = src[2];

  end = y + width * height;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  while (y < end) {
    for (j = 0; j < hwidth; j++) {
      dest->y0 = y[0];
      dest->u0 = u[0];
      dest->y1 = y[1];
      dest->v0 = v[0];

      if (chroma && i > 0) {
        dest[-hwidth].u0 = avg_chroma(dest[-hwidth].u0, u[0]);
        dest[-hwidth].v0 = avg_chroma(dest[-hwidth].v0, v[0]);
      }

      dest++;
      y += 2;
      u++;
      v++;
    }
    if (chroma) {
      u -= hwidth;
      v -= hwidth;
    }
    chroma = !chroma;
    i++;
  }
}


static void convert_yuv_planar_to_rgb_frame(uint8_t **src, int width, int height, int irowstride, int orowstride, uint8_t *dest,
    boolean in_alpha, boolean out_alpha, boolean clamped, int thread_id) {
  uint8_t *y = src[0];
  uint8_t *u = src[1];
  uint8_t *v = src[2];
  uint8_t *a = NULL;

  uint8_t *end = y + irowstride * height;

  size_t opstep = 3;
  register int i, j;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (in_alpha) a = src[3];

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((y + dheight * i * width) < end) {
        ccparams[i].hsize = width;

        ccparams[i].srcp[0] = y + dheight * i * irowstride;
        ccparams[i].srcp[1] = u + dheight * i * irowstride;
        ccparams[i].srcp[2] = v + dheight * i * irowstride;
        if (in_alpha) ccparams[i].srcp[3] = a + dheight * i * irowstride;

        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].in_alpha = in_alpha;
        ccparams[i].out_alpha = out_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuv_planar_to_rgb_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuv_planar_to_rgb_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  if (out_alpha) opstep = 4;

  orowstride -= width * opstep;
  irowstride -= width;

  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      yuv2rgb(*(y++), *(u++), *(v++), &dest[0], &dest[1], &dest[2]);
      if (out_alpha) {
        if (in_alpha) {
          dest[3] = *(a++);
        } else dest[3] = 255;
      }
      dest += opstep;
    }
    dest += orowstride;
    y += irowstride;
    u += irowstride;
    v += irowstride;
    if (a != NULL) a += irowstride;
  }
}


void *convert_yuv_planar_to_rgb_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuv_planar_to_rgb_frame((uint8_t **)ccparams->srcp, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                                  ccparams->orowstrides[0],
                                  (uint8_t *)ccparams->dest, ccparams->in_alpha, ccparams->out_alpha,
                                  ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuv_planar_to_bgr_frame(uint8_t **src, int width, int height, int irowstride, int orowstride, uint8_t *dest,
    boolean in_alpha, boolean out_alpha, boolean clamped, int thread_id) {
  uint8_t *y = src[0];
  uint8_t *u = src[1];
  uint8_t *v = src[2];
  uint8_t *a = NULL;

  uint8_t *end = y + irowstride * height;

  size_t opstep = 4;
  register int i, j;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (in_alpha) a = src[3];

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((y + dheight * i * orowstride) < end) {
        ccparams[i].hsize = width;

        ccparams[i].srcp[0] = y + dheight * i * irowstride;
        ccparams[i].srcp[1] = u + dheight * i * irowstride;
        ccparams[i].srcp[2] = v + dheight * i * irowstride;
        if (in_alpha) ccparams[i].srcp[3] = a + dheight * i * irowstride;

        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].in_alpha = in_alpha;
        ccparams[i].out_alpha = out_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuv_planar_to_bgr_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuv_planar_to_bgr_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

#if !USE_THREADS
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);
#endif

  orowstride -= width * opstep;
  irowstride -= width;

  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      yuv2rgb(*(y++), *(u++), *(v++), &dest[2], &dest[1], &dest[0]);
      if (out_alpha) {
        if (in_alpha) {
          dest[3] = *(a++);
        } else dest[3] = 255;
      }
      dest += opstep;
    }
    dest += orowstride;
    y += irowstride;
    u += irowstride;
    v += irowstride;
    if (a != NULL) a += irowstride;
  }
}


void *convert_yuv_planar_to_bgr_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuv_planar_to_bgr_frame((uint8_t **)ccparams->srcp, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                                  ccparams->orowstrides[0],
                                  (uint8_t *)ccparams->dest, ccparams->in_alpha, ccparams->out_alpha,
                                  ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuv_planar_to_argb_frame(uint8_t **src, int width, int height, int irowstride, int orowstride, uint8_t *dest,
    boolean in_alpha, boolean clamped, int thread_id) {
  uint8_t *y = src[0];
  uint8_t *u = src[1];
  uint8_t *v = src[2];
  uint8_t *a = NULL;

  uint8_t *end = y + irowstride * height;

  size_t opstep = 4;
  register int i, j;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  if (in_alpha) a = src[3];

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((y + dheight * i * width) < end) {
        ccparams[i].hsize = width;

        ccparams[i].srcp[0] = y + dheight * i * irowstride;
        ccparams[i].srcp[1] = u + dheight * i * irowstride;
        ccparams[i].srcp[2] = v + dheight * i * irowstride;
        if (in_alpha) ccparams[i].srcp[3] = a + dheight * i * irowstride;

        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].in_alpha = in_alpha;
        ccparams[i].out_clamped = clamped;
        ccparams[i].thread_id = i;

        if (i == 0) convert_yuv_planar_to_argb_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_yuv_planar_to_argb_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  orowstride -= width * opstep;
  orowstride -= width;

  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      yuv2rgb(*(y++), *(u++), *(v++), &dest[1], &dest[2], &dest[3]);
      if (in_alpha) {
        dest[0] = *(a++);
      } else dest[0] = 255;
      dest += opstep;
    }
    dest += orowstride;
    y += irowstride;
    u += irowstride;
    v += irowstride;
    if (a != NULL) a += irowstride;
  }
}


void *convert_yuv_planar_to_argb_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_yuv_planar_to_argb_frame((uint8_t **)ccparams->srcp, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                                   ccparams->orowstrides[0],
                                   (uint8_t *)ccparams->dest, ccparams->in_alpha, ccparams->in_clamped, ccparams->thread_id);
  return NULL;
}


static void convert_yuv_planar_to_uyvy_frame(uint8_t **src, int width, int height, int irowstride,
    uyvy_macropixel *uyvy, boolean clamped) {
  register int x, k;
  int size = (width * height) >> 1;

  uint8_t *y = src[0];
  uint8_t *u = src[1];
  uint8_t *v = src[2];

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (irowstride == width) {
    for (x = 0; x < size; x++) {
      // subsample two u pixels
      uyvy->u0 = avg_chroma(u[0], u[1]);
      u += 2;
      uyvy->y0 = *(y++);
      // subsample 2 v pixels
      uyvy->v0 = avg_chroma(v[0], v[1]);
      v += 2;
      uyvy->y1 = *(y++);
      uyvy++;
    }
    return;
  }
  irowstride -= width;
  for (k = 0; k < height; k++) {
    for (x = 0; x < width; x++) {
      // subsample two u pixels
      uyvy->u0 = avg_chroma(u[0], u[1]);
      u += 2;
      uyvy->y0 = *(y++);
      // subsample 2 v pixels
      uyvy->v0 = avg_chroma(v[0], v[1]);
      v += 2;
      uyvy->y1 = *(y++);
      uyvy++;
    }
    y += irowstride;
    u += irowstride;
    v += irowstride;
  }
}

static void convert_yuv_planar_to_yuyv_frame(uint8_t **src, int width, int height, int irowstride,
    yuyv_macropixel *yuyv, boolean clamped) {
  register int x, k;
  int hsize = (width * height) >> 1;

  uint8_t *y = src[0];
  uint8_t *u = src[1];
  uint8_t *v = src[2];

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (irowstride == width) {
    for (x = 0; x < hsize; x++) {
      yuyv->y0 = *(y++);
      yuyv->u0 = avg_chroma(u[0], u[1]);
      u += 2;
      yuyv->y1 = *(y++);
      yuyv->v0 = avg_chroma(v[0], v[1]);
      v += 2;
      yuyv++;
    }
    return;
  }

  irowstride -= width;
  for (k = 0; k < height; k++) {
    for (x = 0; x < width; x++) {
      yuyv->y0 = *(y++);
      yuyv->u0 = avg_chroma(u[0], u[1]);
      u += 2;
      yuyv->y1 = *(y++);
      yuyv->v0 = avg_chroma(v[0], v[1]);
      v += 2;
      yuyv++;
    }
    y += irowstride;
    u += irowstride;
    v += irowstride;
  }
}

static void convert_combineplanes_frame(uint8_t **src, int width, int height, int irowstride,
                                        int orowstride, uint8_t *dest, boolean in_alpha, boolean out_alpha) {
  // turn 3 or 4 planes into packed pixels, src and dest can have alpha

  // e.g yuv444(4)p to yuv888(8)

  int size = width * height;

  uint8_t *y = src[0];
  uint8_t *u = src[1];
  uint8_t *v = src[2];
  uint8_t *a = NULL;

  register int x, k;

  if (in_alpha) a = src[3];

  if (irowstride == width && orowstride == width) {
    for (x = 0; x < size; x++) {
      *(dest++) = *(y++);
      *(dest++) = *(u++);
      *(dest++) = *(v++);
      if (out_alpha) {
        if (in_alpha) *(dest++) = *(a++);
        else *(dest++) = 255;
      }
    }
  } else {
    irowstride -= width;
    orowstride -= width;
    for (k = 0; k < height; k++) {
      for (x = 0; x < width; x++) {
        *(dest++) = *(y++);
        *(dest++) = *(u++);
        *(dest++) = *(v++);
        if (out_alpha) {
          if (in_alpha) *(dest++) = *(a++);
          else *(dest++) = 255;
        }
      }
      dest += orowstride;
      y += irowstride;
      u += irowstride;
      v += irowstride;
    }
  }
}


static void convert_yuvap_to_yuvp_frame(uint8_t **src, int width, int height, int irowstride, int orowstride, uint8_t **dest) {
  size_t size = irowstride * height;

  uint8_t *ys = src[0];
  uint8_t *us = src[1];
  uint8_t *vs = src[2];

  uint8_t *yd = dest[0];
  uint8_t *ud = dest[1];
  uint8_t *vd = dest[2];

  register int y;

  if (orowstride == irowstride) {
    if (yd != ys) lives_memcpy(yd, ys, size);
    if (ud != us) lives_memcpy(ud, us, size);
    if (vd != vs) lives_memcpy(vd, vs, size);
    return;
  }
  for (y = 0; y < height; y++) {
    if (yd != ys) {
      lives_memcpy(yd, ys, width);
      yd += orowstride;
      ys += irowstride;
    }
    if (ud != us) {
      lives_memcpy(ud, us, width);
      ud += orowstride;
      us += irowstride;
    }
    if (vd != vs) {
      lives_memcpy(vd, vs, width);
      vd += orowstride;
      vs += irowstride;
    }
  }
}


static void convert_yuvp_to_yuvap_frame(uint8_t **src, int width, int height, int irowstride, int orowstride,  uint8_t **dest) {
  convert_yuvap_to_yuvp_frame(src, width, height, irowstride, orowstride, dest);
  lives_memset(dest[3], 255, orowstride * height);
}


static void convert_yuvp_to_yuv420_frame(uint8_t **src, int width, int height, int *irows, int *orows, uint8_t **dest, boolean clamped) {
  // halve the chroma samples vertically and horizontally, with sub-sampling

  // convert 444p to 420p

  // TODO - handle different output sampling types

  // y-plane should be copied before entering here

  register int i, j;
  uint8_t *d_u, *d_v, *s_u = src[1], *s_v = src[2];
  register short x_u, x_v;
  boolean chroma = FALSE;

  int hwidth = width >> 1;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (dest[0] != src[0]) lives_memcpy(dest[0], src[0], width * height);

  d_u = dest[1];
  d_v = dest[2];

  for (i = 0; i < height; i++) {
    for (j = 0; j < hwidth; j++) {
      if (!chroma) {
        // pass 1, copy row
        // average two dest pixels
        d_u[j] = avg_chroma(s_u[j * 2], s_u[j * 2 + 1]);
        d_v[j] = avg_chroma(s_v[j * 2], s_v[j * 2 + 1]);
      } else {
        // pass 2
        // average two dest pixels
        x_u = avg_chroma(s_u[j * 2], s_u[j * 2 + 1]);
        x_v = avg_chroma(s_v[j * 2], s_v[j * 2 + 1]);
        // average two dest rows
        d_u[j] = avg_chroma(d_u[j], x_u);
        d_v[j] = avg_chroma(d_v[j], x_v);
      }
    }
    if (chroma) {
      d_u += orows[1];
      d_v += orows[2];
    }
    chroma = !chroma;
    s_u += irows[1];
    s_v += irows[2];
  }
}


static void convert_yuvp_to_yuv411_frame(uint8_t **src, int width, int height, int irowstride,
    yuv411_macropixel *yuv, boolean clamped) {
  // quarter the chroma samples horizontally, with sub-sampling

  // convert 444p to 411 packed
  // TODO - handle different output sampling types

  register int i, j;
  uint8_t *s_y = src[0], *s_u = src[1], *s_v = src[2];
  register short x_u, x_v;

  int widtha = (width >> 1) << 1; // cut rightmost odd bytes
  int cbytes = width - widtha;

  irowstride -= width + cbytes;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  for (i = 0; i < height; i++) {
    for (j = 0; j < widtha; j += 4) {
      // average four dest pixels
      yuv->u2 = avg_chroma(s_u[0], s_u[1]);
      x_u = avg_chroma(s_u[2], s_u[3]);
      yuv->u2 = avg_chroma(yuv->u2, x_u);

      s_u += 4;

      yuv->y0 = *(s_y++);
      yuv->y1 = *(s_y++);

      yuv->v2 = avg_chroma(s_v[0], s_v[1]);
      x_v = avg_chroma(s_v[2], s_v[3]);
      yuv->v2 = avg_chroma(yuv->v2, x_v);

      s_v += 4;

      yuv->y2 = *(s_y++);
      yuv->y3 = *(s_y++);
    }
    s_u += irowstride;
    s_v += irowstride;
  }
}


static void convert_uyvy_to_yuvp_frame(uyvy_macropixel *uyvy, int width, int height, int orow, uint8_t **dest, boolean add_alpha) {
  // TODO - avg_chroma

  int size = width * height;
  register int x, k;

  uint8_t *y = dest[0];
  uint8_t *u = dest[1];
  uint8_t *v = dest[2];

  if (orow == width) {
    for (x = 0; x < size; x++) {
      *(u++) = uyvy->u0;
      *(u++) = uyvy->u0;
      *(y++) = uyvy->y0;
      *(v++) = uyvy->v0;
      *(v++) = uyvy->v0;
      *(y++) = uyvy->y1;
      uyvy++;
    }
  } else {
    orow -= width;
    for (k = 0; k < height; k++) {
      for (x = 0; x < width; x++) {
        *(u++) = uyvy->u0;
        *(u++) = uyvy->u0;
        *(y++) = uyvy->y0;
        *(v++) = uyvy->v0;
        *(v++) = uyvy->v0;
        *(y++) = uyvy->y1;
        uyvy++;
      }
      y += orow;
      u += orow;
      v += orow;
    }
  }
  if (add_alpha) lives_memset(dest[3], 255, size * 2);
}


static void convert_yuyv_to_yuvp_frame(yuyv_macropixel *yuyv, int width, int height, uint8_t **dest, boolean add_alpha) {
  // TODO - subsampling

  int size = width * height;
  register int x;

  uint8_t *y = dest[0];
  uint8_t *u = dest[1];
  uint8_t *v = dest[2];

  for (x = 0; x < size; x++) {
    *(y++) = yuyv->y0;
    *(u++) = yuyv->u0;
    *(u++) = yuyv->u0;
    *(y++) = yuyv->y1;
    *(v++) = yuyv->v0;
    *(v++) = yuyv->v0;
    yuyv++;
  }
  if (x);
  if (add_alpha) lives_memset(dest[3], 255, size * 2);
}


static void convert_uyvy_to_yuv888_frame(uyvy_macropixel *uyvy, int width, int height, uint8_t *yuv, boolean add_alpha) {
  int size = width * height;
  register int x;

  // double chroma horizontally, no subsampling
  // no subsampling : TODO

  for (x = 0; x < size; x++) {
    *(yuv++) = uyvy->y0;
    *(yuv++) = uyvy->u0;
    *(yuv++) = uyvy->v0;
    if (add_alpha) *(yuv++) = 255;
    *(yuv++) = uyvy->y1;
    *(yuv++) = uyvy->u0;
    *(yuv++) = uyvy->v0;
    if (add_alpha) *(yuv++) = 255;
    uyvy++;
  }
}


static void convert_yuyv_to_yuv888_frame(yuyv_macropixel *yuyv, int width, int height, uint8_t *yuv, boolean add_alpha) {
  int size = width * height;
  register int x;

  // no subsampling : TODO

  for (x = 0; x < size; x++) {
    *(yuv++) = yuyv->y0;
    *(yuv++) = yuyv->u0;
    *(yuv++) = yuyv->v0;
    if (add_alpha) *(yuv++) = 255;
    *(yuv++) = yuyv->y1;
    *(yuv++) = yuyv->u0;
    *(yuv++) = yuyv->v0;
    if (add_alpha) *(yuv++) = 255;
    yuyv++;
  }
}


static void convert_uyvy_to_yuv420_frame(uyvy_macropixel *uyvy, int width, int height, uint8_t **yuv, boolean clamped) {
  // subsample vertically

  // TODO - handle different sampling types

  register int j;

  uint8_t *y = yuv[0];
  uint8_t *u = yuv[1];
  uint8_t *v = yuv[2];

  boolean chroma = TRUE;

  uint8_t *end = y + width * height * 2;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  while (y < end) {
    for (j = 0; j < width; j++) {
      if (chroma) *(u++) = uyvy->u0;
      else {
        *u = avg_chroma(*u, uyvy->u0);
        u++;
      }
      *(y++) = uyvy->y0;
      if (chroma) *(v++) = uyvy->v0;
      else {
        *v = avg_chroma(*v, uyvy->v0);
        v++;
      }
      *(y++) = uyvy->y1;
      uyvy++;
    }
    if (chroma) {
      u -= width;
      v -= width;
    }
    chroma = !chroma;
  }
}


static void convert_yuyv_to_yuv420_frame(yuyv_macropixel *yuyv, int width, int height, uint8_t **yuv, boolean clamped) {
  // subsample vertically

  // TODO - handle different sampling types

  register int j;

  uint8_t *y = yuv[0];
  uint8_t *u = yuv[1];
  uint8_t *v = yuv[2];

  boolean chroma = TRUE;

  uint8_t *end = y + width * height * 2;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  while (y < end) {
    for (j = 0; j < width; j++) {
      *(y++) = yuyv->y0;
      if (chroma) *(u++) = yuyv->u0;
      else {
        *u = avg_chroma(*u, yuyv->u0);
        u++;
      }
      *(y++) = yuyv->y1;
      if (chroma) *(v++) = yuyv->v0;
      else {
        *v = avg_chroma(*v, yuyv->v0);
        v++;
      }
      yuyv++;
    }
    if (chroma) {
      u -= width;
      v -= width;
    }
    chroma = !chroma;
  }
}


static void convert_uyvy_to_yuv411_frame(uyvy_macropixel *uyvy, int width, int height, yuv411_macropixel *yuv,
    boolean clamped) {
  // subsample chroma horizontally

  uyvy_macropixel *end = uyvy + width * height;
  register int x;

  int widtha = (width << 1) >> 1;
  size_t cbytes = width - widtha;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  for (; uyvy < end; uyvy += cbytes) {
    for (x = 0; x < widtha; x += 2) {
      yuv->u2 = avg_chroma(uyvy[0].u0, uyvy[1].u0);

      yuv->y0 = uyvy[0].y0;
      yuv->y1 = uyvy[0].y1;

      yuv->v2 = avg_chroma(uyvy[0].v0, uyvy[1].v0);

      yuv->y2 = uyvy[1].y0;
      yuv->y3 = uyvy[1].y1;

      uyvy += 2;
      yuv++;
    }
  }
}


static void convert_yuyv_to_yuv411_frame(yuyv_macropixel *yuyv, int width, int height, yuv411_macropixel *yuv,
    boolean clamped) {
  // subsample chroma horizontally

  // TODO - handle different sampling types

  yuyv_macropixel *end = yuyv + width * height;
  register int x;

  int widtha = (width << 1) >> 1;
  size_t cybtes = width - widtha;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  for (; yuyv < end; yuyv += cybtes) {
    for (x = 0; x < widtha; x += 2) {
      yuv->u2 = avg_chroma(yuyv[0].u0, yuyv[1].u0);

      yuv->y0 = yuyv[0].y0;
      yuv->y1 = yuyv[0].y1;

      yuv->v2 = avg_chroma(yuyv[0].v0, yuyv[1].v0);

      yuv->y2 = yuyv[1].y0;
      yuv->y3 = yuyv[1].y1;

      yuyv += 2;
      yuv++;
    }
  }
}


static void convert_yuv888_to_yuv420_frame(uint8_t *yuv8, int width, int height, int irowstride, int *orows,
    uint8_t **yuv4, boolean src_alpha, boolean clamped) {
  // subsample vertically and horizontally

  //

  // yuv888(8) packed to 420p

  // TODO - handle different sampling types

  // TESTED !

  register int j;
  register short x_u, x_v;

  uint8_t *d_y, *d_u, *d_v, *end;

  boolean chroma = TRUE;

  size_t ipsize = 3, ipsize2;
  int widthx;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (src_alpha) ipsize = 4;

  d_y = yuv4[0];
  d_u = yuv4[1];
  d_v = yuv4[2];

  end = d_y + width * height;
  ipsize2 = ipsize * 2;
  widthx = width * ipsize;

  while (d_y < end) {
    for (j = 0; j < widthx; j += ipsize2) {
      *(d_y++) = yuv8[j];
      *(d_y++) = yuv8[j + ipsize];
      if (chroma) {
        *(d_u++) = avg_chroma(yuv8[j + 1], yuv8[j + 1 + ipsize]);
        *(d_v++) = avg_chroma(yuv8[j + 2], yuv8[j + 2 + ipsize]);
      } else {
        x_u = avg_chroma(yuv8[j + 1], yuv8[j + 1 + ipsize]);
        *d_u = avg_chroma(*d_u, x_u);
        d_u++;
        x_v = avg_chroma(yuv8[j + 2], yuv8[j + 2 + ipsize]);
        *d_v = avg_chroma(*d_v, x_v);
        d_v++;
      }
    }
    if (chroma) {
      d_u -= orows[1];
      d_v -= orows[2];
    }
    chroma = !chroma;
    yuv8 += irowstride;
  }
}


static void convert_uyvy_to_yuv422_frame(uyvy_macropixel *uyvy, int width, int height, uint8_t **yuv) {
  int size = width * height; // y is twice this, u and v are equal

  uint8_t *y = yuv[0];
  uint8_t *u = yuv[1];
  uint8_t *v = yuv[2];

  register int x;

  for (x = 0; x < size; x++) {
    uyvy_2_yuv422(uyvy, y, u, v, y + 1);
    y += 2;
    u++;
    v++;
  }
}


static void convert_yuyv_to_yuv422_frame(yuyv_macropixel *yuyv, int width, int height, uint8_t **yuv) {
  int size = width * height; // y is twice this, u and v are equal

  uint8_t *y = yuv[0];
  uint8_t *u = yuv[1];
  uint8_t *v = yuv[2];

  register int x;

  for (x = 0; x < size; x++) {
    yuyv_2_yuv422(yuyv, y, u, v, y + 1);
    y += 2;
    u++;
    v++;
  }
}


static void convert_yuv888_to_yuv422_frame(uint8_t *yuv8, int width, int height, int irowstride, int *ostrides,
    uint8_t **yuv4, boolean has_alpha, boolean clamped) {
  // 888(8) packed to 422p

  // TODO - handle different sampling types

  int size = width * height; // y is equal this, u and v are half, chroma subsampled horizontally

  uint8_t *y = yuv4[0];
  uint8_t *u = yuv4[1];
  uint8_t *v = yuv4[2];

  register int x, i, j;

  int offs = 0;
  size_t ipsize;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (has_alpha) offs = 1;

  ipsize = (3 + offs) << 1;

  if ((irowstride << 1) == width * ipsize && ostrides[0] == width && ostrides[1] == (width >> 1)) {
    for (x = 0; x < size; x += 2) {
      *(y++) = yuv8[0];
      *(y++) = yuv8[3 + offs];
      *(u++) = avg_chroma(yuv8[1], yuv8[4 + offs]);
      *(v++) = avg_chroma(yuv8[2], yuv8[5 + offs]);
      yuv8 += ipsize;
    }
  } else {
    width >>= 1;
    irowstride -= width * ipsize;
    ostrides[0] -= width;
    ostrides[1] -= width >> 1;
    ostrides[2] -= width >> 1;

    for (i = 0; i < height; i++) {
      for (j = 0; j < width; j++) {
        *(y++) = yuv8[0];
        *(y++) = yuv8[3 + offs];
        *(u++) = avg_chroma(yuv8[1], yuv8[4 + offs]);
        *(v++) = avg_chroma(yuv8[2], yuv8[5 + offs]);
        yuv8 += ipsize;
      }
      yuv8 += irowstride;
      y += ostrides[0];
      u += ostrides[1];
      v += ostrides[2];
    }
  }
}


static void convert_yuv888_to_uyvy_frame(uint8_t *yuv, int width, int height, int irowstride,
    uyvy_macropixel *uyvy, boolean has_alpha, boolean clamped) {
  int size = width * height;

  register int x, i, j;

  int offs = 0;
  size_t ipsize;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (has_alpha) offs = 1;

  ipsize = (3 + offs) << 1;

  if ((irowstride << 1) == width * ipsize) {
    for (x = 0; x < size; x += 2) {
      uyvy->u0 = avg_chroma(yuv[1], yuv[4 + offs]);
      uyvy->y0 = yuv[0];
      uyvy->v0 = avg_chroma(yuv[2], yuv[5 + offs]);
      uyvy->y1 = yuv[3 + offs];
      yuv += ipsize;
      uyvy++;
    }
  } else {
    width >>= 1;
    irowstride -= width * ipsize;
    for (i = 0; i < height; i++) {
      for (j = 0; j < width; j++) {
        uyvy->u0 = avg_chroma(yuv[1], yuv[4 + offs]);
        uyvy->y0 = yuv[0];
        uyvy->v0 = avg_chroma(yuv[2], yuv[5 + offs]);
        uyvy->y1 = yuv[3 + offs];
        yuv += ipsize;
        uyvy++;
      }
      yuv += irowstride;
    }
  }
}


static void convert_yuv888_to_yuyv_frame(uint8_t *yuv, int width, int height, int irowstride,
    yuyv_macropixel *yuyv, boolean has_alpha, boolean clamped) {
  int size = width * height;

  register int x, i, j;

  int offs = 0;
  size_t ipsize;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (has_alpha) offs = 1;

  ipsize = (3 + offs) << 1;

  if (irowstride << 1 == width * ipsize) {
    for (x = 0; x < size; x += 2) {
      yuyv->y0 = yuv[0];
      yuyv->u0 = avg_chroma(yuv[1], yuv[4 + offs]);
      yuyv->y1 = yuv[3 + offs];
      yuyv->v0 = avg_chroma(yuv[2], yuv[5 + offs]);
      yuv += ipsize;
      yuyv++;
    }
  } else {
    width >>= 1;
    irowstride -= width * ipsize;
    for (i = 0; i < height; i++) {
      for (j = 0; j < width; j++) {
        yuyv->y0 = yuv[0];
        yuyv->u0 = avg_chroma(yuv[1], yuv[4 + offs]);
        yuyv->y1 = yuv[3 + offs];
        yuyv->v0 = avg_chroma(yuv[2], yuv[5 + offs]);
        yuv += ipsize;
        yuyv++;
      }
      yuv += irowstride;
    }
  }
}


static void convert_yuv888_to_yuv411_frame(uint8_t *yuv8, int width, int height, int irowstride,
    yuv411_macropixel *yuv411, boolean has_alpha) {
  // yuv 888(8) packed to yuv411. Chroma pixels are averaged.

  // TODO - handle different sampling types

  uint8_t *end = yuv8 + width * height;
  register int x;
  size_t ipsize = 3;
  int widtha = (width >> 1) << 1; // cut rightmost odd bytes
  int cbytes = width - widtha;

  if (has_alpha) ipsize = 4;

  irowstride -= widtha * ipsize;

  for (; yuv8 < end; yuv8 += cbytes) {
    for (x = 0; x < widtha; x += 4) { // process 4 input pixels for one output macropixel
      yuv411->u2 = (yuv8[1] + yuv8[ipsize + 1] + yuv8[2 * ipsize + 1] + yuv8[3 * ipsize + 1]) >> 2;
      yuv411->y0 = yuv8[0];
      yuv411->y1 = yuv8[ipsize];
      yuv411->v2 = (yuv8[2] + yuv8[ipsize + 2] + yuv8[2 * ipsize + 2] + yuv8[3 * ipsize + 2]) >> 2;
      yuv411->y2 = yuv8[ipsize * 2];
      yuv411->y3 = yuv8[ipsize * 3];

      yuv411++;
      yuv8 += ipsize * 4;
    }
    yuv8 += irowstride;
  }
}


static void convert_yuv411_to_rgb_frame(yuv411_macropixel *yuv411, int width, int height, int orowstride,
                                        uint8_t *dest, boolean add_alpha, boolean clamped) {
  uyvy_macropixel uyvy;
  int m = 3, n = 4, o = 5;
  uint8_t u, v, h_u, h_v, q_u, q_v, y0, y1;
  register int j;
  yuv411_macropixel *end = yuv411 + width * height;
  size_t psize = 3, psize2;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (add_alpha) {
    m = 4;
    n = 5;
    o = 6;
    psize = 4;
  }

  orowstride -= width * 4 * psize;
  psize2 = psize << 1;

  while (yuv411 < end) {
    // write 2 RGB pixels
    if (add_alpha) dest[3] = dest[7] = 255;

    uyvy.y0 = yuv411[0].y0;
    uyvy.y1 = yuv411[0].y1;
    uyvy.u0 = yuv411[0].u2;
    uyvy.v0 = yuv411[0].v2;
    uyvy2rgb(&uyvy, &dest[0], &(dest[1]), &dest[2], &dest[m], &dest[n], &dest[o]);
    dest += psize2;

    for (j = 1; j < width; j++) {
      // convert 6 yuv411 bytes to 4 rgb(a) pixels

      // average first 2 RGB pixels of this block and last 2 RGB pixels of previous block

      y0 = yuv411[j - 1].y2;
      y1 = yuv411[j - 1].y3;

      h_u = avg_chroma(yuv411[j - 1].u2, yuv411[j].u2);
      h_v = avg_chroma(yuv411[j - 1].v2, yuv411[j].v2);

      // now we have 1/2, 1/2

      // average last pixel again to get 1/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j - 1].u2);
      q_v = avg_chroma(h_v, yuv411[j - 1].v2);

      // average again to get 1/8, 3/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      yuv2rgb(y0, u, v, &dest[0], &dest[1], &dest[2]);

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      yuv2rgb(y1, u, v, &dest[m], &dest[n], &dest[o]);

      dest += psize2;

      // set first 2 RGB pixels of this block

      y0 = yuv411[j].y0;
      y1 = yuv411[j].y1;

      // avg to get 3/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j].u2);
      q_v = avg_chroma(h_v, yuv411[j].v2);

      // average again to get 5/8, 7/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      yuv2rgb(y0, u, v, &dest[0], &dest[1], &dest[2]);

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      yuv2rgb(y1, u, v, &dest[m], &dest[n], &dest[o]);

      if (add_alpha) dest[3] = dest[7] = 255;
      dest += psize2;

    }
    // write last 2 pixels

    if (add_alpha) dest[3] = dest[7] = 255;

    uyvy.y0 = yuv411[j - 1].y2;
    uyvy.y1 = yuv411[j - 1].y3;
    uyvy.u0 = yuv411[j - 1].u2;
    uyvy.v0 = yuv411[j - 1].v2;
    uyvy2rgb(&uyvy, &dest[0], &(dest[1]), &dest[2], &dest[m], &dest[n], &dest[o]);

    dest += psize2 + orowstride;
    yuv411 += width;
  }
}


static void convert_yuv411_to_bgr_frame(yuv411_macropixel *yuv411, int width, int height, int orowstride,
                                        uint8_t *dest, boolean add_alpha, boolean clamped) {
  uyvy_macropixel uyvy;
  int m = 3, n = 4, o = 5;
  uint8_t u, v, h_u, h_v, q_u, q_v, y0, y1;
  register int j;
  yuv411_macropixel *end = yuv411 + width * height;
  size_t psize = 3, psize2;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (add_alpha) {
    m = 4;
    n = 5;
    o = 6;
    psize = 4;
  }

  orowstride -= width * 4 * psize;

  psize2 = psize << 1;

  while (yuv411 < end) {
    // write 2 RGB pixels
    if (add_alpha) dest[3] = dest[7] = 255;

    uyvy.y0 = yuv411[0].y0;
    uyvy.y1 = yuv411[0].y1;
    uyvy.u0 = yuv411[0].u2;
    uyvy.v0 = yuv411[0].v2;
    uyvy2rgb(&uyvy, &dest[0], &(dest[1]), &dest[2], &dest[o], &dest[n], &dest[m]);
    dest += psize2;

    for (j = 1; j < width; j++) {
      // convert 6 yuv411 bytes to 4 rgb(a) pixels

      // average first 2 RGB pixels of this block and last 2 RGB pixels of previous block

      y0 = yuv411[j - 1].y2;
      y1 = yuv411[j - 1].y3;

      h_u = avg_chroma(yuv411[j - 1].u2, yuv411[j].u2);
      h_v = avg_chroma(yuv411[j - 1].v2, yuv411[j].v2);

      // now we have 1/2, 1/2

      // average last pixel again to get 1/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j - 1].u2);
      q_v = avg_chroma(h_v, yuv411[j - 1].v2);

      // average again to get 1/8, 3/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      yuv2rgb(y0, u, v, &dest[0], &dest[1], &dest[2]);

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      yuv2rgb(y1, u, v, &dest[o], &dest[n], &dest[m]);

      dest += psize2;

      // set first 2 RGB pixels of this block

      y0 = yuv411[j].y0;
      y1 = yuv411[j].y1;

      // avg to get 3/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j].u2);
      q_v = avg_chroma(h_v, yuv411[j].v2);

      // average again to get 5/8, 7/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      yuv2rgb(y0, u, v, &dest[0], &dest[1], &dest[2]);

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      yuv2rgb(y1, u, v, &dest[o], &dest[n], &dest[m]);

      if (add_alpha) dest[3] = dest[7] = 255;
      dest += psize2;

    }
    // write last 2 pixels

    if (add_alpha) dest[3] = dest[7] = 255;

    uyvy.y0 = yuv411[j - 1].y2;
    uyvy.y1 = yuv411[j - 1].y3;
    uyvy.u0 = yuv411[j - 1].u2;
    uyvy.v0 = yuv411[j - 1].v2;
    uyvy2rgb(&uyvy, &dest[0], &(dest[1]), &dest[2], &dest[m], &dest[n], &dest[o]);

    dest += psize2 + orowstride;
    yuv411 += width;
  }
}


static void convert_yuv411_to_argb_frame(yuv411_macropixel *yuv411, int width, int height, int orowstride,
    uint8_t *dest, boolean clamped) {
  uyvy_macropixel uyvy;
  uint8_t u, v, h_u, h_v, q_u, q_v, y0, y1;
  register int j;
  yuv411_macropixel *end = yuv411 + width * height;
  size_t psize = 4, psize2;

  if (LIVES_UNLIKELY(!conv_YR_inited)) init_YUV_to_RGB_tables();

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  orowstride -= width * 4 * psize;
  psize2 = psize << 1;

  while (yuv411 < end) {
    // write 2 ARGB pixels
    dest[0] = dest[4] = 255;

    uyvy.y0 = yuv411[0].y0;
    uyvy.y1 = yuv411[0].y1;
    uyvy.u0 = yuv411[0].u2;
    uyvy.v0 = yuv411[0].v2;
    uyvy2rgb(&uyvy, &dest[1], &(dest[2]), &dest[3], &dest[5], &dest[6], &dest[7]);
    dest += psize2;

    for (j = 1; j < width; j++) {
      // convert 6 yuv411 bytes to 4 argb pixels

      // average first 2 ARGB pixels of this block and last 2 ARGB pixels of previous block

      y0 = yuv411[j - 1].y2;
      y1 = yuv411[j - 1].y3;

      h_u = avg_chroma(yuv411[j - 1].u2, yuv411[j].u2);
      h_v = avg_chroma(yuv411[j - 1].v2, yuv411[j].v2);

      // now we have 1/2, 1/2

      // average last pixel again to get 1/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j - 1].u2);
      q_v = avg_chroma(h_v, yuv411[j - 1].v2);

      // average again to get 1/8, 3/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      yuv2rgb(y0, u, v, &dest[1], &dest[2], &dest[3]);

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      yuv2rgb(y1, u, v, &dest[5], &dest[6], &dest[7]);

      dest += psize2;

      // set first 2 ARGB pixels of this block

      y0 = yuv411[j].y0;
      y1 = yuv411[j].y1;

      // avg to get 3/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j].u2);
      q_v = avg_chroma(h_v, yuv411[j].v2);

      // average again to get 5/8, 7/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      yuv2rgb(y0, u, v, &dest[1], &dest[2], &dest[3]);

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      yuv2rgb(y1, u, v, &dest[5], &dest[6], &dest[7]);

      dest[0] = dest[4] = 255;
      dest += psize2;

    }
    // write last 2 pixels

    dest[0] = dest[4] = 255;

    uyvy.y0 = yuv411[j - 1].y2;
    uyvy.y1 = yuv411[j - 1].y3;
    uyvy.u0 = yuv411[j - 1].u2;
    uyvy.v0 = yuv411[j - 1].v2;
    uyvy2rgb(&uyvy, &dest[1], &(dest[2]), &dest[3], &dest[5], &dest[6], &dest[7]);

    dest += psize2 + orowstride;
    yuv411 += width;
  }
}


static void convert_yuv411_to_yuv888_frame(yuv411_macropixel *yuv411, int width, int height,
    uint8_t *dest, boolean add_alpha, boolean clamped) {
  size_t psize = 3;
  register int j;
  yuv411_macropixel *end = yuv411 + width * height;
  uint8_t u, v, h_u, h_v, q_u, q_v, y0, y1;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (add_alpha) psize = 4;

  while (yuv411 < end) {
    // write 2 RGB pixels
    if (add_alpha) dest[3] = dest[7] = 255;

    // write first 2 pixels
    dest[0] = yuv411[0].y0;
    dest[1] = yuv411[0].u2;
    dest[2] = yuv411[0].v2;
    dest += psize;

    dest[0] = yuv411[0].y1;
    dest[1] = yuv411[0].u2;
    dest[2] = yuv411[0].v2;
    dest += psize;

    for (j = 1; j < width; j++) {
      // convert 6 yuv411 bytes to 4 rgb(a) pixels

      // average first 2 RGB pixels of this block and last 2 RGB pixels of previous block

      y0 = yuv411[j - 1].y2;
      y1 = yuv411[j - 1].y3;

      h_u = avg_chroma(yuv411[j - 1].u2, yuv411[j].u2);
      h_v = avg_chroma(yuv411[j - 1].v2, yuv411[j].v2);

      // now we have 1/2, 1/2

      // average last pixel again to get 1/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j - 1].u2);
      q_v = avg_chroma(h_v, yuv411[j - 1].v2);

      // average again to get 1/8, 3/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      dest[0] = y0;
      dest[1] = u;
      dest[2] = v;
      if (add_alpha) dest[3] = 255;

      dest += psize;

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      dest[0] = y1;
      dest[1] = u;
      dest[2] = v;
      if (add_alpha) dest[3] = 255;

      dest += psize;

      // set first 2 RGB pixels of this block

      y0 = yuv411[j].y0;
      y1 = yuv411[j].y1;

      // avg to get 3/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j].u2);
      q_v = avg_chroma(h_v, yuv411[j].v2);

      // average again to get 5/8, 7/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      dest[0] = y0;
      dest[1] = u;
      dest[2] = v;

      if (add_alpha) dest[3] = 255;
      dest += psize;

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      dest[0] = y1;
      dest[1] = u;
      dest[2] = v;

      if (add_alpha) dest[3] = 255;
      dest += psize;
    }
    // write last 2 pixels

    if (add_alpha) dest[3] = dest[7] = 255;

    dest[0] = yuv411[j - 1].y2;
    dest[1] = yuv411[j - 1].u2;
    dest[2] = yuv411[j - 1].v2;
    dest += psize;

    dest[0] = yuv411[j - 1].y3;
    dest[1] = yuv411[j - 1].u2;
    dest[2] = yuv411[j - 1].v2;

    dest += psize;
    yuv411 += width;
  }
}


static void convert_yuv411_to_yuvp_frame(yuv411_macropixel *yuv411, int width, int height, uint8_t **dest,
    boolean add_alpha, boolean clamped) {
  register int j;
  yuv411_macropixel *end = yuv411 + width * height;
  uint8_t u, v, h_u, h_v, q_u, q_v, y0;

  uint8_t *d_y = dest[0];
  uint8_t *d_u = dest[1];
  uint8_t *d_v = dest[2];
  uint8_t *d_a = dest[3];

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  while (yuv411 < end) {
    // write first 2 pixels
    *(d_y++) = yuv411[0].y0;
    *(d_u++) = yuv411[0].u2;
    *(d_v++) = yuv411[0].v2;
    if (add_alpha) *(d_a++) = 255;

    *(d_y++) = yuv411[0].y0;
    *(d_u++) = yuv411[0].u2;
    *(d_v++) = yuv411[0].v2;
    if (add_alpha) *(d_a++) = 255;

    for (j = 1; j < width; j++) {
      // convert 6 yuv411 bytes to 4 rgb(a) pixels

      // average first 2 RGB pixels of this block and last 2 RGB pixels of previous block

      y0 = yuv411[j - 1].y2;

      h_u = avg_chroma(yuv411[j - 1].u2, yuv411[j].u2);
      h_v = avg_chroma(yuv411[j - 1].v2, yuv411[j].v2);

      // now we have 1/2, 1/2

      // average last pixel again to get 1/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j - 1].u2);
      q_v = avg_chroma(h_v, yuv411[j - 1].v2);

      // average again to get 1/8, 3/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      *(d_y++) = y0;
      *(d_u++) = u;
      *(d_v++) = v;
      if (add_alpha) *(d_a++) = 255;

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      *(d_y++) = y0;
      *(d_u++) = u;
      *(d_v++) = v;
      if (add_alpha) *(d_a++) = 255;

      // set first 2 RGB pixels of this block

      y0 = yuv411[j].y0;

      // avg to get 3/4, 1/2

      q_u = avg_chroma(h_u, yuv411[j].u2);
      q_v = avg_chroma(h_v, yuv411[j].v2);

      // average again to get 5/8, 7/8

      u = avg_chroma(q_u, yuv411[j - 1].u2);
      v = avg_chroma(q_v, yuv411[j - 1].v2);

      *(d_y++) = y0;
      *(d_u++) = u;
      *(d_v++) = v;
      if (add_alpha) *(d_a++) = 255;

      u = avg_chroma(q_u, yuv411[j].u2);
      v = avg_chroma(q_v, yuv411[j].v2);

      *(d_y++) = y0;
      *(d_u++) = u;
      *(d_v++) = v;
      if (add_alpha) *(d_a++) = 255;
    }
    // write last 2 pixels
    *(d_y++) = yuv411[j - 1].y2;
    *(d_u++) = yuv411[j - 1].u2;
    *(d_v++) = yuv411[j - 1].v2;
    if (add_alpha) *(d_a++) = 255;

    *(d_y++) = yuv411[j - 1].y3;
    *(d_u++) = yuv411[j - 1].u2;
    *(d_v++) = yuv411[j - 1].v2;
    if (add_alpha) *(d_a++) = 255;

    yuv411 += width;
  }
}


static void convert_yuv411_to_uyvy_frame(yuv411_macropixel *yuv411, int width, int height,
    uyvy_macropixel *uyvy, boolean clamped) {
  register int j;
  yuv411_macropixel *end = yuv411 + width * height;
  uint8_t u, v, h_u, h_v, y0;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  while (yuv411 < end) {
    // write first uyvy pixel
    uyvy->u0 = yuv411->u2;
    uyvy->y0 = yuv411->y0;
    uyvy->v0 = yuv411->v2;
    uyvy->y1 = yuv411->y1;

    uyvy++;

    for (j = 1; j < width; j++) {
      // convert 6 yuv411 bytes to 2 uyvy macro pixels

      // average first 2 RGB pixels of this block and last 2 RGB pixels of previous block

      y0 = yuv411[j - 1].y2;

      h_u = avg_chroma(yuv411[j - 1].u2, yuv411[j].u2);
      h_v = avg_chroma(yuv411[j - 1].v2, yuv411[j].v2);

      // now we have 1/2, 1/2

      // average last pixel again to get 1/4

      u = avg_chroma(h_u, yuv411[j - 1].u2);
      v = avg_chroma(h_v, yuv411[j - 1].v2);

      uyvy->u0 = u;
      uyvy->y0 = y0;
      uyvy->v0 = v;
      uyvy->y1 = y0;

      uyvy++;

      // average last pixel again to get 3/4

      u = avg_chroma(h_u, yuv411[j].u2);
      v = avg_chroma(h_v, yuv411[j].v2);

      // set first uyvy macropixel of this block

      y0 = yuv411[j].y0;

      uyvy->u0 = u;
      uyvy->y0 = y0;
      uyvy->v0 = v;
      uyvy->y1 = y0;

      uyvy++;
    }
    // write last uyvy macro pixel
    uyvy->u0 = yuv411[j - 1].u2;
    uyvy->y0 = yuv411[j - 1].y2;
    uyvy->v0 = yuv411[j - 1].v2;
    uyvy->y1 = yuv411[j - 1].y3;

    uyvy++;

    yuv411 += width;
  }
}


static void convert_yuv411_to_yuyv_frame(yuv411_macropixel *yuv411, int width, int height, yuyv_macropixel *yuyv,
    boolean clamped) {
  register int j;
  yuv411_macropixel *end = yuv411 + width * height;
  uint8_t u, v, h_u, h_v, y0;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  while (yuv411 < end) {
    // write first yuyv pixel
    yuyv->y0 = yuv411->y0;
    yuyv->u0 = yuv411->u2;
    yuyv->y1 = yuv411->y1;
    yuyv->v0 = yuv411->v2;

    yuyv++;

    for (j = 1; j < width; j++) {
      // convert 6 yuv411 bytes to 2 yuyv macro pixels

      // average first 2 RGB pixels of this block and last 2 RGB pixels of previous block

      y0 = yuv411[j - 1].y2;

      h_u = avg_chroma(yuv411[j - 1].u2, yuv411[j].u2);
      h_v = avg_chroma(yuv411[j - 1].v2, yuv411[j].v2);

      // now we have 1/2, 1/2

      // average last pixel again to get 1/4

      u = avg_chroma(h_u, yuv411[j - 1].u2);
      v = avg_chroma(h_v, yuv411[j - 1].v2);

      yuyv->y0 = y0;
      yuyv->u0 = u;
      yuyv->y1 = y0;
      yuyv->v0 = v;

      yuyv++;

      // average last pixel again to get 3/4

      u = avg_chroma(h_u, yuv411[j].u2);
      v = avg_chroma(h_v, yuv411[j].v2);

      // set first yuyv macropixel of this block

      y0 = yuv411[j].y0;

      yuyv->y0 = y0;
      yuyv->u0 = u;
      yuyv->y1 = y0;
      yuyv->v0 = v;

      yuyv++;
    }
    // write last yuyv macro pixel
    yuyv->y0 = yuv411[j - 1].y2;
    yuyv->u0 = yuv411[j - 1].u2;
    yuyv->y1 = yuv411[j - 1].y3;
    yuyv->v0 = yuv411[j - 1].v2;

    yuyv++;

    yuv411 += width;
  }
}


static void convert_yuv411_to_yuv422_frame(yuv411_macropixel *yuv411, int width, int height, uint8_t **dest,
    boolean clamped) {
  register int j;
  yuv411_macropixel *end = yuv411 + width * height;
  uint8_t h_u, h_v;

  uint8_t *d_y = dest[0];
  uint8_t *d_u = dest[1];
  uint8_t *d_v = dest[2];

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  while (yuv411 < end) {
    // write first 2 y and 1 uv pixel
    *(d_y++) = yuv411->y0;
    *(d_y++) = yuv411->y1;
    *(d_u++) = yuv411->u2;
    *(d_v++) = yuv411->v2;

    for (j = 1; j < width; j++) {
      // convert 6 yuv411 bytes to 2 yuyv macro pixels

      // average first 2 RGB pixels of this block and last 2 RGB pixels of previous block

      *(d_y++) = yuv411[j - 1].y2;
      *(d_y++) = yuv411[j - 1].y3;

      h_u = avg_chroma(yuv411[j - 1].u2, yuv411[j].u2);
      h_v = avg_chroma(yuv411[j - 1].v2, yuv411[j].v2);

      // now we have 1/2, 1/2

      // average last pixel again to get 1/4

      *(d_u++) = avg_chroma(h_u, yuv411[j - 1].u2);
      *(d_v++) = avg_chroma(h_v, yuv411[j - 1].v2);

      // average first pixel to get 3/4

      *(d_y++) = yuv411[j].y0;
      *(d_y++) = yuv411[j].y1;

      *(d_u++) = avg_chroma(h_u, yuv411[j].u2);
      *(d_v++) = avg_chroma(h_v, yuv411[j].v2);

    }
    // write last pixels
    *(d_y++) = yuv411[j - 1].y2;
    *(d_y++) = yuv411[j - 1].y3;
    *(d_u++) = yuv411[j - 1].u2;
    *(d_v++) = yuv411[j - 1].v2;

    yuv411 += width;
  }
}


static void convert_yuv411_to_yuv420_frame(yuv411_macropixel *yuv411, int width, int height, uint8_t **dest,
    boolean is_yvu, boolean clamped) {
  register int j;
  yuv411_macropixel *end = yuv411 + width * height;
  uint8_t h_u, h_v, u, v;

  uint8_t *d_y = dest[0];
  uint8_t *d_u;
  uint8_t *d_v;

  boolean chroma = FALSE;

  size_t width2 = width << 1;

  if (!is_yvu) {
    d_u = dest[1];
    d_v = dest[2];
  } else {
    d_u = dest[2];
    d_v = dest[1];
  }

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  while (yuv411 < end) {
    // write first 2 y and 1 uv pixel
    *(d_y++) = yuv411->y0;
    *(d_y++) = yuv411->y1;

    u = yuv411->u2;
    v = yuv411->v2;

    if (!chroma) {
      *(d_u++) = u;
      *(d_v++) = v;
    } else {
      *d_u = avg_chroma(*d_u, u);
      *d_v = avg_chroma(*d_v, v);
    }

    for (j = 1; j < width; j++) {
      // convert 6 yuv411 bytes to 2 yuyv macro pixels

      // average first 2 RGB pixels of this block and last 2 RGB pixels of previous block

      *(d_y++) = yuv411[j - 1].y2;
      *(d_y++) = yuv411[j - 1].y3;

      h_u = avg_chroma(yuv411[j - 1].u2, yuv411[j].u2);
      h_v = avg_chroma(yuv411[j - 1].v2, yuv411[j].v2);

      // now we have 1/2, 1/2

      // average last pixel again to get 1/4

      u = avg_chroma(h_u, yuv411[j - 1].u2);
      v = avg_chroma(h_v, yuv411[j - 1].v2);

      if (!chroma) {
        *(d_u++) = u;
        *(d_v++) = v;
      } else {
        *d_u = avg_chroma(*d_u, u);
        *d_v = avg_chroma(*d_v, v);
      }

      // average first pixel to get 3/4

      *(d_y++) = yuv411[j].y0;
      *(d_y++) = yuv411[j].y1;

      u = avg_chroma(h_u, yuv411[j].u2);
      v = avg_chroma(h_v, yuv411[j].v2);

      if (!chroma) {
        *(d_u++) = u;
        *(d_v++) = v;
      } else {
        *d_u = avg_chroma(*d_u, u);
        *d_v = avg_chroma(*d_v, v);
      }

    }

    // write last pixels
    *(d_y++) = yuv411[j - 1].y2;
    *(d_y++) = yuv411[j - 1].y3;

    u = yuv411[j - 1].u2;
    v = yuv411[j - 1].v2;

    if (!chroma) {
      *(d_u++) = u;
      *(d_v++) = v;

      d_u -= width2;
      d_v -= width2;

    } else {
      *d_u = avg_chroma(*d_u, u);
      *d_v = avg_chroma(*d_v, v);
    }

    chroma = !chroma;
    yuv411 += width;
  }
}


static void convert_yuv420_to_yuv411_frame(uint8_t **src, int hsize, int vsize, yuv411_macropixel *dest,
    boolean is_422, boolean clamped) {
  // TODO -handle various sampling types

  register int i = 0, j;
  uint8_t *y, *u, *v, *end;
  boolean chroma = TRUE;

  size_t qwidth, hwidth;

  // TODO - handle different in sampling types
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  y = src[0];
  u = src[1];
  v = src[2];

  end = y + hsize * vsize;

  hwidth = hsize >> 1;
  qwidth = hwidth >> 1;

  while (y < end) {
    for (j = 0; j < qwidth; j++) {
      dest->u2 = avg_chroma(u[0], u[1]);
      dest->y0 = y[0];
      dest->y1 = y[1];
      dest->v2 = avg_chroma(v[0], v[1]);
      dest->y2 = y[2];
      dest->y3 = y[3];

      if (!is_422 && chroma && i > 0) {
        dest[-qwidth].u2 = avg_chroma(dest[-qwidth].u2, dest->u2);
        dest[-qwidth].v2 = avg_chroma(dest[-qwidth].v2, dest->v2);
      }
      dest++;
      y += 4;
      u += 2;
      v += 2;
    }
    chroma = !chroma;
    if (!chroma && !is_422) {
      u -= hwidth;
      v -= hwidth;
    }
    i++;
  }
}


static void convert_splitplanes_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                      uint8_t **dest, boolean src_alpha, boolean dest_alpha) {
  // TODO - orowstrides
  // convert 888(8) packed to 444(4)P planar
  size_t size = width * height;
  int ipsize = 3;

  uint8_t *y = dest[0];
  uint8_t *u = dest[1];
  uint8_t *v = dest[2];
  uint8_t *a = dest[3];

  uint8_t *end;

  register int i, j;

  if (src_alpha) ipsize = 4;

  if (irowstride == ipsize * width && irowstride == orowstride) {
    for (end = src + size * ipsize; src < end;) {
      *(y++) = *(src++);
      *(u++) = *(src++);
      *(v++) = *(src++);
      if (dest_alpha) {
        if (src_alpha) *(a++) = *(src++);
        else *(a++) = 255;
      }
    }
  } else {
    width *= ipsize;
    irowstride -= width;
    orowstride -= width;
    for (i = 0; i < height; i++) {
      for (j = 0; j < width; j += ipsize) {
        *(y++) = *(src++);
        *(u++) = *(src++);
        *(v++) = *(src++);
        if (dest_alpha) {
          if (src_alpha) *(a++) = *(src++);
          else *(a++) = 255;
        }
      }
      y += orowstride;
      u += orowstride;
      v += orowstride;
      if (dest_alpha) {
        a += orowstride;
      }
      src += irowstride;
    }
  }
}


/////////////////////////////////////////////////////////////////
// RGB palette conversions

static void convert_swap3_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                uint8_t *dest, int thread_id) {
  // swap 3 byte palette
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_swap3_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_swap3_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if (src == dest) {
    uint8_t tmp;
    int width3 = width * 3;
    orowstride -= width3;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width3; i += 3) {
        tmp = src[i];
        dest[0] = src[i + 2]; // red
        dest[2] = tmp; // blue
        dest += 3;
      }
      dest += orowstride;
    }
    return;
  }

  if ((irowstride == width * 3) && (orowstride == irowstride)) {
    // quick version
#ifdef ENABLE_OIL
    oil_rgb2bgr(dest, src, width * height);
#else
    for (; src < end; src += 3) {
      *(dest++) = src[2]; // red
      *(dest++) = src[1]; // green
      *(dest++) = src[0]; // blue
    }
#endif
  } else {
    int width3 = width * 3;
    orowstride -= width3;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width3; i += 3) {
        *(dest++) = src[i + 2]; // red
        *(dest++) = src[i + 1]; // green
        *(dest++) = src[i]; // blue
      }
      dest += orowstride;
    }
  }
}


void *convert_swap3_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_swap3_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                      ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_swap4_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                uint8_t *dest, int thread_id) {
  // swap 4 byte palette
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if ((height - dheight * i) < dheight) dheight = height - (dheight * i);

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_swap4_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_swap4_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if (src == dest) {
    uint8_t tmp[4];
    int width4 = width * 4;
    orowstride -= width4;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width4; i += 4) {
        tmp[0] = src[i + 3]; // alpha
        tmp[1] = src[i + 2]; // red
        tmp[2] = src[i + 1]; // green
        tmp[3] = src[i]; // blue
        lives_memcpy(dest, tmp, 4);
        dest += 4;
      }
      dest += orowstride;
    }
    return;
  }

  if ((irowstride == width * 4) && (orowstride == irowstride)) {
    // quick version
    for (; src < end; src += 4) {
      *(dest++) = src[3]; // alpha
      *(dest++) = src[2]; // red
      *(dest++) = src[1]; // green
      *(dest++) = src[0]; // blue
    }
  } else {
    int width4 = width * 4;
    orowstride -= width4;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width4; i += 4) {
        *(dest++) = src[i + 3]; // alpha
        *(dest++) = src[i + 2]; // red
        *(dest++) = src[i + 1]; // green
        *(dest++) = src[i]; // blue
      }
      dest += orowstride;
    }
  }
}


void *convert_swap4_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_swap4_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                      ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_swap3addpost_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                       uint8_t *dest, int thread_id) {
  // swap 3 bytes, add post alpha
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_swap3addpost_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_swap3addpost_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if ((irowstride == width * 3) && (orowstride == width * 4)) {
    // quick version
    for (; src < end; src += 3) {
      *(dest++) = src[2]; // red
      *(dest++) = src[1]; // green
      *(dest++) = src[0]; // blue
      *(dest++) = 255; // alpha
    }
  } else {
    int width3 = width * 3;
    orowstride -= width * 4;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width3; i += 3) {
        *(dest++) = src[i + 2]; // red
        *(dest++) = src[i + 1]; // green
        *(dest++) = src[i]; // blue
        *(dest++) = 255; // alpha
      }
      dest += orowstride;
    }
  }
}


void *convert_swap3addpost_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_swap3addpost_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                             ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_swap3addpre_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                      uint8_t *dest, int thread_id) {
  // swap 3 bytes, add pre alpha
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_swap3addpre_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_swap3addpre_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if ((irowstride == width * 3) && (orowstride == width * 4)) {
    // quick version
    for (; src < end; src += 3) {
      *(dest++) = 255; // alpha
      *(dest++) = src[2]; // red
      *(dest++) = src[1]; // green
      *(dest++) = src[0]; // blue
    }
  } else {
    int width3 = width * 3;
    orowstride -= width * 4;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width3; i += 3) {
        *(dest++) = 255; // alpha
        *(dest++) = src[i + 2]; // red
        *(dest++) = src[i + 1]; // green
        *(dest++) = src[i]; // blue
      }
      dest += orowstride;
    }
  }
}


void *convert_swap3addpre_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_swap3addpre_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_swap3postalpha_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
    uint8_t *dest, int thread_id) {
  // swap 3 bytes, leave alpha
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_swap3postalpha_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_swap3postalpha_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if (src == dest) {
    uint8_t tmp;
    int width4 = width * 4;
    orowstride -= width4;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width4; i += 4) {
        tmp = src[i]; // red
        dest[0] = src[i + 2];
        dest[2] = tmp;
        dest += 4;
      }
      dest += orowstride;
    }
    return;
  }

  if ((irowstride == width * 4) && (orowstride == irowstride)) {
    // quick version
    for (; src < end; src += 4) {
      *(dest++) = src[2]; // red
      *(dest++) = src[1]; // green
      *(dest++) = src[0]; // blue
      *(dest++) = src[3]; // alpha
    }
  } else {
    int width4 = width * 4;
    orowstride -= width4;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width4; i += 4) {
        *(dest++) = src[i + 2]; // red
        *(dest++) = src[i + 1]; // green
        *(dest++) = src[i]; // blue
        *(dest++) = src[i + 3]; // alpha
      }
      dest += orowstride;
    }
  }
}


void *convert_swap3postalpha_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_swap3postalpha_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                               ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_addpost_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                  uint8_t *dest, int thread_id) {
  // add post alpha
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_addpost_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_addpost_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if ((irowstride == width * 3) && (orowstride == width * 4)) {
    // quick version
#ifdef ENABLE_OIL
    oil_rgb2rgba(dest, src, width * height);
#else
    for (; src < end; src += 3) {
      lives_memcpy(dest, src, 3);
      dest += 3;
      *(dest++) = 255; // alpha
    }
#endif
  } else {
    int width3 = width * 3;
    orowstride -= width * 4;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width3; i += 3) {
        lives_memcpy(dest, src + i, 3);
        dest += 3;
        *(dest++) = 255; // alpha
      }
      dest += orowstride;
    }
  }
}


void *convert_addpost_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_addpost_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                        ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_addpre_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                 uint8_t *dest, int thread_id) {
  // add pre alpha
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_addpre_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_addpre_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if ((irowstride == width * 3) && (orowstride == width * 4)) {
    // quick version
    for (; src < end; src += 3) {
      *(dest++) = 255; // alpha
      lives_memcpy(dest, src, 3);
      dest += 3;
    }
  } else {
    int width3 = width * 3;
    orowstride -= width * 4;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width3; i += 3) {
        *(dest++) = 255; // alpha
        lives_memcpy(dest, src + i, 3);
        dest += 3;
      }
      dest += orowstride;
    }
  }
}


void *convert_addpre_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_addpre_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                       ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_swap3delpost_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                       uint8_t *dest, int thread_id) {
  // swap 3 bytes, delete post alpha
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_swap3delpost_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_swap3delpost_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if ((irowstride == width * 4) && (orowstride == width * 3)) {
    // quick version
    for (; src < end; src += 4) {
      *(dest++) = src[2]; // red
      *(dest++) = src[1]; // green
      *(dest++) = src[0]; // blue
    }
  } else {
    int width4 = width * 4;
    orowstride -= width * 3;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width4; i += 4) {
        *(dest++) = src[i + 2]; // red
        *(dest++) = src[i + 1]; // green
        *(dest++) = src[i]; // blue
      }
      dest += orowstride;
    }
  }
}


void *convert_swap3delpost_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_swap3delpost_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                             ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_delpost_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                  uint8_t *dest, int thread_id) {
  // delete post alpha
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_delpost_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_delpost_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if ((irowstride == width * 4) && (orowstride == width * 3)) {
    // quick version
    for (; src < end; src += 4) {
      lives_memcpy(dest, src, 3);
      dest += 3;
    }
  } else {
    int width4 = width * 4;
    orowstride -= width * 3;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width4; i += 4) {
        lives_memcpy(dest, src + i, 3);
        dest += 3;
      }
      dest += orowstride;
    }
  }
}


void *convert_delpost_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_delpost_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                        ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_delpre_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                 uint8_t *dest, int thread_id) {
  // delete pre alpha
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_delpre_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_delpre_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  src++;

  if ((irowstride == width * 4) && (orowstride == width * 3)) {
    // quick version
    for (; src < end; src += 4) {
      lives_memcpy(dest, src, 3);
      dest += 3;
    }
  } else {
    int width4 = width * 4;
    orowstride -= width * 3;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width4; i += 4) {
        lives_memcpy(dest, src + i, 3);
        dest += 3;
      }
      dest += orowstride;
    }
  }
}


void *convert_delpre_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_delpre_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                       ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_swap3delpre_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                      uint8_t *dest, int thread_id) {
  // delete pre alpha, swap last 3
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;
        ccparams[i].thread_id = i;

        if (i == 0) convert_swap3delpre_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_swap3delpre_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if ((irowstride == width * 4) && (orowstride == width * 3)) {
    // quick version
    for (; src < end; src += 4) {
      *(dest++) = src[3]; // red
      *(dest++) = src[2]; // green
      *(dest++) = src[1]; // blue
    }
  } else {
    int width4 = width * 4;
    orowstride -= width * 3;
    for (; src < end; src += irowstride) {
      for (i = 0; i < width4; i += 4) {
        *(dest++) = src[i + 3]; // red
        *(dest++) = src[i + 2]; // green
        *(dest++) = src[i + 1]; // blue
      }
      dest += orowstride;
    }
  }
}


void *convert_swap3delpre_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_swap3delpre_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_swapprepost_frame(uint8_t *src, int width, int height, int irowstride, int orowstride,
                                      uint8_t *dest, boolean alpha_first, int thread_id) {
  // swap first and last bytes in a 4 byte palette
  uint8_t *end = src + height * irowstride;
  register int i;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * irowstride) < end) {
        ccparams[i].src = src + dheight * i * irowstride;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * orowstride;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].irowstrides[0] = irowstride;
        ccparams[i].orowstrides[0] = orowstride;

        ccparams[i].alpha_first = alpha_first;

        ccparams[i].thread_id = i;

        if (i == 0) convert_swapprepost_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_swapprepost_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  if (src == dest) {
    uint8_t tmp;
    int width4 = width << 2;
    orowstride -= width4;
    for (; src < end; src += irowstride) {
      if (alpha_first) {
        for (i = 0; i < width4; i += 4) {
          tmp = dest[i];
          lives_memmove(&dest[i], &dest[i + 1], 3);
          dest[i + 3] = tmp;
        }
      } else {
        for (i = 0; i < width4; i += 4) {
          tmp = dest[i + 3];
          lives_memmove(&dest[i + 1], &dest[i], 3);
          dest[i] = tmp;
        }
      }
      dest += orowstride;
    }
    return;
  } else {
    uint8_t tmp;
    int width4 = width << 2;
    orowstride -= width4;
    for (; src < end; src += irowstride) {
      if (alpha_first) {
        for (i = 0; i < width4; i += 4) {
          tmp = src[i];
          lives_memcpy(&dest[i], &src[i + 1], 3);
          dest[i + 3] = tmp;
        }
      } else {
        for (i = 0; i < width4; i += 4) {
          tmp = dest[i + 3];
          lives_memcpy(&dest[i + 1], &src[i], 3);
          dest[i] = tmp;
        }
      }
      dest += orowstride;
    }
  }
}


void *convert_swapprepost_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_swapprepost_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize, ccparams->irowstrides[0],
                            ccparams->orowstrides[0], (uint8_t *)ccparams->dest, ccparams->alpha_first, ccparams->thread_id);
  return NULL;
}


//////////////////////////
// genric YUV

static void convert_swab_frame(uint8_t *src, int width, int height, uint8_t *dest, int thread_id) {
  register int i;
  int width4 = width * 4;
  uint8_t *end = src + height * width4;

  if (thread_id == -1 && prefs->nfx_threads > 1) {
    int nthreads = 0;
    int dheight;
    lives_cc_params *ccparams = (lives_cc_params *)lives_malloc(prefs->nfx_threads * sizeof(lives_cc_params));

    dheight = CEIL((double)height / (double)prefs->nfx_threads, 4);
    for (i = 0; i < prefs->nfx_threads; i++) {
      if ((src + dheight * i * width4) < end) {
        ccparams[i].src = src + dheight * i * width4;
        ccparams[i].hsize = width;
        ccparams[i].dest = dest + dheight * i * width4;

        if (dheight * (i + 1) > (height - 4)) {
          dheight = height - (dheight * i);
        }

        ccparams[i].vsize = dheight;

        ccparams[i].thread_id = i;

        if (i == 0) convert_swab_frame_thread(&ccparams[i]);
        else {
          pthread_create(&cthreads[i], NULL, convert_swab_frame_thread, &ccparams[i]);
          nthreads = i + 1;
        }
      }
    }

    for (i = 1; i < nthreads; i++) {
      pthread_join(cthreads[i], NULL);
    }
    lives_free(ccparams);
    return;
  }

  for (; src < end; src += width4) {
    for (i = 0; i < width4; i += 4) {
      swab(&src[i], &dest[i], 4);
    }
    dest += width4;
  }
}


void *convert_swab_frame_thread(void *data) {
  lives_cc_params *ccparams = (lives_cc_params *)data;
  convert_swab_frame((uint8_t *)ccparams->src, ccparams->hsize, ccparams->vsize,
                     (uint8_t *)ccparams->dest, ccparams->thread_id);
  return NULL;
}


static void convert_halve_chroma(uint8_t **src, int width, int height, int *istrides, int *ostrides,
                                 uint8_t **dest, boolean clamped) {
  // width and height here are width and height of src *chroma* planes, in bytes

  // halve the chroma samples vertically, with sub-sampling, e.g. 422p to 420p

  // TODO : handle different sampling methods in and out

  register int i, j;
  uint8_t *d_u = dest[1], *d_v = dest[2], *s_u = src[1], *s_v = src[2];
  boolean chroma = FALSE;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      if (!chroma) {
        // pass 1, copy row
        lives_memcpy(d_u, s_u, width);
        lives_memcpy(d_v, s_v, width);
      } else {
        // pass 2
        // average two dest rows
        d_u[j] = avg_chroma(d_u[j], s_u[j]);
        d_v[j] = avg_chroma(d_v[j], s_v[j]);
      }
    }
    if (chroma) {
      d_u += ostrides[1];
      d_v += ostrides[2];
    }
    chroma = !chroma;
    s_u += istrides[1];
    s_v += istrides[2];
  }
}


static void convert_double_chroma(uint8_t **src, int width, int height, int *istrides, int *ostrides,
                                  uint8_t **dest, boolean clamped) {
  // width and height here are width and height of src *chroma* planes, in bytes

  // double two chroma planes vertically, with interpolation: eg: 420p to 422p

  // TODO - handle different sampling methods in and out

  register int i, j;
  uint8_t *d_u = dest[1], *d_v = dest[2], *s_u = src[1], *s_v = src[2];
  boolean chroma = FALSE;
  int height2 = height << 1;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  for (i = 0; i < height2; i++) {
    for (j = 0; j < width; j++) {
      lives_memcpy(d_u, s_u, width);
      lives_memcpy(d_v, s_v, width);

      if (!chroma && i > 0) {
        // pass 2
        // average two src rows
        d_u[j - ostrides[1]] = avg_chroma(d_u[j - ostrides[1]], s_u[j]);
        d_v[j - ostrides[2]] = avg_chroma(d_v[j - ostrides[2]], s_v[j]);
      }
    }
    if (chroma) {
      s_u += istrides[1];
      s_v += istrides[2];
    }
    chroma = !chroma;
    d_u += ostrides[1];
    d_v += ostrides[2];
  }
}


static void convert_quad_chroma(uint8_t **src, int width, int height, int *istrides, int ostride, uint8_t **dest,
                                boolean add_alpha, boolean clamped) {
  // width and height here are width and height of dest chroma planes, in bytes

  // double the chroma samples vertically and horizontally, with interpolation, eg. 420p to 444p

  // output to planes

  //TODO: handle mpeg and dvpal input

  // TESTED !

  register int i, j;
  uint8_t *d_u = dest[1], *d_v = dest[2], *s_u = src[1], *s_v = src[2];
  boolean chroma = FALSE;
  int height2;
  int width2;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  height >>= 1;
  width >>= 1;

  height2 = height << 1;
  width2 = width << 1;

  // for this algorithm, we assume chroma samples are aligned like mpeg

  for (i = 0; i < height2; i++) {
    d_u[0] = d_u[1] = s_u[0];
    d_v[0] = d_v[1] = s_v[0];
    for (j = 2; j < width2; j += 2) {
      d_u[j + 1] = d_u[j] = s_u[(j >> 1)];
      d_v[j + 1] = d_v[j] = s_v[(j >> 1)];

      d_u[j - 1] = avg_chroma(d_u[j - 1], d_u[j]);
      d_v[j - 1] = avg_chroma(d_v[j - 1], d_v[j]);

      if (!chroma && i > 0) {
        // pass 2
        // average two src rows (e.g 2 with 1, 4 with 3, ... etc) for odd dst rows
        // thus dst row 1 becomes average of src chroma rows 0 and 1, etc.)
        d_u[j - ostride] = avg_chroma(d_u[j - ostride], d_u[j]);
        d_v[j - ostride] = avg_chroma(d_v[j - ostride], d_v[j]);
        d_u[j - 1 - ostride] = avg_chroma(d_u[j - 1 - ostride], d_u[j - 1]);
        d_v[j - 1 - ostride] = avg_chroma(d_v[j - 1 - ostride], d_v[j - 1]);
      }
    }
    if (!chroma && i > 0) {
      d_u[j - 1 - ostride] = avg_chroma(d_u[j - 1 - ostride], d_u[j - 1]);
      d_v[j - 1 - ostride] = avg_chroma(d_v[j - 1 - ostride], d_v[j - 1]);
    }
    if (chroma) {
      s_u += istrides[1];
      s_v += istrides[2];
    }
    chroma = !chroma;
    d_u += ostride;
    d_v += ostride;
  }

  if (add_alpha) lives_memset(dest + ((width * height) << 3), 255, ((width * height) << 3));
}


static void convert_quad_chroma_packed(uint8_t **src, int width, int height, int *istrides, int ostride,
                                       uint8_t *dest, boolean add_alpha,
                                       boolean clamped) {
  // width and height here are width and height of dest chroma planes, in bytes
  // stretch (double) the chroma samples vertically and horizontally, with interpolation

  // ouput to packed pixels

  // e.g: 420p to 888(8)

  //TODO: handle mpeg and dvpal input

  // TESTED !

  register int i, j;
  uint8_t *s_y = src[0], *s_u = src[1], *s_v = src[2];
  boolean chroma = FALSE;
  int widthx;
  int irow = istrides[0] - width;
  int opsize = 3, opsize2;

  //int count;
  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (add_alpha) opsize = 4;

  widthx = width * opsize;
  opsize2 = opsize * 2;

  for (i = 0; i < height; i++) {
    dest[0] = *(s_y++);
    dest[1] = dest[opsize + 1] = s_u[0];
    dest[2] = dest[opsize + 2] = s_v[0];
    if (add_alpha) dest[3] = dest[opsize + 3] = 255;
    dest[opsize] = *(s_y++);

    //count=10;

    for (j = opsize2; j < widthx; j += opsize2) {
      // process two pixels at a time, and we average the first colour pixel with the last from the previous 2
      // we know we can do this because Y must be even width

      // implements jpeg style subsampling : TODO - mpeg and dvpal style

      dest[j] = *(s_y++);
      dest[j + opsize] = *(s_y++);
      if (add_alpha) dest[j + 3] = dest[j + 7] = 255;
      dest[j + 1] = dest[j + opsize + 1] = s_u[(j / opsize) / 2];
      dest[j + 2] = dest[j + opsize + 2] = s_v[(j / opsize) / 2];

      dest[j - opsize + 1] = avg_chroma(dest[j - opsize + 1], dest[j + 1]);
      dest[j - opsize + 2] = avg_chroma(dest[j - opsize + 2], dest[j + 2]);
      if (!chroma && i > 0) {
        // pass 2
        // average two src rows
        dest[j + 1 - ostride] = avg_chroma(dest[j + 1 - ostride], dest[j + 1]);
        dest[j + 2 - ostride] = avg_chroma(dest[j + 2 - ostride], dest[j + 2]);
        dest[j - opsize + 1 - ostride] = avg_chroma(dest[j - opsize + 1 - ostride], dest[j - opsize + 1]);
        dest[j - opsize + 2 - ostride] = avg_chroma(dest[j - opsize + 2 - ostride], dest[j - opsize + 2]);
      }
    }
    if (!chroma && i > 0) {
      dest[j - opsize + 1 - ostride] = avg_chroma(dest[j - opsize + 1 - ostride], dest[j - opsize + 1]);
      dest[j - opsize + 2 - ostride] = avg_chroma(dest[j - opsize + 2 - ostride], dest[j - opsize + 2]);
    }
    if (chroma) {
      s_u += istrides[1];
      s_v += istrides[2];
    }
    s_y += irow;
    chroma = !chroma;
    dest += ostride;
  }
}


static void convert_double_chroma_packed(uint8_t **src, int width, int height, int *istrides, int ostride, uint8_t *dest, boolean add_alpha,
    boolean clamped) {
  // width and height here are width and height of dest chroma planes, in bytes
  // double the chroma samples horizontally, with interpolation

  // output to packed pixels

  // e.g 422p to 888(8)

  //TODO: handle non-dvntsc in

  register int i, j;
  uint8_t *s_y = src[0], *s_u = src[1], *s_v = src[2];
  int widthx;
  int irow = istrides[0] - width;
  int opsize = 3, opsize2;

  set_conversion_arrays(clamped ? WEED_YUV_CLAMPING_CLAMPED : WEED_YUV_CLAMPING_UNCLAMPED, WEED_YUV_SUBSPACE_YCBCR);

  if (add_alpha) opsize = 4;

  widthx = width * opsize;
  opsize2 = opsize * 2;

  for (i = 0; i < height; i++) {
    dest[0] = *(s_y++);
    dest[1] = dest[opsize + 1] = s_u[0];
    dest[2] = dest[opsize + 2] = s_v[0];
    if (add_alpha) dest[3] = dest[opsize + 3] = 255;
    dest[opsize] = *(s_y++);
    for (j = opsize2; j < widthx; j += opsize2) {
      // dvntsc style - chroma is aligned with luma

      // process two pixels at a time, and we average the first colour pixel with the last from the previous 2
      // we know we can do this because Y must be even width
      dest[j] = *(s_y++);
      dest[j + opsize] = *(s_y++);
      if (add_alpha) dest[j + opsize - 1] = dest[j + opsize2 - 1] = 255;

      dest[j + 1] = dest[j + opsize + 1] = s_u[(j / opsize) >> 1];
      dest[j + 2] = dest[j + opsize + 2] = s_v[(j / opsize) >> 1];

      dest[j - opsize + 1] = avg_chroma(dest[j - opsize + 1], dest[j + 1]);
      dest[j - opsize + 2] = avg_chroma(dest[j - opsize + 2], dest[j + 2]);
    }
    s_y += irow;
    s_u += istrides[1];
    s_v += istrides[2];
    dest += ostride;
  }
}


static void switch_yuv_sampling(weed_layer_t *layer) {
  int error;
  int sampling = weed_get_int_value(layer, WEED_LEAF_YUV_SAMPLING, &error);
  int clamping = weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error);
  int subspace = weed_get_int_value(layer, WEED_LEAF_YUV_SUBSPACE, &error);
  int palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);
  int width = (weed_get_int_value(layer, WEED_LEAF_WIDTH, &error) >> 1);
  int height = (weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error) >> 1);
  unsigned char **pixel_data, *dst;

  register int i, j, k;

  if (palette != WEED_PALETTE_YUV420P) return;

  pixel_data = (unsigned char **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);

  set_conversion_arrays(clamping, subspace);

  if (sampling == WEED_YUV_SAMPLING_MPEG) {
    // jpeg is located centrally between Y, mpeg(2) and some flv are located on the left Y
    // so here we just set dst[0]=avg(src[0],src[1]), dst[1]=avg(src[1],src[2]), etc.
    // the last value is repeated once

    width--;
    for (k = 1; k < 3; k++) {
      dst = pixel_data[k];
      for (j = 0; j < height; j++) {
        for (i = 0; i < width; i++) dst[i] = avg_chroma(dst[i], dst[i + 1]);
        dst[i] = dst[i - 1];
        dst += width + 1;
      }
    }
    weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_JPEG);
  } else if (sampling == WEED_YUV_SAMPLING_JPEG) {
    for (k = 1; k < 3; k++) {
      dst = pixel_data[k];
      for (j = 0; j < height; j++) {
        for (i = width - 1; i > 0; i--) dst[i] = avg_chroma(dst[i], dst[i - 1]);
        dst[0] = dst[1];
        dst += width;
      }
    }
    weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_MPEG);
  }
  lives_free(pixel_data);
}


static void switch_yuv_clamping_and_subspace(weed_layer_t *layer, int oclamping, int osubspace) {
  // currently subspace conversions are not performed - TODO
  // we assume subspace Y'CbCr
  int error;
  int iclamping = weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error);
  int isubspace = weed_get_int_value(layer, WEED_LEAF_YUV_SUBSPACE, &error);

  int palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);
  int width = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
  int height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);

  void **pixel_data = weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);

  uint8_t *src, *src1, *src2, *end;

  get_YUV_to_YUV_conversion_arrays(iclamping, isubspace, oclamping, osubspace);

  switch (palette) {
  case WEED_PALETTE_YUVA8888:
    src = (uint8_t *)pixel_data[0];
    end = src + width * height * 4;
    while (src < end) {
      *src = Y_to_Y[*src];
      src++;
      *src = U_to_U[*src];
      src++;
      *src = V_to_V[*src];
      src += 2;
    }
    break;
  case WEED_PALETTE_YUV888:
    src = (uint8_t *)pixel_data[0];
    end = src + width * height * 3;
    while (src < end) {
      *src = Y_to_Y[*src];
      src++;
      *src = U_to_U[*src];
      src++;
      *src = V_to_V[*src];
      src++;
    }
    break;
  case WEED_PALETTE_YUVA4444P:
  case WEED_PALETTE_YUV444P:
    src = (uint8_t *)pixel_data[0];
    src1 = (uint8_t *)pixel_data[1];
    src2 = (uint8_t *)pixel_data[2];
    end = src + width * height;
    while (src < end) {
      *src = Y_to_Y[*src];
      src++;
      *src1 = U_to_U[*src1];
      src1++;
      *src2 = V_to_V[*src2];
      src2++;
    }
    break;
  case WEED_PALETTE_UYVY:
    src = (uint8_t *)pixel_data[0];
    end = src + width * height * 4;
    while (src < end) {
      *src = U_to_U[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src = V_to_V[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
    }
    break;
  case WEED_PALETTE_YUYV:
    src = (uint8_t *)pixel_data[0];
    end = src + width * height * 4;
    while (src < end) {
      *src = Y_to_Y[*src];
      src++;
      *src = U_to_U[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src = V_to_V[*src];
      src++;
    }
    break;
  case WEED_PALETTE_YUV422P:
    src = (uint8_t *)pixel_data[0];
    src1 = (uint8_t *)pixel_data[1];
    src2 = (uint8_t *)pixel_data[2];
    end = src + width * height;
    while (src < end) {
      *src = Y_to_Y[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src1 = U_to_U[*src1];
      src1++;
      *src2 = V_to_V[*src2];
      src2++;
    }
    break;
  case WEED_PALETTE_YVU420P:
    src = (uint8_t *)pixel_data[0];
    src1 = (uint8_t *)pixel_data[2];
    src2 = (uint8_t *)pixel_data[1];
    end = src + width * height;
    while (src < end) {
      *src = Y_to_Y[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src1 = U_to_U[*src1];
      src1++;
      *src2 = V_to_V[*src2];
      src2++;
    }
    break;
  case WEED_PALETTE_YUV420P:
    src = (uint8_t *)pixel_data[0];
    src1 = (uint8_t *)pixel_data[1];
    src2 = (uint8_t *)pixel_data[2];
    end = src + width * height;
    while (src < end) {
      *src = Y_to_Y[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src1 = U_to_U[*src1];
      src1++;
      *src2 = V_to_V[*src2];
      src2++;
    }
    break;
  case WEED_PALETTE_YUV411:
    src = (uint8_t *)pixel_data[0];
    end = src + width * height * 6;
    while (src < end) {
      *src = U_to_U[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src = V_to_V[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
      *src = Y_to_Y[*src];
      src++;
    }
    break;
  }
  weed_set_int_value(layer, WEED_LEAF_YUV_CLAMPING, oclamping);
  lives_free(pixel_data);
}


////////////////////////////////////////////////////////////////////////////////////////
// TODO - move into layers.c

/**
a "layer" is CHANNEL type plant which is not created from a plugin CHANNEL_TEMPLATE.
  When we pass this to a plugin, we need to adjust it depending
  on the plugin's CHANNEL_TEMPLATE to which we will assign it.

  e.g.: memory may need aligning afterwards for particular plugins which set channel template flags:
  layer palette may need changing, layer may need resizing */

/** @brief fills the plane pointed to by ptr with bpix

psize is sizeof(bpix), width, height and rowstride are the dimensions of the target plane
*/
LIVES_INLINE void fill_plane(uint8_t *ptr, int psize, int width, int height, int rowstride, unsigned char *bpix) {
  int i, j;
  rowstride -= width * psize;
  for (i = 0; i < height; i++) {
    for (j = 0; j < width; j++) {
      lives_memcpy(ptr, bpix, psize);
      ptr += psize;
    }
    ptr += rowstride;
  }
}

#define SHIFTVAL sbits
#define ALIGN_SIZE (1 << SHIFTVAL)
#define EXTRA_BYTES 64


/** @brief creates pixel data for layer

   @returns FALSE on memory error

    width, height, and current_palette must be pre-set in layer; width is in (macro) pixels of the palette
    width and height may be adjusted (rounded) in the function
    rowstrides will be set, and each plane will be aligned depending on mainw->rowstride_alignment
    if mainw->rowstride_alignment_hint is non 0 it will set mainw->rowstride_alignment, which must be a power of 2
    the special value -1 for the hint will create compact frames (rowstride = width * pixel_size)

    if black_fill is set, fill with opaque black in the specified palette: for yuv palettes, YUV_clamping may be pre-set
    otherwise it will be set to WEED_YUV_CLAMPING_CLAMPED.

    may_contig should normally be set to TRUE, except for special uses during palette conversion
    if set, then for planar palettes, only plane 0 will be allocated, so only this value should be freed
    in this case, the leaf WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS will be set to WEED_TRUE

    the allocated frames will be aligned to the pixel size for whatever palette and may be padded with extra bytes
    to guard against accidental overwrites
*/
boolean create_empty_pixel_data(weed_layer_t *layer, boolean black_fill, boolean may_contig) {
  int palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, NULL);
  int width = weed_get_int_value(layer, WEED_LEAF_WIDTH, NULL);
  int height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, NULL);
  int rowstride, *rowstrides;

  int clamping = WEED_YUV_CLAMPING_CLAMPED;
  boolean compact = FALSE;

  uint8_t *pixel_data = NULL;
  uint8_t *memblock;
  uint8_t **pd_array;

  unsigned char black[6] = {0, 0, 0, 255, 255, 255};
  unsigned char yuv_black[6] = {16, 128, 128, 255, 255, 255};
  float blackf[4] = {0., 0., 0., 1.};

  size_t framesize, framesize2;

  // max is 128 min is 32,  and it must be a power of 2 (i.e 32, 64, 128)
  int sbits = 7, al, r;
  int rowstride_alignment;

  if (mainw->rowstride_alignment < ALIGN_DEF) mainw->rowstride_alignment = ALIGN_DEF;
  rowstride_alignment = mainw->rowstride_alignment;

  if (mainw->rowstride_alignment_hint > 0) {
    r = rowstride_alignment = mainw->rowstride_alignment_hint;
    for (al = 1 << sbits; (al > ALIGN_MIN && !(al & r)); al >>= 1) sbits--;
    rowstride_alignment = al;
  }
  if (mainw->rowstride_alignment_hint < 0 || (weed_palette_is_alpha(palette) && mainw->rowstride_alignment_hint == 0)) {
    compact = TRUE;
    rowstride_alignment = 0;
  }
  mainw->rowstride_alignment_hint = 0;

  for (sbits = 7; (1 << sbits) > rowstride_alignment; sbits--);

  weed_set_int_value(layer, WEED_LEAF_GAMMA_TYPE, WEED_GAMMA_SRGB);

  if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXBUF_SRC)) {
    weed_leaf_delete(layer, WEED_LEAF_HOST_PIXBUF_SRC);
  }
  if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_SURFACE_SRC)) {
    weed_leaf_delete(layer, WEED_LEAF_HOST_SURFACE_SRC);
  }

  if (black_fill) {
    if (weed_plant_has_leaf(layer, WEED_LEAF_YUV_CLAMPING))
      clamping = weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, NULL);
    if (clamping != WEED_YUV_CLAMPING_CLAMPED) yuv_black[0] = 0;
  }

  switch (palette) {
  case WEED_PALETTE_RGB24:
  case WEED_PALETTE_BGR24:
    rowstride = width * 3;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE) + EXTRA_BYTES;
    pixel_data = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    break;

  case WEED_PALETTE_YUV888:
    rowstride = width * 3;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE) + EXTRA_BYTES;
    pixel_data = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    if (black_fill) fill_plane(pixel_data, 3, width, height, rowstride, yuv_black);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    break;

  case WEED_PALETTE_UYVY8888:
    rowstride = width * 4;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE) + EXTRA_BYTES;
    pixel_data = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    if (black_fill) {
      yuv_black[1] = yuv_black[3] = yuv_black[0];
      yuv_black[0] = yuv_black[2];
      fill_plane(pixel_data, 4, width, height, rowstride, yuv_black);
    }
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    break;

  case WEED_PALETTE_YUYV8888:
    rowstride = width * 4;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE) + EXTRA_BYTES;
    pixel_data = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    if (black_fill) {
      yuv_black[2] = yuv_black[0];
      black[3] = yuv_black[1];
      fill_plane(pixel_data, 4, width, height, rowstride, yuv_black);
    }
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    break;

  case WEED_PALETTE_RGBA32:
  case WEED_PALETTE_BGRA32:
  case WEED_PALETTE_ARGB32:
    rowstride = width * 4;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE) + EXTRA_BYTES;
    pixel_data = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    if (black_fill) {
      if (palette == WEED_PALETTE_ARGB32) {
        black[3] = black[0];
        black[0] = 255;
      }
      fill_plane(pixel_data, 4, width, height, rowstride, black);
    }
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    break;

  case WEED_PALETTE_YUVA8888:
    rowstride = width * 4;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE) + EXTRA_BYTES;
    pixel_data = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    if (black_fill) fill_plane(pixel_data, 4, width, height, rowstride, yuv_black);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    break;

  case WEED_PALETTE_YUV420P:
  case WEED_PALETTE_YVU420P:
    width = (width >> 1) << 1;
    weed_set_int_value(layer, WEED_LEAF_WIDTH, width);
    height = (height >> 1) << 1;
    weed_set_int_value(layer, WEED_LEAF_HEIGHT, height);
    rowstride = width;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE);
    rowstrides = (int *)lives_malloc(sizint * 3);
    rowstrides[0] = rowstride;
    rowstride >>= 1;
    //if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize2 = ALIGN_CEIL(rowstride * (height >> 1), ALIGN_SIZE);
    rowstrides[1] = rowstrides[2] = rowstride;
    weed_set_int_array(layer, WEED_LEAF_ROWSTRIDES, 3, rowstrides);
    lives_free(rowstrides);
    pd_array = (uint8_t **)lives_malloc(3 * sizeof(uint8_t *));

    if (!may_contig) {
      weed_leaf_delete(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS);
      pd_array[0] = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[0] == NULL) {
        lives_free(pd_array);
        return FALSE;
      }
      pd_array[1] = (uint8_t *)lives_calloc((framesize2 + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[1] == NULL) {
        lives_free(pd_array[0]);
        lives_free(pd_array);
        return FALSE;
      }
      pd_array[2] = (uint8_t *)lives_calloc((framesize2  + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[2] == NULL) {
        lives_free(pd_array[1]);
        lives_free(pd_array[0]);
        lives_free(pd_array);
        return FALSE;
      }
    } else {
      weed_set_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, WEED_TRUE);
      memblock = (uint8_t *)lives_calloc((framesize + framesize2 * 2 + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (memblock == NULL) return FALSE;
      pd_array[0] = (uint8_t *)memblock;
      pd_array[1] = (uint8_t *)(memblock + framesize);
      pd_array[2] = (uint8_t *)(memblock + framesize + framesize2);
    }
    if (black_fill) {
      if (yuv_black[0] != 0) lives_memset(pd_array[0], yuv_black[0], framesize);
      if (may_contig) {
        lives_memset(pd_array[1], yuv_black[1], framesize2 * 2); // fill both planes
      } else {
        lives_memset(pd_array[1], yuv_black[1], framesize2);
        lives_memset(pd_array[2], yuv_black[2], framesize2);
      }
    }

    weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)pd_array);
    lives_free(pd_array);
    break;

  case WEED_PALETTE_YUV422P:
    width = (width >> 1) << 1;
    weed_set_int_value(layer, WEED_LEAF_WIDTH, width);
    rowstride = width;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE);
    rowstrides = (int *)lives_malloc(sizint * 3);
    rowstrides[0] = rowstride;
    rowstride = width >> 1;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize2 = ALIGN_CEIL(rowstride * height, ALIGN_SIZE);
    rowstrides[1] = rowstrides[2] = rowstride;
    weed_set_int_array(layer, WEED_LEAF_ROWSTRIDES, 3, rowstrides);
    lives_free(rowstrides);
    pd_array = (uint8_t **)lives_malloc(3 * sizeof(uint8_t *));

    if (!may_contig) {
      weed_leaf_delete(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS);
      pd_array[0] = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[0] == NULL) {
        lives_free(pd_array);
        return FALSE;
      }
      pd_array[1] = (uint8_t *)lives_calloc(framesize2 >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[1] == NULL) {
        lives_free(pd_array[0]);
        lives_free(pd_array);
        return FALSE;
      }
      pd_array[2] = (uint8_t *)lives_calloc((framesize2 + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[2] == NULL) {
        lives_free(pd_array[1]);
        lives_free(pd_array[0]);
        lives_free(pd_array);
        return FALSE;
      }
    } else {
      weed_set_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, WEED_TRUE);
      memblock = (uint8_t *)lives_calloc((framesize + framesize2 * 2 + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (memblock == NULL) return FALSE;
      pd_array[0] = (uint8_t *)memblock;
      pd_array[1] = (uint8_t *)(memblock + framesize);
      pd_array[2] = (uint8_t *)(memblock + framesize + framesize2);
    }
    if (black_fill) {
      if (yuv_black[0] != 0) lives_memset(pd_array[0], yuv_black[0], framesize);
      if (may_contig) {
        lives_memset(pd_array[1], yuv_black[1], framesize2 * 2);
      } else {
        lives_memset(pd_array[1], yuv_black[1], framesize2);
        lives_memset(pd_array[2], yuv_black[2], framesize2);
      }
    }
    weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)pd_array);
    lives_free(pd_array);
    break;

  case WEED_PALETTE_YUV444P:
    rowstride = width;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    rowstrides = (int *)lives_malloc(sizint * 3);
    rowstrides[0] = rowstrides[1] = rowstrides[2] = rowstride;
    weed_set_int_array(layer, WEED_LEAF_ROWSTRIDES, 3, rowstrides);
    lives_free(rowstrides);
    pd_array = (uint8_t **)lives_malloc(3 * sizeof(uint8_t *));
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE);

    if (!may_contig) {
      weed_leaf_delete(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS);
      pd_array[0] = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[0] == NULL) {
        lives_free(pd_array);
        return FALSE;
      }
      pd_array[1] = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[1] == NULL) {
        lives_free(pd_array[0]);
        lives_free(pd_array);
        return FALSE;
      }
      pd_array[2] = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[2] == NULL) {
        lives_free(pd_array[1]);
        lives_free(pd_array[0]);
        lives_free(pd_array);
        return FALSE;
      }
    } else {
      weed_set_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, WEED_TRUE);
      memblock = (uint8_t *)lives_calloc((framesize * 3 + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (memblock == NULL) return FALSE;
      pd_array[0] = memblock;
      pd_array[1] = memblock + framesize;
      pd_array[2] = memblock + framesize * 2;
    }
    if (black_fill) {
      if (yuv_black[0] != 0) lives_memset(pd_array[0], yuv_black[0], framesize);
      if (may_contig) {
        lives_memset(pd_array[1], yuv_black[1], framesize * 2);
      } else {
        lives_memset(pd_array[1], yuv_black[1], framesize);
        lives_memset(pd_array[2], yuv_black[2], framesize);
      }
    }
    weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)pd_array);
    lives_free(pd_array);
    break;

  case WEED_PALETTE_YUVA4444P:
    rowstride = width;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    rowstrides = (int *)lives_malloc(sizint * 4);
    rowstrides[0] = rowstrides[1] = rowstrides[2] = rowstrides[3] = rowstride;
    weed_set_int_array(layer, WEED_LEAF_ROWSTRIDES, 4, rowstrides);
    lives_free(rowstrides);
    pd_array = (uint8_t **)lives_malloc(4 * sizeof(uint8_t *));
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE);

    if (!may_contig) {
      weed_leaf_delete(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS);
      pd_array[0] = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[0] == NULL) {
        lives_free(pd_array);
        return FALSE;
      }
      pd_array[1] = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[1] == NULL) {
        lives_free(pd_array[0]);
        lives_free(pd_array);
        return FALSE;
      }
      pd_array[2] = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[2] == NULL) {
        lives_free(pd_array[1]);
        lives_free(pd_array[0]);
        lives_free(pd_array);
        return FALSE;
      }
      pd_array[3] = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (pd_array[3] == NULL) {
        lives_free(pd_array[2]);
        lives_free(pd_array[1]);
        lives_free(pd_array[0]);
        lives_free(pd_array);
        return FALSE;
      }
    } else {
      weed_set_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, WEED_TRUE);
      memblock = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (memblock == NULL) return FALSE;
      pd_array[0] = memblock;
      pd_array[1] = memblock + framesize;
      pd_array[2] = memblock + framesize * 2;
      pd_array[3] = memblock + framesize * 3;
    }
    if (black_fill) {
      if (yuv_black[0] != 0) {
        lives_memset(pd_array[0], yuv_black[0], framesize * 2);
      }
      if (may_contig) {
        lives_memset(pd_array[1], yuv_black[1], framesize * 2);
      } else {
        lives_memset(pd_array[1], yuv_black[1], framesize);
        lives_memset(pd_array[2], yuv_black[2], framesize);
      }
      lives_memset(pd_array[3], 255, framesize);
    }
    weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 4, (void **)pd_array);
    lives_free(pd_array);
    break;

  case WEED_PALETTE_YUV411:
    rowstride = width * 6; // a macro-pixel is 6 bytes, and contains 4 real pixels
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    weed_set_int_value(layer, WEED_LEAF_WIDTH, width);
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE);
    pixel_data = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    if (black_fill) {
      yuv_black[3] = yuv_black[1];
      yuv_black[1] = yuv_black[2] = yuv_black[4] = yuv_black[5] = yuv_black[0];
      yuv_black[0] = yuv_black[3];
      pixel_data = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
      if (black_fill) {
        fill_plane(pixel_data, 6, width, height, rowstride, black);
      }
    }
    if (pixel_data == NULL) return FALSE;
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    break;

  case WEED_PALETTE_RGBFLOAT:
    rowstride = width * 3 * sizeof(float);
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    pixel_data = (uint8_t *)lives_calloc((rowstride * height + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    break;

  case WEED_PALETTE_RGBAFLOAT:
    rowstride = width * 4 * sizeof(float);
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    pixel_data = (uint8_t *)lives_calloc((rowstride * height + EXTRA_BYTES), ALIGN_SIZE);
    if (black_fill) {
      fill_plane(pixel_data, 4 * sizeof(float), width, height, rowstride, (uint8_t *)blackf);
    }
    if (pixel_data == NULL) return FALSE;
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    break;

  case WEED_PALETTE_AFLOAT:
    rowstride = width * sizeof(float);
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    pixel_data = (uint8_t *)lives_calloc((width * height + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    if (black_fill) {
      blackf[0] = 1.;
      fill_plane(pixel_data, sizeof(float), width, height, rowstride, (uint8_t *)blackf);
    }
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    break;

  case WEED_PALETTE_A8:
    rowstride = width;
    if (!compact) rowstride = ALIGN_CEIL(rowstride, rowstride_alignment);
    framesize = ALIGN_CEIL((rowstride * height + EXTRA_BYTES), ALIGN_SIZE);
    pixel_data = (uint8_t *)lives_calloc(framesize >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    if (black_fill) {
      lives_memset(pixel_data, 255, rowstride * height);
    }
    if (pixel_data == NULL) return FALSE;
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    break;

  case WEED_PALETTE_A1:
    rowstride = (width + 7) >> 3;
    framesize = ALIGN_CEIL(rowstride * height, ALIGN_SIZE);
    pixel_data = (uint8_t *)lives_calloc((framesize + EXTRA_BYTES) >> SHIFTVAL, ALIGN_SIZE);
    if (pixel_data == NULL) return FALSE;
    lives_memset(pixel_data, 255, rowstride * height);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
    break;

  default:
    lives_printerr("Warning: asked to create empty pixel_data for palette %d !\n", palette);
  }
  return TRUE;
}


/** @ brief fills layer with default values.

If either width or height are zero, then dimensions will be taken from the layer or
  defaults used
  if layer has a palette set, that will be maintained, else it will be set to target_palette
  if targette palette is WEED_PALETTE_END then default will be set depending on image_ext
  if this is "jpg" then it will be RGB24, otherwise RGBA32
  finally we create the pixel data for layer */
void create_blank_layer(weed_layer_t *layer, const char *image_ext, int width, int height, int target_palette) {

  // TODO - see if this is useful elsewhere
  int error;
  if ((width == 0 || height == 0) && weed_plant_has_leaf(layer, WEED_LEAF_WIDTH)
      && weed_plant_has_leaf(layer, WEED_LEAF_HEIGHT)) {
    width = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
    height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);
  }
  if (width == 0) width = DEF_FRAME_HSIZE_UNSCALED;
  if (height == 0) height = DEF_FRAME_VSIZE_UNSCALED;
  weed_set_int_value(layer, WEED_LEAF_WIDTH, width);
  weed_set_int_value(layer, WEED_LEAF_HEIGHT, height);
  if (!weed_plant_has_leaf(layer, WEED_LEAF_CURRENT_PALETTE)) {
    if (target_palette != WEED_PALETTE_END) weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, target_palette);
    else {
      if (image_ext == NULL || !strcmp(image_ext, LIVES_FILE_EXT_JPG))
        weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, WEED_PALETTE_RGB24);
      else weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, WEED_PALETTE_RGBA32);
    }
  }
  create_empty_pixel_data(layer, TRUE, TRUE);
}


boolean rowstrides_differ(int n1, int *n1_array, int n2, int *n2_array) {
  // returns TRUE if the rowstrides differ
  int i;

  if (n1 != n2) return TRUE;
  for (i = 0; i < n1; i++) if (n1_array[i] != n2_array[i]) return TRUE;
  return FALSE;
}


LIVES_GLOBAL_INLINE weed_layer_t *weed_layer_new(void) {
  return weed_plant_new(WEED_PLANT_LAYER);
}


weed_layer_t *weed_layer_new_for_frame(int clip, int frame) {
  // create a layer ready to receive a frame from a clip
  weed_layer_t *layer = weed_layer_new();
  weed_set_int_value(layer, WEED_LEAF_CLIP, clip);
  weed_set_int_value(layer, WEED_LEAF_FRAME, frame);
  return layer;
}

// returns TRUE on success
boolean align_pixel_data(weed_layer_t *layer, size_t alignment) {
#ifndef HAVE_POSIX_MEMALIGN
  return FALSE;
#else

  void **pixel_data, **new_pixel_data;
  uint8_t *npixel_data, zpixel_data = 0;
  int *rowstrides;

  size_t size, totsize = 0;

  boolean needs_change = FALSE;
  boolean can_contiguous = TRUE;

  int memerror, error;

  int numplanes, height;

  register int i;

  numplanes = weed_leaf_num_elements(layer, WEED_LEAF_ROWSTRIDES);
  pixel_data = weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);

  for (i = 0; i < numplanes; i++) {
    if (((uint64_t)(pixel_data[i])) % alignment == 0) continue;
    needs_change = TRUE;
  }

  if (!needs_change) return TRUE;

  rowstrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, &error);
  height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);

  for (i = 0; i < numplanes; i++) {
    size = height * rowstrides[i];
    totsize += ALIGN_CEIL(size, 32);
  }

  for (i = 1; i < numplanes; i++) {
    size = height * rowstrides[i];
    zpixel_data += ALIGN_CEIL(size, 32);
    if (zpixel_data % alignment != 0) {
      can_contiguous = FALSE;
      break;
    }
  }

  new_pixel_data = (void **)lives_malloc(numplanes * (sizeof(void *)));

  if (can_contiguous) {
    // all planes can be set in contiguous block
    if ((memerror = posix_memalign((void **)&npixel_data, alignment, totsize))) {
      lives_freep((void **)&new_pixel_data);
      lives_freep((void **)&pixel_data);
      lives_freep((void **)&rowstrides);
      return FALSE;
    }

    for (i = 0; i < numplanes; i++) {
      lives_memcpy(npixel_data, pixel_data[i], height * rowstrides[i]);
      new_pixel_data[i] = npixel_data;
      size = height * rowstrides[i];
      npixel_data += ALIGN_CEIL(size, 32);
    }

    weed_layer_pixel_data_free(layer);

    weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, numplanes, new_pixel_data);
    if (numplanes > 1) weed_set_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, WEED_TRUE);
    else weed_set_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, WEED_FALSE);

    lives_freep((void **)&new_pixel_data);
    lives_freep((void **)&pixel_data);
    lives_freep((void **)&rowstrides);

    return TRUE;
  }

  // non-contiguous
  for (i = 0; i < numplanes; i++) {
    if ((memerror = posix_memalign((void **)&npixel_data, alignment, height * rowstrides[i]))) {
      lives_freep((void **)&new_pixel_data);
      lives_freep((void **)&pixel_data);
      lives_freep((void **)&rowstrides);
      return FALSE;
    }
    lives_memcpy(npixel_data, pixel_data[i], height * rowstrides[i]);
    new_pixel_data[i] = npixel_data;
  }

  weed_layer_pixel_data_free(layer);

  lives_freep((void **)&new_pixel_data);
  lives_freep((void **)&pixel_data);
  lives_freep((void **)&rowstrides);

  weed_set_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, WEED_FALSE);

  return TRUE;
#endif
}


/** (un)premultply alpha using a lookup table

    if un is FALSE we go the other way, and do a pre-multiplication */
void alpha_unpremult(weed_layer_t *layer, boolean un) {
  /// this is only used when going from palette with alpha to one without
  int error;
  int aoffs, coffs, psize, psizel, widthx;
  int alpha;
  int flags = 0;
  int width = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
  int height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);
  int rowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
  int pal = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);

  int *rows;

  unsigned char *ptr;
  unsigned char **ptrp;

  boolean clamped;

  register int i, j, p;

  if (!unal_inited) init_unal();

  if (weed_plant_has_leaf(layer, WEED_LEAF_YUV_CLAMPING))
    clamped = (weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error) == WEED_YUV_CLAMPING_CLAMPED);
  else clamped = TRUE;

  switch (pal) {
  case WEED_PALETTE_RGBA32:
  case WEED_PALETTE_BGRA32:
    clamped = FALSE;
  case WEED_PALETTE_YUVA8888:
    widthx = width * 4;
    psize = 4;
    psizel = 3;
    coffs = 0;
    aoffs = 3;
    break;
  case WEED_PALETTE_ARGB32:
    widthx = width * 4;
    psize = 4;
    psizel = 4;
    coffs = 1;
    aoffs = 0;
    clamped = FALSE;
    break;
  case WEED_PALETTE_YUVA4444P:
    /// special case - planar with alpha
    ptrp = (unsigned char **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
    rows = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, &error);

    if (!clamped) {
      if (un) {
        for (i = 0; i < height; i++) {
          for (j = 0; j < width; j++) {
            alpha = ptrp[3][j];
            for (p = 0; p < 3; p++) {
              ptrp[p][j] = unal[alpha][ptrp[p][j]];
            }
          }
          for (p = 0; p < 4; p++) {
            ptrp[p] += rows[p];
          }
        }
      } else {
        for (i = 0; i < height; i++) {
          for (j = 0; j < width; j++) {
            alpha = ptrp[3][j];
            for (p = 0; p < 3; p++) {
              ptrp[p][j] = al[alpha][ptrp[p][j]];
            }
          }
          for (p = 0; p < 4; p++) {
            ptrp[p] += rows[p];
          }
        }
      }
    } else {
      if (un) {
        for (i = 0; i < height; i++) {
          for (j = 0; j < width; j++) {
            alpha = ptrp[3][j];
            ptrp[0][j] = unalcy[alpha][ptrp[0][j]];
            ptrp[1][j] = unalcuv[alpha][ptrp[0][j]];
            ptrp[2][j] = unalcuv[alpha][ptrp[0][j]];
          }
          for (p = 0; p < 4; p++) {
            ptrp[p] += rows[p];
          }
        }
      } else {
        for (i = 0; i < height; i++) {
          for (j = 0; j < width; j++) {
            alpha = ptrp[3][j];
            ptrp[0][j] = alcy[alpha][ptrp[0][j]];
            ptrp[1][j] = alcuv[alpha][ptrp[0][j]];
            ptrp[2][j] = alcuv[alpha][ptrp[0][j]];
          }
          for (p = 0; p < 4; p++) {
            ptrp[p] += rows[p];
          }
        }
      }
    }
    return;
  default:
    return;
  }

  ptr = (unsigned char *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);

  if (!clamped) {
    if (un) {
      for (i = 0; i < height; i++) {
        for (j = 0; j < widthx; j += psize) {
          alpha = ptr[j + aoffs];
          for (p = coffs; p < psizel; p++) {
            ptr[j + p] = unal[alpha][ptr[j + p]];
          }
        }
        ptr += rowstride;
      }
    } else {
      for (i = 0; i < height; i++) {
        for (j = 0; j < widthx; j += psize) {
          alpha = ptr[j + aoffs];
          for (p = coffs; p < psizel; p++) {
            ptr[j + p] = al[alpha][ptr[j + p]];
          }
        }
        ptr += rowstride;
      }
    }
  } else {
    /// unclamped YUVA8888 (packed)
    if (un) {
      for (i = 0; i < height; i++) {
        for (j = 0; j < widthx; j += psize) {
          alpha = ptr[j + 3];
          ptr[j] = unalcy[alpha][ptr[j]];
          ptr[j + 1] = unalcuv[alpha][ptr[j]];
          ptr[j + 2] = unalcuv[alpha][ptr[j]];
        }
        ptr += rowstride;
      }
    } else {
      for (i = 0; i < height; i++) {
        for (j = 0; j < widthx; j += psize) {
          alpha = ptr[j + 3];
          ptr[j] = alcy[alpha][ptr[j]];
          ptr[j + 1] = alcuv[alpha][ptr[j]];
          ptr[j + 2] = alcuv[alpha][ptr[j]];
        }
        ptr += rowstride;
      }
    }
  }

  if (weed_plant_has_leaf(layer, WEED_LEAF_FLAGS))
    flags = weed_get_int_value(layer, WEED_LEAF_FLAGS, &error);

  if (!un) flags |= WEED_LAYER_ALPHA_PREMULT;
  else if (flags & WEED_LAYER_ALPHA_PREMULT) flags ^= WEED_LAYER_ALPHA_PREMULT;

  if (flags == 0) weed_leaf_delete(layer, WEED_LEAF_FLAGS);
  else weed_set_int_value(layer, WEED_LEAF_FLAGS, flags);
}


/** @brief convert the palette of a layer

    convert to/from the 5 non-float RGB palettes and 10 YUV palettes
    giving a total of 15*14=210 conversions

    in addition YUV can be converted from clamped to unclamped and vice-versa

    chroma sub and supersampling is implemented, and threading is used wherever possible

    all conversions are performed via lookup tables

    NOTE - if converting to YUV411, we cut pixels so (RGB) width is divisible by 4
    if converting to YUV420 or YVU420, we cut pixels so (RGB) width is divisible by 2
    if converting to YUV420 or YVU420, we cut pixels so height is divisible by 2

    returns FALSE if the palette conversion fails or if layer is NULL

    - original palette pixel_data is free()d (unless converting between YUV420 and YVU420, there the u and v pointers are
                                                                simply swapped).

    current limitations:
    - chroma is assumed centred between luma for input and output
    - bt709 yuv is only implemented for conversions to / from rgb palettes and yuv420 / yvu420
    - rowstride values may be ignored for UYVY, YUYV and YUV411 planar palettes.
    - RGB float palettes not yet implemented

*/
boolean convert_layer_palette_full(weed_layer_t *layer, int outpl, int osamtype, int oclamping, int osubspace) {
  // TODO: allow plugin candidates/delegates
  uint8_t *gusrc = NULL, **gusrc_array = NULL, *gudest = NULL, **gudest_array, *tmp;
  int width, height, orowstride, irowstride, *istrides, *ostrides;
  int error, inpl, flags = 0;
  int isamtype, isubspace;
  int new_gamma_type;
  boolean contig = FALSE;
  int iclamping;
  boolean iclamped, oclamped = (oclamping == WEED_YUV_CLAMPING_CLAMPED);

  if (layer == NULL) return FALSE;

  inpl = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);

  if (weed_plant_has_leaf(layer, WEED_LEAF_YUV_SAMPLING)) isamtype = weed_get_int_value(layer, WEED_LEAF_YUV_SAMPLING, &error);
  else isamtype = WEED_YUV_SAMPLING_DEFAULT;

  if (weed_plant_has_leaf(layer, WEED_LEAF_YUV_CLAMPING))
    iclamping = (weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error));
  else iclamping = oclamping;

  iclamped = (iclamping == WEED_YUV_CLAMPING_CLAMPED);

  if (weed_plant_has_leaf(layer, WEED_LEAF_YUV_SUBSPACE)) isubspace = weed_get_int_value(layer, WEED_LEAF_YUV_SUBSPACE, &error);
  else isubspace = WEED_YUV_SUBSPACE_YUV;

  width = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
  height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);

  //       #define DEBUG_PCONV
#ifdef DEBUG_PCONV
  g_print("converting %d X %d palette %s(%s) to %s(%s)\n", width, height, weed_palette_get_name(inpl),
          weed_yuv_clamping_get_name(iclamping),
          weed_palette_get_name(outpl),
          weed_yuv_clamping_get_name(oclamping));
#endif

  if (weed_palette_is_yuv(inpl) && weed_palette_is_yuv(outpl) && (iclamping != oclamping || isubspace != osubspace)) {
    if (isubspace == osubspace) {
#ifdef DEBUG_PCONV
      lives_printerr("converting clamping %d to %d\n", iclamping, oclamping);
#endif
      switch_yuv_clamping_and_subspace(layer, oclamping, osubspace);
      iclamping = oclamping;
      iclamped = oclamped;
    } else {
      // convert first to RGB(A)
      if (weed_palette_has_alpha_channel(inpl)) {
        convert_layer_palette(layer, WEED_PALETTE_RGBA32, 0);
      } else {
        convert_layer_palette(layer, WEED_PALETTE_RGB24, 0);
      }
      inpl = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);
      isubspace = osubspace;
      isamtype = osamtype;
      iclamping = oclamping;
#ifdef DEBUG_PCONV
      g_print("subspace conversion via palette %s\n", weed_palette_get_name(inpl));
#endif
    }
  }

  if (inpl == outpl) {
#ifdef DEBUG_PCONV
    lives_printerr("not converting palette\n");
#endif
    if (!weed_palette_is_yuv(inpl) || (isamtype == osamtype &&
                                       (isubspace == osubspace || (osubspace != WEED_YUV_SUBSPACE_BT709)))) return TRUE;
    if (inpl == WEED_PALETTE_YUV420P && ((isamtype == WEED_YUV_SAMPLING_JPEG && osamtype == WEED_YUV_SAMPLING_MPEG) ||
                                         (isamtype == WEED_YUV_SAMPLING_MPEG && osamtype == WEED_YUV_SAMPLING_JPEG))) {
      switch_yuv_sampling(layer);
    } else {
      char *tmp2 = lives_strdup_printf("Switch sampling types (%d %d) or subspace(%d %d): (%d) conversion not yet written !\n",
                                       isamtype, osamtype, isubspace, osubspace, inpl);
      LIVES_DEBUG(tmp2);
      lives_free(tmp2);
      return TRUE;
    }
  }

  if (weed_plant_has_leaf(layer, WEED_LEAF_FLAGS))
    flags = weed_get_int_value(layer, WEED_LEAF_FLAGS, &error);

  if (prefs->alpha_post) {
    if ((flags & WEED_LAYER_ALPHA_PREMULT) &&
        (weed_palette_has_alpha_channel(inpl) && !(weed_palette_has_alpha_channel(outpl)))) {
      // if we have pre-multiplied alpha, remove it when removing alpha channel
      alpha_unpremult(layer, TRUE);
    }
  } else {
    if (!weed_palette_has_alpha_channel(inpl) && weed_palette_has_alpha_channel(outpl)) {
      flags |= WEED_LAYER_ALPHA_PREMULT;
      weed_set_int_value(layer, WEED_LEAF_FLAGS, flags);
    }
  }

  if (weed_palette_has_alpha_channel(inpl) && !(weed_palette_has_alpha_channel(outpl)) && (flags & WEED_LAYER_ALPHA_PREMULT)) {
    flags ^= WEED_LAYER_ALPHA_PREMULT;
    if (flags == 0) weed_leaf_delete(layer, WEED_LEAF_FLAGS);
    else weed_set_int_value(layer, WEED_LEAF_FLAGS, flags);
  }

  if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS) &&
      weed_get_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, &error) == WEED_TRUE)
    contig = TRUE;

  width = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
  height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);

  new_gamma_type = get_layer_gamma(layer);
  if (weed_palette_is_rgb(inpl) && !weed_palette_is_rgb(outpl)) {
    if (prefs->apply_gamma) {
      // gamma correction
      if (prefs->btgamma && osubspace == WEED_YUV_SUBSPACE_BT709) {
        gamma_correct_layer(WEED_GAMMA_BT709, layer);
      } else gamma_correct_layer(WEED_GAMMA_SRGB, layer);
    }
    new_gamma_type = weed_get_int_value(layer, WEED_LEAF_GAMMA_TYPE, NULL);
  }

  istrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
  if (istrides == NULL) return FALSE;

  irowstride = istrides[0];
  weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, outpl);

  // TODO: rowstrides for uyvy, yuyv, 422P, 411

  switch (inpl) {
  case WEED_PALETTE_BGR24:
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_RGBA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_swap3addpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_RGB24:
      convert_swap3_frame(gusrc, width, height, irowstride, irowstride, gusrc, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_addpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_swap3addpre_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_bgr_to_uyvy_frame(gusrc, width, height, irowstride, (uyvy_macropixel *)gudest, FALSE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_bgr_to_yuyv_frame(gusrc, width, height, irowstride, (yuyv_macropixel *)gudest, FALSE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_bgr_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, FALSE, FALSE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUVA8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_bgr_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, FALSE, TRUE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_bgr_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, TRUE,
                                  FALSE, WEED_YUV_SAMPLING_DEFAULT, oclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YVU420P:
    case WEED_PALETTE_YUV420P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_bgr_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, FALSE, FALSE, osubspace, oclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_bgr_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, FALSE, FALSE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_bgr_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, FALSE, TRUE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_bgr_to_yuv411_frame(gusrc, width, height, irowstride, (yuv411_macropixel *)gudest, FALSE, oclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    break;
  case WEED_PALETTE_RGBA32:
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_BGR24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_swap3delpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_RGB24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_delpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      convert_swap3postalpha_frame(gusrc, width, height, irowstride, irowstride, gusrc, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      convert_swapprepost_frame(gusrc, width, height, irowstride, irowstride, gusrc, FALSE, -USE_THREADS);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_rgb_to_uyvy_frame(gusrc, width, height, irowstride, (uyvy_macropixel *)gudest, TRUE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_rgb_to_yuyv_frame(gusrc, width, height, irowstride, (yuyv_macropixel *)gudest, TRUE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_rgb_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, TRUE, FALSE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUVA8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_rgb_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, TRUE, TRUE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_rgb_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, TRUE, TRUE,
                                  WEED_YUV_SAMPLING_DEFAULT, oclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV420P:
    case WEED_PALETTE_YVU420P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_rgb_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, FALSE, TRUE, osubspace, oclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_rgb_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, TRUE, FALSE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_rgb_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, TRUE, TRUE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_rgb_to_yuv411_frame(gusrc, width, height, irowstride, (yuv411_macropixel *)gudest, TRUE, oclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    break;
  case WEED_PALETTE_RGB24:
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_BGR24:
      convert_swap3_frame(gusrc, width, height, irowstride, irowstride, gusrc, -USE_THREADS);
      break;
    case WEED_PALETTE_RGBA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_addpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_swap3addpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_addpre_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_rgb_to_uyvy_frame(gusrc, width, height, irowstride, (uyvy_macropixel *)gudest, FALSE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_rgb_to_yuyv_frame(gusrc, width, height, irowstride, (yuyv_macropixel *)gudest, FALSE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_rgb_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, FALSE, FALSE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUVA8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_rgb_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, FALSE, TRUE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_rgb_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, TRUE, FALSE, osubspace, oclamped);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV420P:
    case WEED_PALETTE_YVU420P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_rgb_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, FALSE,
                                  FALSE, WEED_YUV_SAMPLING_DEFAULT, oclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_rgb_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, FALSE, FALSE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_rgb_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, FALSE, TRUE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_rgb_to_yuv411_frame(gusrc, width, height, irowstride, (yuv411_macropixel *)gudest, FALSE, oclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      return FALSE;
    }
    break;
  case WEED_PALETTE_BGRA32:
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_BGR24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_delpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_RGB24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_swap3delpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_RGBA32:
      convert_swap3postalpha_frame(gusrc, width, height, irowstride, irowstride, gusrc, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      convert_swap4_frame(gusrc, width, height, irowstride, irowstride, gusrc, -USE_THREADS);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_bgr_to_uyvy_frame(gusrc, width, height, irowstride, (uyvy_macropixel *)gudest, TRUE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_bgr_to_yuyv_frame(gusrc, width, height, irowstride, (yuyv_macropixel *)gudest, TRUE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_bgr_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, TRUE, FALSE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUVA8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_bgr_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, TRUE, TRUE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_bgr_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, TRUE, TRUE,
                                  WEED_YUV_SAMPLING_DEFAULT, oclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YVU420P:
    case WEED_PALETTE_YUV420P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_bgr_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, FALSE, TRUE, osubspace, oclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_bgr_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, TRUE, FALSE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_bgr_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, TRUE, TRUE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_bgr_to_yuv411_frame(gusrc, width, height, irowstride, (yuv411_macropixel *)gudest, TRUE, oclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    break;
  case WEED_PALETTE_ARGB32:
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_BGR24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_swap3delpre_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_RGB24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_delpre_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_RGBA32:
      convert_swapprepost_frame(gusrc, width, height, irowstride, irowstride, gusrc, TRUE, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      convert_swap4_frame(gusrc, width, height, irowstride, irowstride, gusrc, -USE_THREADS);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_argb_to_uyvy_frame(gusrc, width, height, irowstride, (uyvy_macropixel *)gudest, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_argb_to_yuyv_frame(gusrc, width, height, irowstride, (yuyv_macropixel *)gudest, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_argb_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, FALSE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUVA8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_argb_to_yuv_frame(gusrc, width, height, irowstride, orowstride, gudest, TRUE, oclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_argb_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, FALSE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_argb_to_yuvp_frame(gusrc, width, height, irowstride, orowstride, gudest_array, TRUE, oclamped, -USE_THREADS);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_argb_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, TRUE,
                                   WEED_YUV_SAMPLING_DEFAULT, oclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV420P:
    case WEED_PALETTE_YVU420P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_argb_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, FALSE, osubspace, oclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_argb_to_yuv411_frame(gusrc, width, height, irowstride, (yuv411_macropixel *)gudest, oclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    break;
  case WEED_PALETTE_YUV444P:
    gusrc_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      lives_free(gudest_array[0]);
      gudest_array[0] = gusrc_array[0];
      weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)gudest_array);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_halve_chroma(gusrc_array, width, height, istrides, ostrides, gudest_array, iclamped);
      gusrc_array[0] = NULL;
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_RGB24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_rgb_frame(gusrc_array, width, height, irowstride, orowstride, gudest, FALSE, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_RGBA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_rgb_frame(gusrc_array, width, height, irowstride, orowstride, gudest, FALSE, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGR24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_bgr_frame(gusrc_array, width, height, irowstride, orowstride, gudest, FALSE, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_bgr_frame(gusrc_array, width, height, irowstride, orowstride, gudest, FALSE, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_argb_frame(gusrc_array, width, height, irowstride, orowstride, gudest, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_uyvy_frame(gusrc_array, width, height, irowstride, (uyvy_macropixel *)gudest, iclamped);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_yuyv_frame(gusrc_array, width, height, irowstride, (yuyv_macropixel *)gudest, iclamped);
      break;
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_combineplanes_frame(gusrc_array, width, height, irowstride, orowstride, gudest, FALSE, FALSE);
      break;
    case WEED_PALETTE_YUVA8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_combineplanes_frame(gusrc_array, width, height, irowstride, orowstride, gudest, FALSE, TRUE);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_yuvp_to_yuvap_frame(gusrc_array, width, height, irowstride, orowstride, gudest_array);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV420P:
    case WEED_PALETTE_YVU420P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_yuvp_to_yuv420_frame(gusrc_array, width, height, istrides, ostrides, gudest_array, iclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuvp_to_yuv411_frame(gusrc_array, width, height, irowstride, (yuv411_macropixel *)gudest, iclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      if (gusrc_array != NULL) lives_free(gusrc_array);
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    if (gusrc_array != NULL) {
      if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXBUF_SRC)) {
        LiVESPixbuf *pixbuf = (LiVESPixbuf *)weed_get_voidptr_value(layer, WEED_LEAF_HOST_PIXBUF_SRC, &error);
        weed_leaf_delete(layer, WEED_LEAF_HOST_PIXBUF_SRC);
        lives_widget_object_unref(pixbuf);
      } else {
        if (gusrc_array[0] != NULL) lives_free(gusrc_array[0]);
        if (!contig) {
          lives_free(gusrc_array[1]);
          lives_free(gusrc_array[2]);
        }
      }
      lives_free(gusrc_array);
    }
    break;
  case WEED_PALETTE_YUVA4444P:
    gusrc_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      lives_free(gudest_array[0]);
      gudest_array[0] = gusrc_array[0];
      weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)gudest_array);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_halve_chroma(gusrc_array, width, height, istrides, ostrides, gudest_array, iclamped);
      gusrc_array[0] = NULL;
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_RGB24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_rgb_frame(gusrc_array, width, height, irowstride, orowstride, gudest, TRUE, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_RGBA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_rgb_frame(gusrc_array, width, height, irowstride, orowstride, gudest, TRUE, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGR24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_bgr_frame(gusrc_array, width, height, irowstride, orowstride, gudest, TRUE, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_bgr_frame(gusrc_array, width, height, irowstride, orowstride, gudest, TRUE, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_argb_frame(gusrc_array, width, height, irowstride, orowstride, gudest, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_uyvy_frame(gusrc_array, width, height, irowstride, (uyvy_macropixel *)gudest, iclamped);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv_planar_to_yuyv_frame(gusrc_array, width, height, irowstride, (yuyv_macropixel *)gudest, iclamped);
      break;
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_combineplanes_frame(gusrc_array, width, height, irowstride, orowstride, gudest, TRUE, FALSE);
      break;
    case WEED_PALETTE_YUVA8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_combineplanes_frame(gusrc_array, width, height, irowstride, orowstride, gudest, TRUE, TRUE);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_yuvap_to_yuvp_frame(gusrc_array, width, height, irowstride, orowstride, gudest_array);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV420P:
    case WEED_PALETTE_YVU420P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_yuvp_to_yuv420_frame(gusrc_array, width, height, istrides, ostrides, gudest_array, iclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuvp_to_yuv411_frame(gusrc_array, width, height, irowstride, (yuv411_macropixel *)gudest, iclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      if (gusrc_array != NULL) lives_free(gusrc_array);
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    if (gusrc_array != NULL) {
      if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXBUF_SRC)) {
        LiVESPixbuf *pixbuf = (LiVESPixbuf *)weed_get_voidptr_value(layer, WEED_LEAF_HOST_PIXBUF_SRC, &error);
        weed_leaf_delete(layer, WEED_LEAF_HOST_PIXBUF_SRC);
        lives_widget_object_unref(pixbuf);
      } else {
        if (gusrc_array[0] != NULL) lives_free(gusrc_array[0]);
        if (!contig) {
          lives_free(gusrc_array[1]);
          lives_free(gusrc_array[2]);
          lives_free(gusrc_array[3]);
        }
      }
      lives_free(gusrc_array);
    }
    break;
  case WEED_PALETTE_UYVY8888:
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_YUYV8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_swab_frame(gusrc, width, height, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV422P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_uyvy_to_yuv422_frame((uyvy_macropixel *)gusrc, width, height, gudest_array);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_RGB24:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_uyvy_to_rgb_frame((uyvy_macropixel *)gusrc, width, height, orowstride, gudest, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_RGBA32:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_uyvy_to_rgb_frame((uyvy_macropixel *)gusrc, width, height, orowstride, gudest, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGR24:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_uyvy_to_bgr_frame((uyvy_macropixel *)gusrc, width, height, orowstride, gudest, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_uyvy_to_bgr_frame((uyvy_macropixel *)gusrc, width, height, orowstride, gudest, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_uyvy_to_argb_frame((uyvy_macropixel *)gusrc, width, height, orowstride, gudest, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV444P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_uyvy_to_yuvp_frame((uyvy_macropixel *)gusrc, width, height, orowstride, gudest_array, FALSE);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_uyvy_to_yuvp_frame((uyvy_macropixel *)gusrc, width, height, orowstride, gudest_array, TRUE);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_uyvy_to_yuv888_frame((uyvy_macropixel *)gusrc, width, height, gudest, FALSE);
      break;
    case WEED_PALETTE_YUVA8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_uyvy_to_yuv888_frame((uyvy_macropixel *)gusrc, width, height, gudest, TRUE);
      break;
    case WEED_PALETTE_YUV420P:
    case WEED_PALETTE_YVU420P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_uyvy_to_yuv420_frame((uyvy_macropixel *)gusrc, width, height, gudest_array, iclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_uyvy_to_yuv411_frame((uyvy_macropixel *)gusrc, width, height, (yuv411_macropixel *)gudest, iclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    break;
  case WEED_PALETTE_YUYV8888:
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_UYVY8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_swab_frame(gusrc, width, height, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV422P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuyv_to_yuv422_frame((yuyv_macropixel *)gusrc, width, height, gudest_array);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_RGB24:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_yuyv_to_rgb_frame((yuyv_macropixel *)gusrc, width, height, orowstride, gudest, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_RGBA32:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_yuyv_to_rgb_frame((yuyv_macropixel *)gusrc, width, height, orowstride, gudest, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGR24:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_yuyv_to_bgr_frame((yuyv_macropixel *)gusrc, width, height, orowstride, gudest, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_yuyv_to_bgr_frame((yuyv_macropixel *)gusrc, width, height, orowstride, gudest, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_yuyv_to_argb_frame((yuyv_macropixel *)gusrc, width, height, orowstride, gudest, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV444P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuyv_to_yuvp_frame((yuyv_macropixel *)gusrc, width, height, gudest_array, FALSE);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuyv_to_yuvp_frame((yuyv_macropixel *)gusrc, width, height, gudest_array, TRUE);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuyv_to_yuv888_frame((yuyv_macropixel *)gusrc, width, height, gudest, FALSE);
      break;
    case WEED_PALETTE_YUVA8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuyv_to_yuv888_frame((yuyv_macropixel *)gusrc, width, height, gudest, TRUE);
      break;
    case WEED_PALETTE_YUV420P:
    case WEED_PALETTE_YVU420P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuyv_to_yuv420_frame((yuyv_macropixel *)gusrc, width, height, gudest_array, iclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuyv_to_yuv411_frame((yuyv_macropixel *)gusrc, width, height, (yuv411_macropixel *)gudest, iclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    break;
  case WEED_PALETTE_YUV888:
    // need to check rowstrides (may have been resized)
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_YUVA8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_addpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_splitplanes_frame(gusrc, width, height, irowstride, orowstride, gudest_array, FALSE, FALSE);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_splitplanes_frame(gusrc, width, height, irowstride, orowstride, gudest_array, FALSE, TRUE);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_RGB24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_rgb_frame(gusrc, width, height, irowstride, orowstride, gudest, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_RGBA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_rgb_frame(gusrc, width, height, irowstride, orowstride, gudest, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGR24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_bgr_frame(gusrc, width, height, irowstride, orowstride, gudest, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_bgr_frame(gusrc, width, height, irowstride, orowstride, gudest, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_argb_frame(gusrc, width, height, irowstride, orowstride, gudest, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YVU420P:
    // convert to YUV420P, then fall through
    case WEED_PALETTE_YUV420P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_yuv888_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, FALSE, iclamped);
      weed_free(ostrides);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      //weed_set_int_value(layer,WEED_LEAF_YUV_SAMPLING,osamtype);
      break;
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_yuv888_to_yuv422_frame(gusrc, width, height, irowstride, ostrides, gudest_array, FALSE, iclamped);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_uyvy_frame(gusrc, width, height, irowstride, (uyvy_macropixel *)gudest, FALSE, iclamped);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_yuyv_frame(gusrc, width, height, irowstride, (yuyv_macropixel *)gudest, FALSE, iclamped);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_yuv411_frame(gusrc, width, height, irowstride, (yuv411_macropixel *)gudest, FALSE);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    break;
  case WEED_PALETTE_YUVA8888:
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_delpost_frame(gusrc, width, height, irowstride, orowstride, gudest, -USE_THREADS);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_splitplanes_frame(gusrc, width, height, irowstride, orowstride, gudest_array, TRUE, TRUE);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_splitplanes_frame(gusrc, width, height, irowstride, orowstride, gudest_array, TRUE, FALSE);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_RGB24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuva8888_to_rgba_frame(gusrc, width, height, irowstride, orowstride, gudest, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_RGBA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuva8888_to_rgba_frame(gusrc, width, height, irowstride, orowstride, gudest, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGR24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuva8888_to_bgra_frame(gusrc, width, height, irowstride, orowstride, gudest, TRUE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_BGRA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuva8888_to_bgra_frame(gusrc, width, height, irowstride, orowstride, gudest, FALSE, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_ARGB32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuva8888_to_argb_frame(gusrc, width, height, irowstride, orowstride, gudest, iclamped, -USE_THREADS);
      break;
    case WEED_PALETTE_YUV420P:
    case WEED_PALETTE_YVU420P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_yuv888_to_yuv420_frame(gusrc, width, height, irowstride, ostrides, gudest_array, TRUE, iclamped);
      lives_free(gudest_array);
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
      //weed_set_int_value(layer,WEED_LEAF_YUV_SAMPLING,osamtype);
      break;
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_yuv888_to_yuv422_frame(gusrc, width, height, irowstride, ostrides, gudest_array, TRUE, iclamped);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_yuv888_to_uyvy_frame(gusrc, width, height, irowstride, (uyvy_macropixel *)gudest, TRUE, iclamped);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_yuyv_frame(gusrc, width, height, irowstride, (yuyv_macropixel *)gudest, TRUE, iclamped);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv888_to_yuv411_frame(gusrc, width, height, irowstride, (yuv411_macropixel *)gudest, TRUE);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    break;
  case WEED_PALETTE_YVU420P:
    // swap u and v planes, then fall through to YUV420P
    gusrc_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
    tmp = gusrc_array[1];
    gusrc_array[1] = gusrc_array[2];
    gusrc_array[2] = tmp;
    weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)gusrc_array);
    lives_free(gusrc_array);
  case WEED_PALETTE_YUV420P:
    gusrc_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_RGB24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_rgb_frame(gusrc_array, width, height, istrides, orowstride, gudest, FALSE, FALSE, isubspace, iclamped);
      break;
    case WEED_PALETTE_RGBA32:
      g_print("b4 width = %d\n", weed_layer_get_width(layer));
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_rgb_frame(gusrc_array, width, height, istrides, orowstride, gudest, TRUE, FALSE, isubspace, iclamped);
      g_print("af width = %d\n", weed_layer_get_width(layer));
      break;
    case WEED_PALETTE_BGR24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_bgr_frame(gusrc_array, width, height, istrides, orowstride, gudest, FALSE, FALSE, isubspace, iclamped);
      break;
    case WEED_PALETTE_BGRA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_bgr_frame(gusrc_array, width, height, istrides, orowstride, gudest, TRUE, FALSE, isubspace, iclamped);
      break;
    case WEED_PALETTE_ARGB32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_argb_frame(gusrc_array, width, height, istrides, orowstride, gudest, FALSE, isubspace, iclamped);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420_to_uyvy_frame(gusrc_array, width, height, (uyvy_macropixel *)gudest, iclamped);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420_to_yuyv_frame(gusrc_array, width, height, (yuyv_macropixel *)gudest, iclamped);
      break;
    case WEED_PALETTE_YUV422P:
      create_empty_pixel_data(layer, FALSE, FALSE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      lives_free(gudest_array[0]);
      gudest_array[0] = gusrc_array[0];
      weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)gudest_array);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_double_chroma(gusrc_array, width >> 1, height >> 1, istrides, ostrides, gudest_array, iclamped);
      gusrc_array[0] = NULL;
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, FALSE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      lives_free(gudest_array[0]);
      gudest_array[0] = gusrc_array[0];
      weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)gudest_array);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_quad_chroma(gusrc_array, width, height, istrides, orowstride, gudest_array, FALSE, iclamped);
      gusrc_array[0] = NULL;
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, FALSE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      lives_free(gudest_array[0]);
      gudest_array[0] = gusrc_array[0];
      weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 4, (void **)gudest_array);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_quad_chroma(gusrc_array, width, height, istrides, orowstride, gudest_array, TRUE, iclamped);
      gusrc_array[0] = NULL;
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YVU420P:
      if (inpl == WEED_PALETTE_YUV420P) {
        tmp = gusrc_array[1];
        gusrc_array[1] = gusrc_array[2];
        gusrc_array[2] = tmp;
        weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)gusrc_array);
        lives_free(gusrc_array);
      }
    // fall through
    case WEED_PALETTE_YUV420P:
      lives_free(gusrc_array);
      gusrc_array = NULL;
      break;
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_quad_chroma_packed(gusrc_array, width, height, istrides, orowstride,
                                 gudest, FALSE, iclamped);
      break;
    case WEED_PALETTE_YUVA8888:
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, outpl);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_quad_chroma_packed(gusrc_array, width, height, istrides, orowstride,
                                 gudest, TRUE, iclamped);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, outpl);
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420_to_yuv411_frame(gusrc_array, width, height, (yuv411_macropixel *)gudest, FALSE, iclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      if (gusrc_array != NULL) lives_free(gusrc_array);
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    if (gusrc_array != NULL) {
      if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXBUF_SRC)) {
        LiVESPixbuf *pixbuf = (LiVESPixbuf *)weed_get_voidptr_value(layer, WEED_LEAF_HOST_PIXBUF_SRC, &error);
        weed_leaf_delete(layer, WEED_LEAF_HOST_PIXBUF_SRC);
        lives_widget_object_unref(pixbuf);
      } else {
        if (gusrc_array[0] != NULL) lives_free(gusrc_array[0]);
        if (!contig) {
          lives_free(gusrc_array[1]);
          lives_free(gusrc_array[2]);
        }
      }
      lives_free(gusrc_array);
    }
    break;
  case WEED_PALETTE_YUV422P:
    gusrc_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_RGB24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_rgb_frame(gusrc_array, width, height, istrides, orowstride, gudest, FALSE, TRUE, isamtype, iclamped);
      break;
    case WEED_PALETTE_RGBA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_rgb_frame(gusrc_array, width, height, istrides, orowstride, gudest, TRUE, TRUE, isamtype, iclamped);
      break;
    case WEED_PALETTE_BGR24:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_bgr_frame(gusrc_array, width, height, istrides, orowstride, gudest, FALSE, TRUE, isamtype, iclamped);
      break;
    case WEED_PALETTE_BGRA32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_bgr_frame(gusrc_array, width, height, istrides, orowstride, gudest, TRUE, TRUE, isamtype, iclamped);
      break;
    case WEED_PALETTE_ARGB32:
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420p_to_argb_frame(gusrc_array, width, height, istrides, orowstride, gudest, TRUE, isamtype, iclamped);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv422p_to_uyvy_frame(gusrc_array, width, height, gudest);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv422p_to_yuyv_frame(gusrc_array, width, height, gudest);
      break;
    case WEED_PALETTE_YUV420P:
    case WEED_PALETTE_YVU420P:
      create_empty_pixel_data(layer, FALSE, FALSE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      lives_free(gudest_array[0]);
      gudest_array[0] = gusrc_array[0];
      weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)gudest_array);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_halve_chroma(gusrc_array, width >> 1, height >> 1, istrides, ostrides, gudest_array, iclamped);
      lives_free(gudest_array);
      gusrc_array[0] = NULL;
      weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, isamtype);
      break;
    case WEED_PALETTE_YUV444P:
      create_empty_pixel_data(layer, FALSE, FALSE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      lives_free(gudest_array[0]);
      gudest_array[0] = gusrc_array[0];
      weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)gudest_array);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_double_chroma(gusrc_array, width >> 1, height >> 1, istrides, ostrides, gudest_array, iclamped);
      gusrc_array[0] = NULL;
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      create_empty_pixel_data(layer, FALSE, FALSE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      lives_free(gudest_array[0]);
      gudest_array[0] = gusrc_array[0];
      weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 4, (void **)gudest_array);
      ostrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);
      convert_double_chroma(gusrc_array, width >> 1, height >> 1, istrides, ostrides, gudest_array, iclamped);
      lives_memset(gudest_array[3], 255, width * height);
      gusrc_array[0] = NULL;
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_double_chroma_packed(gusrc_array, width, height, istrides, orowstride, gudest, FALSE, iclamped);
      break;
    case WEED_PALETTE_YUVA8888:
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      convert_double_chroma_packed(gusrc_array, width, height, istrides, orowstride, gudest, TRUE, iclamped);
      break;
    case WEED_PALETTE_YUV411:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width >> 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv420_to_yuv411_frame(gusrc_array, width, height, (yuv411_macropixel *)gudest, TRUE, iclamped);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      if (gusrc_array != NULL) lives_free(gusrc_array);
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    if (gusrc_array != NULL) {
      if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXBUF_SRC)) {
        LiVESPixbuf *pixbuf = (LiVESPixbuf *)weed_get_voidptr_value(layer, WEED_LEAF_HOST_PIXBUF_SRC, &error);
        weed_leaf_delete(layer, WEED_LEAF_HOST_PIXBUF_SRC);
        lives_widget_object_unref(pixbuf);
      } else {
        if (gusrc_array[0] != NULL) lives_free(gusrc_array[0]);
        if (!contig) {
          lives_free(gusrc_array[1]);
          lives_free(gusrc_array[2]);
        }
      }
      lives_free(gusrc_array);
    }
    break;
  case WEED_PALETTE_YUV411:
    gusrc = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    switch (outpl) {
    case WEED_PALETTE_RGB24:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_rgb_frame((yuv411_macropixel *)gusrc, width, height, orowstride, gudest, FALSE, iclamped);
      break;
    case WEED_PALETTE_RGBA32:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_rgb_frame((yuv411_macropixel *)gusrc, width, height, orowstride, gudest, TRUE, iclamped);
      break;
    case WEED_PALETTE_BGR24:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_bgr_frame((yuv411_macropixel *)gusrc, width, height, orowstride, gudest, FALSE, iclamped);
      break;
    case WEED_PALETTE_BGRA32:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_bgr_frame((yuv411_macropixel *)gusrc, width, height, orowstride, gudest, TRUE, iclamped);
      break;
    case WEED_PALETTE_ARGB32:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_argb_frame((yuv411_macropixel *)gusrc, width, height, orowstride, gudest, iclamped);
      break;
    case WEED_PALETTE_YUV888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_yuv888_frame((yuv411_macropixel *)gusrc, width, height, gudest, FALSE, iclamped);
      break;
    case WEED_PALETTE_YUVA8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_yuv888_frame((yuv411_macropixel *)gusrc, width, height, gudest, TRUE, iclamped);
      break;
    case WEED_PALETTE_YUV444P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_yuvp_frame((yuv411_macropixel *)gusrc, width, height, gudest_array, FALSE, iclamped);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUVA4444P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_yuvp_frame((yuv411_macropixel *)gusrc, width, height, gudest_array, TRUE, iclamped);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_UYVY8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_uyvy_frame((yuv411_macropixel *)gusrc, width, height, (uyvy_macropixel *)gudest, iclamped);
      break;
    case WEED_PALETTE_YUYV8888:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 1);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_yuyv_frame((yuv411_macropixel *)gusrc, width, height, (yuyv_macropixel *)gudest, iclamped);
      break;
    case WEED_PALETTE_YUV422P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_yuv422_frame((yuv411_macropixel *)gusrc, width, height, gudest_array, iclamped);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YUV420P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_yuv420_frame((yuv411_macropixel *)gusrc, width, height, gudest_array, FALSE, iclamped);
      lives_free(gudest_array);
      break;
    case WEED_PALETTE_YVU420P:
      weed_set_int_value(layer, WEED_LEAF_WIDTH, width << 2);
      create_empty_pixel_data(layer, FALSE, TRUE);
      gudest_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
      convert_yuv411_to_yuv420_frame((yuv411_macropixel *)gusrc, width, height, gudest_array, TRUE, iclamped);
      lives_free(gudest_array);
      break;
    default:
      lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                     weed_palette_get_name(outpl));
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, inpl);
      return FALSE;
    }
    break;
  default:
    lives_printerr("Invalid palette conversion: %s to %s not written yet !!\n", weed_palette_get_name(inpl),
                   weed_palette_get_name(outpl));
    return FALSE;
  }
  if (gusrc != NULL && gudest != NULL) {
    if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXBUF_SRC)) {
      LiVESPixbuf *pixbuf = (LiVESPixbuf *)weed_get_voidptr_value(layer, WEED_LEAF_HOST_PIXBUF_SRC, &error);
      weed_leaf_delete(layer, WEED_LEAF_HOST_PIXBUF_SRC);
      lives_widget_object_unref(pixbuf);
    } else {
      lives_free(gusrc);
    }
  }

  if (weed_palette_is_rgb(outpl)) {
    weed_leaf_delete(layer, WEED_LEAF_YUV_CLAMPING);
    weed_leaf_delete(layer, WEED_LEAF_YUV_SUBSPACE);
    weed_leaf_delete(layer, WEED_LEAF_YUV_SAMPLING);
  } else {
    weed_set_int_value(layer, WEED_LEAF_YUV_CLAMPING, oclamped ? WEED_YUV_CLAMPING_CLAMPED
                       : WEED_YUV_CLAMPING_UNCLAMPED);
    if (weed_palette_is_rgb(inpl)) {
      // TODO - bt709
      weed_set_int_value(layer, WEED_LEAF_YUV_SUBSPACE, WEED_YUV_SUBSPACE_YCBCR);
    }
    if (!weed_plant_has_leaf(layer, WEED_LEAF_YUV_SAMPLING)) weed_set_int_value(layer,
          WEED_LEAF_YUV_SAMPLING, WEED_YUV_SAMPLING_DEFAULT);
  }

  if (weed_palette_is_rgb(inpl) && weed_palette_is_yuv(outpl)) {
    width = ((weed_get_int_value(layer, WEED_LEAF_WIDTH, &error) * weed_palette_get_pixels_per_macropixel(outpl)) >> 1) << 1;
    weed_set_int_value(layer, WEED_LEAF_WIDTH, width / weed_palette_get_pixels_per_macropixel(outpl));
  }

  if ((outpl == WEED_PALETTE_YVU420P && inpl != WEED_PALETTE_YVU420P && inpl != WEED_PALETTE_YUV420P)) {
    // swap u and v planes
    uint8_t **pd_array = (uint8_t **)weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
    uint8_t *tmp = pd_array[1];
    pd_array[1] = pd_array[2];
    pd_array[2] = tmp;
    weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, 3, (void **)pd_array);
    lives_free(pd_array);
  }

  weed_set_int_value(layer, WEED_LEAF_GAMMA_TYPE, new_gamma_type); // after conversion

  return TRUE;
}


boolean convert_layer_palette(weed_layer_t *layer, int outpl, int op_clamping) {
  return convert_layer_palette_full(layer, outpl, WEED_YUV_SAMPLING_DEFAULT, op_clamping, WEED_YUV_SUBSPACE_YUV);
}

/////////////////////////////////////////////////////////////////////////////////////


LiVESPixbuf *lives_pixbuf_new_blank(int width, int height, int palette) {
  LiVESPixbuf *pixbuf;
  int rowstride;
  uint8_t *pixels;
  size_t size;

  switch (palette) {
  case WEED_PALETTE_RGB24:
  case WEED_PALETTE_BGR24:
    pixbuf = lives_pixbuf_new(FALSE, width, height);
    rowstride = lives_pixbuf_get_rowstride(pixbuf);
    pixels = lives_pixbuf_get_pixels(pixbuf);
    size = rowstride * (height - 1) + get_last_pixbuf_rowstride_value(width, 3);
    lives_memset(pixels, 0, size);
    break;
  case WEED_PALETTE_RGBA32:
  case WEED_PALETTE_BGRA32:
    pixbuf = lives_pixbuf_new(TRUE, width, height);
    rowstride = lives_pixbuf_get_rowstride(pixbuf);
    pixels = lives_pixbuf_get_pixels(pixbuf);
    size = rowstride * (height - 1) + get_last_pixbuf_rowstride_value(width, 4);
    lives_memset(pixels, 0, size);
    break;
  case WEED_PALETTE_ARGB32:
    pixbuf = lives_pixbuf_new(TRUE, width, height);
    rowstride = lives_pixbuf_get_rowstride(pixbuf);
    pixels = lives_pixbuf_get_pixels(pixbuf);
    size = rowstride * (height - 1) + get_last_pixbuf_rowstride_value(width, 4);
    lives_memset(pixels, 0, size);
    break;
  default:
    return NULL;
  }
  return pixbuf;
}


LIVES_INLINE LiVESPixbuf *lives_pixbuf_cheat(boolean has_alpha, int width, int height, uint8_t *buf) {
  // we can cheat if our buffer is correctly sized
  LiVESPixbuf *pixbuf;
  int channels = has_alpha ? 4 : 3;
  int rowstride = get_pixbuf_rowstride_value(width * channels);

  pixbuf = lives_pixbuf_new_from_data(buf, has_alpha, width, height, rowstride,
                                      (LiVESPixbufDestroyNotify)lives_free_buffer, NULL);
  threaded_dialog_spin(0.);
  return pixbuf;
}


int get_layer_gamma(weed_layer_t *layer) {
  int gamma_type = WEED_GAMMA_UNKNOWN;
  int error;
  if (prefs->apply_gamma) {
    if (weed_plant_has_leaf(layer, WEED_LEAF_GAMMA_TYPE)) {
      gamma_type = weed_get_int_value(layer, WEED_LEAF_GAMMA_TYPE, &error);
    }
    if (gamma_type == WEED_GAMMA_UNKNOWN) {
      break_me();
      LIVES_WARN("Layer with unknown gamma !!");
      gamma_type = WEED_GAMMA_SRGB;
    }
  }
  return gamma_type;
}


void gamma_conv_params(int gamma_type, weed_layer_t *inst, boolean is_in) {
  if (!prefs->apply_gamma) return;
  else {
    // convert colour param values to gamma_type (only integer values)
    weed_layer_t **params;
    weed_layer_t *ptmpl, *param;

    const char *type = is_in ? WEED_LEAF_IN_PARAMETERS : WEED_LEAF_OUT_PARAMETERS;

    boolean fwd = TRUE;

    int *ivals;
    int ogamma_type;
    int phint, pcspace, ptype, nvals, qvals;
    int nparms, i, j, k, error;

    if (inst == NULL || !weed_plant_has_leaf(inst, type) || (nparms = weed_leaf_num_elements(inst, type)) == 0) return;

    if (gamma_type == WEED_GAMMA_LINEAR) fwd = FALSE;

    params = weed_get_plantptr_array(inst, type, &error);

    for (i = 0; i < nparms; i++) {
      param = params[i];
      if (!weed_plant_has_leaf(param, WEED_LEAF_VALUE) || (nvals = weed_leaf_num_elements(param, WEED_LEAF_VALUE)) == 0) continue;
      ptmpl = weed_get_plantptr_value(param, WEED_LEAF_TEMPLATE, &error);
      phint = weed_get_int_value(ptmpl, WEED_LEAF_HINT, &error);
      if (phint != WEED_HINT_COLOR) continue;

      ptype = weed_leaf_seed_type(ptmpl, WEED_LEAF_DEFAULT);

      if (ptype != WEED_SEED_INT) gamma_type = WEED_GAMMA_SRGB;

      if (!prefs->apply_gamma || !weed_plant_has_leaf(param, WEED_LEAF_GAMMA_TYPE)) {
        ogamma_type = WEED_GAMMA_SRGB;
      } else {
        ogamma_type = weed_get_int_value(param, WEED_LEAF_GAMMA_TYPE, &error);
      }

      weed_set_int_value(param, WEED_LEAF_GAMMA_TYPE, gamma_type);
      weed_leaf_set_flags(param, WEED_LEAF_GAMMA_TYPE, (weed_leaf_get_flags(param, WEED_LEAF_GAMMA_TYPE) |
                          WEED_FLAG_IMMUTABLE | WEED_FLAG_UNDELETABLE));

      // no change needed
      if (gamma_type == ogamma_type) continue;

      qvals = 3;
      pcspace = weed_get_int_value(ptmpl, WEED_LEAF_COLORSPACE, &error);
      if (pcspace == WEED_COLORSPACE_RGBA) qvals = 4;
      ivals = weed_get_int_array(param, WEED_LEAF_VALUE, &error);
      pthread_mutex_lock(&mainw->gamma_lut_mutex);
      if ((fwd && (current_gamma_from != WEED_GAMMA_LINEAR || current_gamma_to != WEED_GAMMA_SRGB)) ||
          (!fwd && (current_gamma_from != WEED_GAMMA_SRGB || current_gamma_to != WEED_GAMMA_LINEAR)))
        update_gamma_lut(fwd, WEED_GAMMA_SRGB);
      for (j = 0; j < nvals; j += qvals) {
        for (k = 0; k < 3; k++) {
          ivals[j + k] = gamma_lut[ivals[j + k]];
        }
      }
      pthread_mutex_unlock(&mainw->gamma_lut_mutex);
      weed_set_int_array(param, WEED_LEAF_VALUE, nvals, ivals);
      lives_free(ivals);
      weed_set_int_value(param, WEED_LEAF_GAMMA_TYPE, gamma_type);
      weed_leaf_set_flags(param, WEED_LEAF_GAMMA_TYPE, (weed_leaf_get_flags(param, WEED_LEAF_GAMMA_TYPE) |
                          WEED_FLAG_IMMUTABLE | WEED_FLAG_UNDELETABLE));
    }

    lives_free(params);
  }
}


boolean gamma_correct_layer(int gamma_type, weed_layer_t *layer) {
  if (!prefs->apply_gamma) return TRUE;
  else {
    // convert layer from current gamma to target
    register int j, k;

    uint8_t *pixels, *end;
    int widthx;
    int error;
    int start = 0;
    int lgamma_type = get_layer_gamma(layer);

    int orowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);
    int pal = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error), nchannels;
    int width = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
    int height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);

    if (!weed_palette_is_rgb(pal)) return FALSE; //  dont know how to convert in yuv space

    if (gamma_type == lgamma_type) return TRUE;

    nchannels = weed_palette_get_bits_per_macropixel(pal) >> 3;
    widthx = width * nchannels;

    pixels = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    end = pixels + height * orowstride;

    if (pal == WEED_PALETTE_ARGB32) start = 1;

    pthread_mutex_lock(&mainw->gamma_lut_mutex);
    if (current_gamma_from != lgamma_type || current_gamma_to != gamma_type)
      update_gamma_lut(lgamma_type, gamma_type);
    for (; pixels < end; pixels += orowstride) {
      for (j = start; j < widthx; j += nchannels) {
        for (k = 0; k < 3; k++) pixels[j + k] = gamma_lut[pixels[j + k]];
      }
    }
    pthread_mutex_unlock(&mainw->gamma_lut_mutex);
    weed_set_int_value(layer, WEED_LEAF_GAMMA_TYPE, gamma_type);
    return TRUE;
  }
}


LiVESPixbuf *layer_to_pixbuf(weed_layer_t *layer, boolean realpalette) {
  // create a gdkpixbuf from a weed layer
  // layer "pixel_data" is then either copied to the pixbuf pixels, or the contents shared with the pixbuf and array value set to NULL

  LiVESPixbuf *pixbuf;

  uint8_t *pixel_data, *pixels, *end;

  boolean cheat = FALSE, done;

  weed_error_t error;
  int palette;
  int width;
  int height;
  int irowstride;
  int rowstride, orowstride;
  int n_channels;
  int gamma_type;

  if (layer == NULL) return NULL;

  palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);

  if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXBUF_SRC) && (!realpalette || weed_palette_is_pixbuf_palette(palette))) {
    // our layer pixel_data originally came from a pixbuf, so just free the layer and return the pixbuf
    pixbuf = (LiVESPixbuf *)weed_get_voidptr_value(layer, WEED_LEAF_HOST_PIXBUF_SRC, &error);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, NULL);
    weed_leaf_delete(layer, WEED_LEAF_HOST_PIXBUF_SRC);
    return pixbuf;
  }

  if (realpalette) {
    // force conversion to RGB24 or RGBA32 (+ gamma)
    palette = WEED_PALETTE_END;
  }

  // otherwise we need to steal or copy the pixel_data

  do {
    width = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
    height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);
    irowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, &error);

    pixel_data = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, &error);
    done = TRUE;
    switch (palette) {
    case WEED_PALETTE_RGB24:
    case WEED_PALETTE_BGR24:
    case WEED_PALETTE_YUV888:
#ifndef GUI_QT
      if (irowstride == get_pixbuf_rowstride_value(width * 3)) {
        // rowstrides are OK, we can just steal the pixel_data
        pixbuf = lives_pixbuf_cheat(FALSE, width, height, pixel_data);
        weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, NULL);
        cheat = TRUE;
      } else
#endif
        // otherwise we need to copy the data
        pixbuf = lives_pixbuf_new(FALSE, width, height);
      n_channels = 3;
      break;
    case WEED_PALETTE_RGBA32:
    case WEED_PALETTE_BGRA32:
#ifdef USE_SWSCALE
    case WEED_PALETTE_ARGB32:
#else
#ifdef GUI_QT
    case WEED_PALETTE_ARGB32:
#endif
#endif
    case WEED_PALETTE_YUVA8888:
#ifndef GUI_QT
      if (irowstride == get_pixbuf_rowstride_value(width * 4)) {
        // rowstrides are OK, we can just steal the pixel_data
        pixbuf = lives_pixbuf_cheat(TRUE, width, height, pixel_data);
        weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, NULL);
        cheat = TRUE;
      } else
#endif
        // otherwise we need to copy the data
        pixbuf = lives_pixbuf_new(TRUE, width, height);
      n_channels = 4;
      break;
    default:
      if (weed_palette_has_alpha_channel(palette)) {
        if (!convert_layer_palette(layer, WEED_PALETTE_RGBA32, 0)) return NULL;
        palette = WEED_PALETTE_RGBA32;
      } else {
        if (!convert_layer_palette(layer, WEED_PALETTE_RGB24, 0)) return NULL;
        palette = WEED_PALETTE_RGB24;
      }
      gamma_type = weed_get_int_value(layer, WEED_LEAF_GAMMA_TYPE, NULL);
      if (gamma_type != WEED_GAMMA_UNKNOWN) gamma_correct_layer(gamma_type, layer);
      done = FALSE;
    }
  } while (!done);

  if (!cheat && LIVES_IS_PIXBUF(pixbuf)) {
    // copy the pixel data
    boolean done = FALSE;
    pixels = lives_pixbuf_get_pixels(pixbuf);
    orowstride = lives_pixbuf_get_rowstride(pixbuf);

    if (irowstride > orowstride) rowstride = orowstride;
    else rowstride = irowstride;
    end = pixels + orowstride * height;

    for (; pixels < end && !done; pixels += orowstride) {
      if (pixels + orowstride >= end) {
        orowstride = rowstride = get_last_pixbuf_rowstride_value(width, n_channels);
        done = TRUE;
      }
      lives_memcpy(pixels, pixel_data, rowstride);
      if (rowstride < orowstride) lives_memset(pixels + rowstride, 0, orowstride - rowstride);
      pixel_data += irowstride;
    }
    weed_layer_pixel_data_free(layer);
  }

  return pixbuf;
}


LIVES_INLINE boolean weed_palette_is_resizable(int pal, int clamped, boolean in_out) {
  // in_out is TRUE for input, FALSE for output

  // in future we may also have resize candidates/delegates for other palettes
  // we will need to check for these

  if (pal == WEED_PALETTE_YUV888 && clamped == WEED_YUV_CLAMPING_UNCLAMPED) pal = WEED_PALETTE_RGB24;
  if (pal == WEED_PALETTE_YUVA8888 && clamped == WEED_YUV_CLAMPING_UNCLAMPED) pal = WEED_PALETTE_RGBA32;

#ifdef USE_SWSCALE
  if (in_out && sws_isSupportedInput(weed_palette_to_avi_pix_fmt(pal, &clamped))) return TRUE;
  else if (sws_isSupportedOutput(weed_palette_to_avi_pix_fmt(pal, &clamped))) return TRUE;
#endif
  if (pal == WEED_PALETTE_RGB24 || pal == WEED_PALETTE_RGBA32 || pal == WEED_PALETTE_BGR24 ||
      pal == WEED_PALETTE_BGRA32) return TRUE;
  return FALSE;
}


void lives_pixbuf_set_opaque(LiVESPixbuf *pixbuf) {
  unsigned char *pdata = lives_pixbuf_get_pixels(pixbuf);
  int row = lives_pixbuf_get_rowstride(pixbuf);
  int height = lives_pixbuf_get_height(pixbuf);
  int offs;
#ifdef GUI_GTK
  offs = 3;
#endif

#ifdef GUI_QT
  offs = 0;
#endif

  register int i, j;
  for (i = 0; i < height; i++) {
    for (j = offs; j < row; j += 4) {
      pdata[j] = 255;
    }
    pdata += row;
  }
}


void compact_rowstrides(weed_layer_t *layer) {
  // remove any extra padding after the image data
  int error;
  int *rowstrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, &error);
  int pal = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);
  int width = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
  int height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);
  int xheight;
  int crow = width * weed_palette_get_bits_per_macropixel(pal) / 8;
  int cxrow;
  int nplanes = weed_palette_get_nplanes(pal);
  register int i, j;

  size_t framesize = 0;

  void **pixel_data, **new_pixel_data;
  uint8_t *npixel_data;

  boolean needs_change = FALSE;

  pixel_data = weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);

  for (i = 0; i < nplanes; i++) {
    cxrow = crow * weed_palette_get_plane_ratio_horizontal(pal, i);
    xheight = height * weed_palette_get_plane_ratio_vertical(pal, i);
    framesize += ALIGN_CEIL(cxrow * xheight, 32);
    if (cxrow != rowstrides[i]) {
      // nth plane has extra padding
      needs_change = TRUE;
    }
  }

  if (!needs_change) {
    lives_free(pixel_data);
    lives_free(rowstrides);
    return;
  }

  npixel_data = (uint8_t *)lives_malloc(framesize);
  if (npixel_data == NULL) {
    lives_free(pixel_data);
    lives_free(rowstrides);
    return;
  }

  new_pixel_data = (void **)lives_malloc(nplanes * sizeof(void *));

  for (i = 0; i < nplanes; i++) {
    cxrow = crow * weed_palette_get_plane_ratio_horizontal(pal, i);
    xheight = height * weed_palette_get_plane_ratio_vertical(pal, i);

    new_pixel_data[i] = (void *)npixel_data;

    for (j = 0; j < xheight; j++) {
      lives_memcpy((uint8_t *)new_pixel_data[i] + j * cxrow, (uint8_t *)pixel_data[i] + j * rowstrides[i], cxrow);
      //for (int k = 3; k < cxrow; k += 4) ((uint8_t *)new_pixel_data[i])[j * cxrow + k] = 0;
    }

    framesize = ALIGN_CEIL(cxrow * xheight, 32);
    npixel_data += framesize;

    rowstrides[i] = cxrow;
  }

  weed_layer_pixel_data_free(layer);

  if (nplanes > 1)
    weed_set_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, WEED_TRUE);

  weed_set_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, nplanes, new_pixel_data);
  weed_set_int_array(layer, WEED_LEAF_ROWSTRIDES, nplanes, rowstrides);
  lives_free(pixel_data);
  lives_free(new_pixel_data);
  lives_free(rowstrides);
}


#ifdef USE_SWSCALE

void sws_free_context(void) {
  if (swscale != NULL) sws_freeContext(swscale);
  swscale = NULL;
}

#endif


/** @brief resize a layer

    width is in macropixels of the output palette

    opal_hint and oclamp_hint may be set to hint the desired output palette and YUV clamping
    this is simply to ensure more efficient resizing, and may be ignored

    - setting opal_hint to WEED_PALETTE_END will attempt to minimise palette changes

    "current_palette" should be checked on return as it may change

    @return FALSE if we were unable to resize */
boolean resize_layer(weed_layer_t *layer, int width, int height, LiVESInterpType interp, int opal_hint, int oclamp_hint) {
  // TODO ** - see if there is a resize plugin candidate/delegate which supports this palette :
  // this allows e.g libabl or a hardware rescaler
  LiVESPixbuf *pixbuf = NULL;
  LiVESPixbuf *new_pixbuf = NULL;

  boolean retval = TRUE;

  int error;
  int palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);

  // original width and height (in macropixels)
  int iwidth = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
  int iheight = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);
  int iclamping = weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error);
  int new_gamma_type = get_layer_gamma(layer);

#ifdef USE_SWSCALE
  boolean resolved = FALSE;
  int xpalette, xopal_hint;
#endif

  if (!weed_plant_has_leaf(layer, WEED_LEAF_PIXEL_DATA)) return FALSE;

  if (width <= 0 || height <= 0) {
    char *msg = lives_strdup_printf("unable to scale layer to %d x %d for palette %d\n", width, height, palette);
    LIVES_DEBUG(msg);
    lives_free(msg);
    return FALSE;
  }
  //     #define DEBUG_RESIZE
#ifdef DEBUG_RESIZE
  g_print("resizing layer size %d X %d with palette %s to %d X %d, hinted %s\n", iwidth, iheight, weed_palette_get_name_full(palette,
          iclamping, 0), width, height, weed_palette_get_name_full(opal_hint, oclamp_hint, 0));
#endif

  if (width < 4) width = 4;
  if (height < 4) height = 4;

  if (iwidth == width && iheight == height) return TRUE; // no resize needed

  // prevent a crash in swscale
  height = (height >> 1) << 1;
  width = (width >> 1) << 1;

  if (iwidth == width && iheight == height) return TRUE; // no resize needed

  // if in palette is a YUV palette which we cannot scale, convert to YUV888 (unclamped) or YUVA8888 (unclamped)
  // we can always scale these by pretending they are RGB24 and RGBA32 respectively
  if (weed_palette_is_yuv(palette)) {
    if (!weed_palette_is_resizable(palette, iclamping, TRUE)) {
      if (opal_hint == WEED_PALETTE_END || weed_palette_is_yuv(opal_hint)) {
        if (weed_palette_has_alpha_channel(palette)) {
          convert_layer_palette(layer, WEED_PALETTE_YUVA8888, WEED_YUV_CLAMPING_UNCLAMPED);
        } else {
          convert_layer_palette(layer, WEED_PALETTE_YUV888, WEED_YUV_CLAMPING_UNCLAMPED);
        }
        oclamp_hint = iclamping = (weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error));
      } else {
        if (weed_palette_has_alpha_channel(palette)) {
          convert_layer_palette(layer, WEED_PALETTE_RGBA32, 0);
        } else {
          convert_layer_palette(layer, WEED_PALETTE_RGB24, 0);
        }
      }
      iwidth = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
      iheight = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);
      if (iwidth == width && iheight == height) return TRUE; // no resize needed
      palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);
#ifdef DEBUG_RESIZE
      g_print("intermediate conversion 1 to %s\n", weed_palette_get_name_full(palette, iclamping, 0));
#endif
    }
  }

  // check if we can also convert to the output palette
#ifdef USE_SWSCALE
  // only swscale can convert and resize together
  if (opal_hint == WEED_PALETTE_END || !weed_palette_is_resizable(opal_hint, oclamp_hint, FALSE)) {
#endif
    opal_hint = palette;
    oclamp_hint = iclamping;
#ifdef USE_SWSCALE
  }
#endif

  // check if we can convert to the target palette/clamping
  if (!weed_palette_is_resizable(opal_hint, oclamp_hint, FALSE)) {
    opal_hint = palette;
    oclamp_hint = iclamping;
  }

#ifdef USE_SWSCALE
  // sws doesn't understand YUV888 or YUVA888, but if the output palette is also YUV888 or YUVA8888
  // then we can use unclamped values and  pretend they are RGB24 and RGBA32.
  // Otherwise we need to use YUV444P and YUVA4444P.

  // lookup values for av_pix_fmt
  xpalette = palette;
  xopal_hint = opal_hint;

  if (palette == WEED_PALETTE_YUV888) {
    if (opal_hint == WEED_PALETTE_YUV888 || opal_hint == WEED_PALETTE_YUVA8888) {
      if (iclamping == WEED_YUV_CLAMPING_CLAMPED) {
        convert_layer_palette(layer, WEED_PALETTE_YUV888, WEED_YUV_CLAMPING_UNCLAMPED);
        iclamping = (weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error));
#ifdef DEBUG_RESIZE
        g_print("intermediate conversion 2 to %s\n", weed_palette_get_name_full(palette, iclamping, 0));
#endif
      }
      if (iclamping == WEED_YUV_CLAMPING_UNCLAMPED) {
        xpalette = WEED_PALETTE_RGB24;
        oclamp_hint = WEED_YUV_CLAMPING_UNCLAMPED;
        resolved = TRUE;
        if (opal_hint == WEED_PALETTE_YUV888) {
          xopal_hint = WEED_PALETTE_RGB24;
        } else {
          xopal_hint = WEED_PALETTE_RGBA32;
        }
#ifdef DEBUG_RESIZE
        g_print("masquerading as %s\n", weed_palette_get_name_full(xpalette, oclamp_hint, 0));
#endif
      }
    }
    if (!resolved) {
      convert_layer_palette(layer, WEED_PALETTE_YUV444P, iclamping);
      xpalette = palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);
      iclamping = weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error);
#ifdef DEBUG_RESIZE
      g_print("intermediate conversion 3 to %s\n", weed_palette_get_name_full(xpalette, iclamping, 0));
#endif
    }
  } else if (palette == WEED_PALETTE_YUVA8888) {
    if (opal_hint == WEED_PALETTE_YUV888 || opal_hint == WEED_PALETTE_YUVA8888) {
      if (iclamping == WEED_YUV_CLAMPING_CLAMPED) {
        convert_layer_palette(layer, WEED_PALETTE_YUVA8888, WEED_YUV_CLAMPING_UNCLAMPED);
        xpalette = palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);
        iclamping = (weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error));
      }
      if (iclamping == WEED_YUV_CLAMPING_UNCLAMPED) {
        xpalette = WEED_PALETTE_RGBA32;
        oclamp_hint = WEED_YUV_CLAMPING_UNCLAMPED;
        resolved = TRUE;
        if (opal_hint == WEED_PALETTE_YUVA8888) {
          xopal_hint = WEED_PALETTE_RGBA32;
        } else {
          xopal_hint = WEED_PALETTE_RGB24;
        }
#ifdef DEBUG_RESIZE
        g_print("masquerading as %s\n", weed_palette_get_name_full(xpalette, oclamp_hint, 0));
#endif
      }
    }
    if (!resolved) {
      convert_layer_palette(layer, WEED_PALETTE_YUVA4444P, iclamping);
      xpalette = palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, &error);
      iclamping = (weed_get_int_value(layer, WEED_LEAF_YUV_CLAMPING, &error));
#ifdef DEBUG_RESIZE
      g_print("intermediate conversion 4 to %s\n", weed_palette_get_name_full(xpalette, iclamping, 0));
#endif
    }
  }

  // reget these after conversion
  iwidth = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
  iheight = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);

  if (opal_hint == WEED_PALETTE_YUV888) opal_hint = xopal_hint = WEED_PALETTE_YUV444P;
  else if (opal_hint == WEED_PALETTE_YUVA8888) opal_hint = xopal_hint = WEED_PALETTE_YUVA4444P;

  if (iwidth > 1 && iheight > 1 && weed_palette_is_resizable(palette, iclamping, TRUE) &&
      weed_palette_is_resizable(xopal_hint, oclamp_hint, FALSE)) {
    weed_layer_t *old_layer;

    void **in_pixel_data, **out_pixel_data;

    int *irowstrides, *orowstrides;

    //boolean store_ctx = FALSE;

#ifdef FF_API_PIX_FMT
    enum PixelFormat ipixfmt, opixfmt;
#else
    enum AVPixelFormat ipixfmt, opixfmt;
#endif

    int flags;

    const uint8_t *ipd[4], *opd[4];
    int irw[4], orw[4];

    int i;
    int subspace = WEED_YUV_SUBSPACE_YUV;
    int inplanes = weed_palette_get_nplanes(palette);
    int oplanes = weed_palette_get_nplanes(xopal_hint);

    /// old layer will hold a ref to the original pixel_data. We will free it at the end since the pixel_data
    /// of layer will be recreated when we calll create_empty_pixel_data()

    // get current values
    in_pixel_data = weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
    irowstrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, &error);

    old_layer = weed_layer_new();
    weed_layer_copy(old_layer, layer);

    av_log_set_level(AV_LOG_FATAL);

    mainw->rowstride_alignment_hint = 16;

    if (interp == LIVES_INTERP_BEST) flags = SWS_BICUBIC;
    if (interp == LIVES_INTERP_NORMAL) flags = SWS_BILINEAR;
    if (interp == LIVES_INTERP_FAST) flags = SWS_FAST_BILINEAR;

    ipixfmt = weed_palette_to_avi_pix_fmt(xpalette, &iclamping);
    opixfmt = weed_palette_to_avi_pix_fmt(xopal_hint, &oclamp_hint);

    for (i = 0; i < 4; i++) {
      // swscale always likes 4 elements, even if fewer planes are used
      if (i < inplanes) {
        ipd[i] = in_pixel_data[i];
        irw[i] = irowstrides[i];
      } else {
        ipd[i] = NULL;
        irw[i] = 0;
      }
    }

    if (weed_palette_is_rgb(palette) && !weed_palette_is_rgb(opal_hint) &&
        weed_get_int_value(layer, WEED_LEAF_GAMMA_TYPE, NULL) == WEED_GAMMA_LINEAR) {
      // gamma correction
      if (prefs->apply_gamma) {
        gamma_correct_layer(WEED_GAMMA_SRGB, layer);
      }
      new_gamma_type = weed_get_int_value(layer, WEED_LEAF_GAMMA_TYPE, NULL);
    }

    // set new values

    if (palette != opal_hint) {
      // our widths are in macropixels of the input palette
      // convert output width to macropixels of the output palette
      width *= weed_palette_get_pixels_per_macropixel(palette);
      width /= weed_palette_get_pixels_per_macropixel(opal_hint);
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, opal_hint);
    }

    if (weed_palette_is_yuv(opal_hint))
      weed_set_int_value(layer, WEED_LEAF_YUV_CLAMPING, oclamp_hint);

    weed_set_int_value(layer, WEED_LEAF_WIDTH, width);
    weed_set_int_value(layer, WEED_LEAF_HEIGHT, height);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, NULL);

    if (!create_empty_pixel_data(layer, FALSE, TRUE)) {
      weed_layer_copy(layer, old_layer);
      weed_plant_free(old_layer);
      return FALSE;
    }

    weed_set_int_value(layer, WEED_LEAF_GAMMA_TYPE, new_gamma_type);

    out_pixel_data = weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &error);
    orowstrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, &error);

    for (i = 0; i < 4; i++) {
      // swscale always likes 4 elements, even if fewer planes are used
      if (i < oplanes) {
        opd[i] = out_pixel_data[i];
        orw[i] = orowstrides[i];
      } else {
        opd[i] = NULL;
        orw[i] = 0;
      }
    }

    // our input widths are in macropixels, but swscale uses pixels
    width *= weed_palette_get_pixels_per_macropixel(opal_hint);
    iwidth *= weed_palette_get_pixels_per_macropixel(palette);

    // TODO - subspaces
    swscale = sws_getCachedContext(swscale, iwidth, iheight, ipixfmt, width, height, opixfmt, flags, NULL, NULL, NULL);
    sws_setColorspaceDetails(swscale, sws_getCoefficients((subspace == WEED_YUV_SUBSPACE_BT709)
                             ? SWS_CS_ITU709 : SWS_CS_DEFAULT) , iclamping, sws_getCoefficients((subspace == WEED_YUV_SUBSPACE_BT709)
                                 ? SWS_CS_ITU709 : SWS_CS_DEFAULT), oclamp_hint,  0, 1 << 16, 1 << 16);


    if (swscale == NULL) {
      LIVES_DEBUG("swscale is NULL !!");
    } else {

#ifdef DEBUG_RESIZE
      g_print("before resize with swscale: layer size %d X %d with palette %s to %d X %d, hinted %s,\nmasquerading as %s (avpixfmt %d to avpixfmt %d)\n",
              iwidth, iheight, weed_palette_get_name_full(palette, iclamping, 0), width, height, weed_palette_get_name_full(opal_hint, oclamp_hint, 0),
              weed_palette_get_name_full(xopal_hint, oclamp_hint, 0), ipixfmt, opixfmt);
#endif

      sws_scale(swscale, (const uint8_t *const *)ipd, irw, 0, iheight,
                (uint8_t *const *)opd, orw);
#ifdef DEBUG_RESIZE
      g_print("after resize with swscale: layer size %d X %d, palette %s (assumed succesful)\n", width, height,
              weed_palette_get_name_full(opal_hint, oclamp_hint, 0));
#endif
      //if (store_ctx) swscale_add_context(iwidth, iheight, width, height, ipixfmt, opixfmt, flags, swscale);
    }

    // this will properly free() in_pixel_data

    weed_layer_free(old_layer);
    lives_free(out_pixel_data);

    lives_free(orowstrides);
    lives_free(irowstrides);

    lives_free(in_pixel_data);

    return TRUE;
  }
#endif

  // reget these after conversion
  iwidth = weed_get_int_value(layer, WEED_LEAF_WIDTH, &error);
  iheight = weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error);

  switch (palette) {
  // anything with 3 or 4 channels (alpha must be last)

  case WEED_PALETTE_YUV888:
  case WEED_PALETTE_YUVA8888:
    if (iclamping == WEED_YUV_CLAMPING_CLAMPED) {
      if (weed_palette_has_alpha_channel(palette)) {
        convert_layer_palette(layer, WEED_PALETTE_YUVA8888, WEED_YUV_CLAMPING_UNCLAMPED);
      } else {
        convert_layer_palette(layer, WEED_PALETTE_YUV888, WEED_YUV_CLAMPING_UNCLAMPED);
      }
    }

  case WEED_PALETTE_RGB24:
  case WEED_PALETTE_BGR24:
  case WEED_PALETTE_RGBA32:
  case WEED_PALETTE_BGRA32:

    // create a new pixbuf
    gamma_correct_layer(cfile->gamma_type, layer);
    pixbuf = layer_to_pixbuf(layer, FALSE);

    threaded_dialog_spin(0.);
    new_pixbuf = lives_pixbuf_scale_simple(pixbuf, width, height, interp);
    threaded_dialog_spin(0.);
    if (new_pixbuf != NULL) {
      weed_set_int_value(layer, WEED_LEAF_WIDTH, lives_pixbuf_get_width(new_pixbuf));
      weed_set_int_value(layer, WEED_LEAF_HEIGHT, lives_pixbuf_get_height(new_pixbuf));
      weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, lives_pixbuf_get_rowstride(new_pixbuf));
    }

    lives_widget_object_unref(pixbuf);

    break;
  default:
    lives_printerr("Warning: resizing unknown palette %d\n", palette);
    break_me();
    retval = FALSE;
  }

  if (new_pixbuf == NULL || (width != weed_get_int_value(layer, WEED_LEAF_WIDTH, &error) ||
                             height != weed_get_int_value(layer, WEED_LEAF_HEIGHT, &error)))  {
    lives_printerr("unable to scale layer to %d x %d for palette %d\n", width, height, palette);
    retval = FALSE;
  } else {
    if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_ORIG_PDATA))
      weed_leaf_delete(layer, WEED_LEAF_HOST_ORIG_PDATA);
  }

  if (new_pixbuf != NULL) {
    if (!pixbuf_to_layer(layer, new_pixbuf)) lives_widget_object_unref(new_pixbuf);
  }

  return retval;
}


void letterbox_layer(weed_layer_t *layer, int width, int height, int nwidth, int nheight,
                     LiVESInterpType interp, int tpal, int tclamp) {
  // stretch or shrink layer to width/height, then overlay it in a black rectangle size nwidth/nheight
  // width, nwidth should be in _macropixels_
  weed_layer_t *old_layer;
  int *rowstrides, *irowstrides;
  void **pixel_data;
  void **new_pixel_data;
  uint8_t *dst, *src;

  int offs_x = 0, offs_y = 0;
  int pal;

  register int i;

  if (width * height * nwidth * nheight == 0) return;
  if (nwidth < width) nwidth = width;
  if (nheight < height) nheight = height;
  if (nheight == height && nwidth == width) return;

  /// resize the inner rectangle
  /// widths are passed as pixels, but we need them in macropixels
  resize_layer(layer, width, height, interp, tpal, tclamp);
  pal = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, NULL);

  // old layer will hold pointers to the original pixel data for layer
  old_layer = weed_layer_new();
  if (old_layer == NULL) return;
  if (!weed_layer_copy(old_layer, layer)) return;

  pixel_data = weed_get_voidptr_array(old_layer, WEED_LEAF_PIXEL_DATA, NULL);

  if (pixel_data == NULL) {
    weed_layer_free(old_layer);
    return;
  }

  width = weed_get_int_value(layer, WEED_LEAF_WIDTH, NULL);
  height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, NULL);
  irowstrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);

  /// create the outer rectangle in layer
  weed_set_int_value(layer, WEED_LEAF_WIDTH, nwidth);
  weed_set_int_value(layer, WEED_LEAF_HEIGHT, nheight);
  weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, NULL);
  create_empty_pixel_data(layer, TRUE, TRUE);
  new_pixel_data = weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, NULL);

  /// get the actual size after any adjustments
  nwidth = weed_get_int_value(layer, WEED_LEAF_WIDTH, NULL);
  nheight = weed_get_int_value(layer, WEED_LEAF_HEIGHT, NULL);

  if (nwidth < width || nheight < height || new_pixel_data == NULL) {
    /// this shouldnt happen, but if  the outer rectangle is smaller than the inner we have to abort
    weed_layer_pixel_data_free(layer);
    weed_layer_copy(layer, old_layer);
    weed_set_voidptr_value(old_layer, WEED_LEAF_PIXEL_DATA, NULL);
    weed_layer_free(old_layer);
    lives_free(pixel_data);
    lives_free(irowstrides);
    return;
  }

  width *= weed_palette_get_pixels_per_macropixel(pal),
           nwidth *= weed_palette_get_pixels_per_macropixel(pal),

                     offs_x = (nwidth - width + 1) >> 1;
  offs_y = (nheight - height + 1) >> 1;

  rowstrides = weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, NULL);

  switch (pal) {
  // 3 byte pixels, packed
  case WEED_PALETTE_RGB24:
  case WEED_PALETTE_BGR24:
  case WEED_PALETTE_YUV888:
    width *= 3;
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x * 3;
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    break;

  // 4 byte pixels, packed
  case WEED_PALETTE_RGBA32:
  case WEED_PALETTE_BGRA32:
  case WEED_PALETTE_ARGB32:
  case WEED_PALETTE_YUVA8888:
  case WEED_PALETTE_UYVY:
  case WEED_PALETTE_YUYV:
    width *= 4;
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x * 4;
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    break;

  case WEED_PALETTE_YUV411:
    width *= 6;
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x * 6;
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    break;

  case WEED_PALETTE_YUV444P:
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x;
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    dst = (uint8_t *)new_pixel_data[1] + offs_y * rowstrides[1] + offs_x;
    src = (uint8_t *)pixel_data[1];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[1];
      src += irowstrides[1];
    }
    dst = (uint8_t *)new_pixel_data[2] + offs_y * rowstrides[2] + offs_x;
    src = (uint8_t *)pixel_data[2];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[2];
      src += irowstrides[2];
    }
    break;

  case WEED_PALETTE_YUVA4444P:
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x;
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    dst = (uint8_t *)new_pixel_data[1] + offs_y * rowstrides[1] + offs_x;
    src = (uint8_t *)pixel_data[1];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[1];
      src += irowstrides[1];
    }
    dst = (uint8_t *)new_pixel_data[2] + offs_y * rowstrides[2] + offs_x;
    src = (uint8_t *)pixel_data[2];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[2];
      src += irowstrides[2];
    }
    dst = (uint8_t *)new_pixel_data[3] + offs_y * rowstrides[3] + offs_x;
    src = (uint8_t *)pixel_data[3];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[3];
      src += irowstrides[3];
    }
    break;

  case WEED_PALETTE_YUV422P:
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x;
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    height >>= 1;
    offs_x >>= 1;
    dst = (uint8_t *)new_pixel_data[1] + offs_y * rowstrides[1] + offs_x;
    src = (uint8_t *)pixel_data[1];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[1];
      src += irowstrides[1];
    }
    dst = (uint8_t *)new_pixel_data[2] + offs_y * rowstrides[2] + offs_x;
    src = (uint8_t *)pixel_data[2];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[2];
      src += irowstrides[2];
    }
    break;

  case WEED_PALETTE_YUV420P:
  case WEED_PALETTE_YVU420P:
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x;
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    height >>= 1;
    offs_x >>= 1;
    width >>= 1;
    offs_y >>= 1;
    dst = (uint8_t *)new_pixel_data[1] + offs_y * rowstrides[1] + offs_x;
    src = (uint8_t *)pixel_data[1];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[1];
      src += irowstrides[1];
    }
    dst = (uint8_t *)new_pixel_data[2] + offs_y * rowstrides[2] + offs_x;
    src = (uint8_t *)pixel_data[2];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[2];
      src += irowstrides[2];
    }
    break;

  case WEED_PALETTE_RGBFLOAT:
    width *= 3 * sizeof(float);
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x * 3 * sizeof(float);
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    break;

  case WEED_PALETTE_RGBAFLOAT:
    width *= 4 * sizeof(float);
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x * 4 * sizeof(float);
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    break;

  case WEED_PALETTE_AFLOAT:
    width *= sizeof(float);
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x * sizeof(float);
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    break;

  case WEED_PALETTE_A8:
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + offs_x;
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    break;

  // assume offs_x and width is a multiple of 8
  case WEED_PALETTE_A1:
    width >>= 3;
    dst = (uint8_t *)new_pixel_data[0] + offs_y * rowstrides[0] + (offs_x >> 3);
    src = (uint8_t *)pixel_data[0];
    for (i = 0; i < height; i++) {
      lives_memcpy(dst, src, width);
      dst += rowstrides[0];
      src += irowstrides[0];
    }
    break;
  }
  weed_layer_free(old_layer);
  lives_free(pixel_data);
  lives_free(new_pixel_data);
  lives_free(irowstrides);
  lives_free(rowstrides);
}


/** @brief turn a (Gdk)Pixbuf into a Weed layer

  return TRUE if we can use the original pixbuf pixels; in this case the pixbuf pixels should only be freed via
  lives_layer_pixel_data_free() or lives_layer_free()
  see code example.

   code example:

  if (pixbuf != NULL) {
    if (!pixbuf_to_layer(layer, pixbuf)) lives_widget_object_unref(pixbuf);
    else do NOT unref the pixbuf !!!!
  }

  do something with layer...

  weed_layer_pixel_data_free(layer); unrefs the pixbuf
  or
  weed_layer_free(layer); also unrefs the pixbuf

  */
boolean pixbuf_to_layer(weed_layer_t *layer, LiVESPixbuf *pixbuf) {
  size_t framesize;
  void *pixel_data;
  void *in_pixel_data;
  int rowstride;
  int width;
  int height;
  int nchannels, palette;

  if (!LIVES_IS_PIXBUF(pixbuf)) {
    weed_set_int_value(layer, WEED_LEAF_WIDTH, 0);
    weed_set_int_value(layer, WEED_LEAF_HEIGHT, 0);
    weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, 0);
    weed_layer_pixel_data_free(layer);
    return FALSE;
  }

  rowstride = lives_pixbuf_get_rowstride(pixbuf);
  width = lives_pixbuf_get_width(pixbuf);
  height = lives_pixbuf_get_height(pixbuf);
  nchannels = lives_pixbuf_get_n_channels(pixbuf);

  weed_set_int_value(layer, WEED_LEAF_WIDTH, width);
  weed_set_int_value(layer, WEED_LEAF_HEIGHT, height);
  weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);

  if (!weed_plant_has_leaf(layer, WEED_LEAF_CURRENT_PALETTE)) {
#ifdef GUI_GTK
    if (nchannels == 4) weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, WEED_PALETTE_RGBA32);
    else weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, WEED_PALETTE_RGB24);
#endif
  }

  if (rowstride == get_last_pixbuf_rowstride_value(width, nchannels)) {
    in_pixel_data = (void *)lives_pixbuf_get_pixels(pixbuf);
    weed_layer_pixel_data_free(layer);
    weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, in_pixel_data);
    weed_set_voidptr_value(layer, WEED_LEAF_HOST_PIXBUF_SRC, pixbuf);
    palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, NULL);
    if (weed_palette_is_rgb(palette)) weed_set_int_value(layer, WEED_LEAF_GAMMA_TYPE, WEED_GAMMA_SRGB);
    return TRUE;
  }

  framesize = ALIGN_CEIL(rowstride * height, 32);

  pixel_data = lives_calloc(framesize >> 4, 16);

  if (pixel_data != NULL) {
    in_pixel_data = (void *)lives_pixbuf_get_pixels_readonly(pixbuf);
    lives_memcpy(pixel_data, in_pixel_data, rowstride * (height - 1));
    // this part is needed because layers always have a memory size height*rowstride, whereas gdkpixbuf can have
    // a shorter last row
    lives_memcpy((uint8_t *)pixel_data + rowstride * (height - 1), (uint8_t *)in_pixel_data + rowstride * (height - 1),
                 get_last_pixbuf_rowstride_value(width, nchannels));
  }

  weed_layer_pixel_data_free(layer);
  weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);

  palette = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, NULL);
  if (weed_palette_is_rgb(palette)) weed_set_int_value(layer, WEED_LEAF_GAMMA_TYPE, WEED_GAMMA_SRGB);

  return FALSE;
}


#ifdef GUI_GTK

/** @brief convert a weed layer to lives_painter (a.k.a cairo)

    width, height and rowstrides of source layer may all change */
lives_painter_t *layer_to_lives_painter(weed_layer_t *layer) {

  lives_painter_surface_t *surf;
  lives_painter_t *cairo;
  lives_painter_format_t cform;
  uint8_t *src, *dst, *pixel_data;

  int irowstride, orowstride;
  int width, widthx;
  int height, pal;

  register int i;

  if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_SURFACE_SRC)) {
    surf = (lives_painter_surface_t *)weed_get_voidptr_value(layer, WEED_LEAF_HOST_SURFACE_SRC, NULL);
  } else {
    width = weed_get_int_value(layer, WEED_LEAF_WIDTH, NULL);
    pal = weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, NULL);
    if (pal == WEED_PALETTE_A8) {
      cform = LIVES_PAINTER_FORMAT_A8;
      widthx = width;
    } else if (pal == WEED_PALETTE_A1) {
      cform = LIVES_PAINTER_FORMAT_A1;
      widthx = width >> 3;
    } else {
      convert_layer_palette(layer, LIVES_PAINTER_COLOR_PALETTE(capable->byte_order), 0);
      cform = LIVES_PAINTER_FORMAT_ARGB32;
      widthx = width << 2;
    }

    height = weed_get_int_value(layer, WEED_LEAF_HEIGHT, NULL);
    irowstride = weed_get_int_value(layer, WEED_LEAF_ROWSTRIDES, NULL);

    orowstride = lives_painter_format_stride_for_width(cform, width);

    src = (uint8_t *)weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, NULL);

    if (irowstride == orowstride && !weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXBUF_SRC) &&
        !weed_plant_has_leaf(layer, WEED_LEAF_HOST_ORIG_PDATA)) {
      pixel_data = src;
    } else {
      dst = pixel_data = (uint8_t *)lives_calloc(height * orowstride, 1);
      if (pixel_data == NULL) return NULL;
      for (i = 0; i < height; i++) {
        lives_memcpy(dst, src, widthx);
        dst += orowstride;
        src += irowstride;
      }
      weed_layer_pixel_data_free(layer);
      weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, pixel_data);
      weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, orowstride);
    }

    if (weed_palette_has_alpha_channel(pal)) {
      int flags = 0;
      if (weed_plant_has_leaf(layer, WEED_LEAF_FLAGS)) flags = weed_get_int_value(layer, WEED_LEAF_FLAGS, NULL);
      if (!(flags & WEED_LAYER_ALPHA_PREMULT)) {
        // if we have post-multiplied alpha, pre multiply
        alpha_unpremult(layer, FALSE);
        flags |= WEED_LAYER_ALPHA_PREMULT;
        weed_set_int_value(layer, WEED_LEAF_FLAGS, flags);
      }
    }

    surf = lives_painter_image_surface_create_for_data(pixel_data, cform, width, height, orowstride);
  }
  if (surf == NULL) return NULL;

  cairo = lives_painter_create_from_surface(surf); // surf is refcounted
#ifdef DEBUG_CAIRO_SURFACE
  g_print("VALaa1 = %d %p\n", cairo_surface_get_reference_count(surf), surf);
#endif
  weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, lives_painter_image_surface_get_data(surf));
  weed_set_voidptr_value(layer, WEED_LEAF_HOST_SURFACE_SRC, surf);

  return cairo;
}


/** @brief convert a lives_painter_t (a.k.a) cairo_t to a weed layer */
boolean lives_painter_to_layer(lives_painter_t *cr, weed_layer_t *layer) {
  // updates a weed_layer from a cr
  void *src;
  lives_painter_surface_t *surface = lives_painter_get_target(cr), *xsurface = NULL;
  lives_painter_format_t  cform;

  int width, height, rowstride, error;

  /// flush to ensure all writing to the image surface was done
  lives_painter_surface_flush(surface);

  if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_SURFACE_SRC)) {
    xsurface = (lives_painter_surface_t *)weed_get_voidptr_value(layer, WEED_LEAF_HOST_SURFACE_SRC, &error);
  }
  if (xsurface != surface) weed_layer_pixel_data_free(layer);

  src = lives_painter_image_surface_get_data(surface);

  weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, src);
  weed_set_voidptr_value(layer, WEED_LEAF_HOST_SURFACE_SRC, surface);

#ifdef DEBUG_CAIRO_SURFACE
  g_print("VALaa2 = %d %p\n", cairo_surface_get_reference_count(surface), surface);
#endif
  lives_painter_surface_reference(surface);
  lives_painter_destroy(cr);

  width = lives_painter_image_surface_get_width(surface);
  height = lives_painter_image_surface_get_height(surface);
  rowstride = lives_painter_image_surface_get_stride(surface);

  weed_set_int_value(layer, WEED_LEAF_ROWSTRIDES, rowstride);
  weed_set_int_value(layer, WEED_LEAF_WIDTH, width);
  weed_set_int_value(layer, WEED_LEAF_HEIGHT, height);

  cform = lives_painter_image_surface_get_format(surface);

  switch (cform) {
  case LIVES_PAINTER_FORMAT_ARGB32:
    if (capable->byte_order == LIVES_BIG_ENDIAN) {
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, WEED_PALETTE_ARGB32);
    } else {
      weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, WEED_PALETTE_BGRA32);
    }
    weed_set_int_value(layer, WEED_LEAF_GAMMA_TYPE, WEED_GAMMA_SRGB);

    if (prefs->alpha_post) {
      /// un-premultiply the alpha
      alpha_unpremult(layer, TRUE);
    } else {
      int flags = 0, error;
      if (weed_plant_has_leaf(layer, WEED_LEAF_FLAGS))
        flags = weed_get_int_value(layer, WEED_LEAF_FLAGS, &error);

      flags |= WEED_LAYER_ALPHA_PREMULT;
      weed_set_int_value(layer, WEED_LEAF_FLAGS, flags);
    }
    break;

  case LIVES_PAINTER_FORMAT_A8:
    weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, WEED_PALETTE_A8);
    break;

  case LIVES_PAINTER_FORMAT_A1:
    weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, WEED_PALETTE_A1);
    break;

  default:
    break;
  }

  return TRUE;
}

#endif


/** @brief create a layer, setting the most important properties */
weed_layer_t *weed_layer_create(int width, int height, int *rowstrides, int current_palette) {
  weed_layer_t *layer = weed_plant_new(WEED_PLANT_LAYER);

  weed_set_int_value(layer, WEED_LEAF_WIDTH, width);
  weed_set_int_value(layer, WEED_LEAF_HEIGHT, height);

  if (current_palette != WEED_PALETTE_END) {
    weed_set_int_value(layer, WEED_LEAF_CURRENT_PALETTE, current_palette);
    if (rowstrides != NULL) weed_set_int_array(layer, WEED_LEAF_ROWSTRIDES,
          weed_palette_get_nplanes(current_palette), rowstrides);
  }
  return layer;
}


weed_layer_t *weed_layer_create_full(int width, int height, int *rowstrides, int current_palette,
                                     int YUV_clamping, int YUV_sampling, int YUV_subspace, int gamma_type) {
  weed_layer_t *layer = weed_layer_create(width, height, rowstrides, current_palette);
  weed_set_int_value(layer, WEED_LEAF_YUV_CLAMPING, YUV_clamping);
  weed_set_int_value(layer, WEED_LEAF_YUV_SAMPLING, YUV_sampling);
  weed_set_int_value(layer, WEED_LEAF_YUV_SUBSPACE, YUV_subspace);
  weed_set_int_value(layer, WEED_LEAF_GAMMA_TYPE, gamma_type);
  return layer;
}


/** @brief copy source layer slayer to dest layer dlayer

  if dlayer is NULL, we return a new layer, otherwise we return dlayer
  for a newly created layer, this is a deep copy, since the pixel_data array is also copied
  for an existing dlayer, we copy pixel_data by reference.
  all the other relevant attributes are also copied
 */
weed_layer_t *weed_layer_copy(weed_layer_t *dlayer, weed_layer_t *slayer) {
  weed_layer_t *layer;
  void **pd_array = NULL;

  if (slayer == NULL || !WEED_PLANT_IS_LAYER(slayer)) return NULL;

  if (dlayer != NULL) {
    if (!WEED_PLANT_IS_LAYER(dlayer)) return NULL;
    layer = dlayer;
  }

  pd_array = weed_get_voidptr_array(slayer, WEED_LEAF_PIXEL_DATA, NULL);

  if (dlayer == NULL) {
    /// deep copy
    int height = weed_get_int_value(slayer, WEED_LEAF_HEIGHT, NULL);
    int width = weed_get_int_value(slayer, WEED_LEAF_WIDTH, NULL);
    int palette = weed_get_int_value(slayer, WEED_LEAF_CURRENT_PALETTE, NULL);
    int *rowstrides = weed_get_int_array(slayer, WEED_LEAF_ROWSTRIDES, NULL);
    if (height <= 0 || width < 0 || rowstrides == NULL || !weed_palette_is_valid(palette)) {
      return NULL;
    } else {
      void **pixel_data;
      layer = weed_layer_create(width, height, rowstrides, palette);
      if (!create_empty_pixel_data(layer, FALSE, TRUE)) {
        /// memory allocation errors
        lives_free(rowstrides);
        lives_free(pd_array);
        return weed_layer_free(layer);
      }
      if (pd_array == NULL) weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, NULL);
      else {
        weed_size_t pd_elements = weed_leaf_num_elements(slayer, WEED_LEAF_PIXEL_DATA);
        pixel_data = weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, NULL);
        if (pixel_data != NULL) {
          for (int i = 0; i < pd_elements; i++) {
            size_t size = (size_t)((((int)(((float)height * weed_palette_get_plane_ratio_vertical(palette, i))) >> 1) << 1) * rowstrides[i]);
            lives_memcpy(pixel_data[i], pd_array[i], size);
          }
          lives_free(pixel_data);
        }
      }
      lives_free(rowstrides);
    }
  } else {
    /// shallow copy
    weed_leaf_copy(layer, WEED_LEAF_ROWSTRIDES, slayer, WEED_LEAF_ROWSTRIDES);
    weed_leaf_copy(layer, WEED_LEAF_PIXEL_DATA, slayer, WEED_LEAF_PIXEL_DATA);
    weed_leaf_copy_or_delete(layer, WEED_LEAF_HEIGHT, slayer);
    weed_leaf_copy_or_delete(layer, WEED_LEAF_WIDTH, slayer);
    weed_leaf_copy_or_delete(layer, WEED_LEAF_CURRENT_PALETTE, slayer);
    if (pd_array != NULL) {
      weed_leaf_copy_or_delete(layer, WEED_LEAF_HOST_PIXBUF_SRC, slayer);
      weed_leaf_copy_or_delete(layer, WEED_LEAF_HOST_ORIG_PDATA, slayer);
      weed_leaf_copy_or_delete(layer, WEED_LEAF_HOST_SURFACE_SRC, slayer);
      weed_leaf_copy_or_delete(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, slayer);
    }
  }

  weed_leaf_copy_or_delete(layer, WEED_LEAF_GAMMA_TYPE, slayer);
  weed_leaf_copy_or_delete(layer, WEED_LEAF_FLAGS, slayer);
  weed_leaf_copy_or_delete(layer, WEED_LEAF_YUV_CLAMPING, slayer);
  weed_leaf_copy_or_delete(layer, WEED_LEAF_YUV_SUBSPACE, slayer);
  weed_leaf_copy_or_delete(layer, WEED_LEAF_YUV_SAMPLING, slayer);
  weed_leaf_copy_or_delete(layer, WEED_LEAF_PIXEL_ASPECT_RATIO, slayer);

  if (pd_array != NULL) lives_free(pd_array);
  return layer;
}


/** @brief free pixel_data from layer

  we do not free if WEED_LEAF_HOST_ORIG_PDATA is set (data is an alpha in which "belongs" to another out param)

  take care of WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS
  take care of WEED_LEAF_HOST_PIXBUF_SRC
  take care of WEED_LEAF_HOST_SURFACE_SRC

  sets WEED_LEAF_PIXEL_DATA to NULL for the layer

  this function should always be used to free WEED_LEAF_PIXEL_DATA */
void weed_layer_pixel_data_free(weed_layer_t *layer) {
  void **pixel_data;
  int pd_elements;
  register int i;

  if (layer == NULL) return;

  if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_ORIG_PDATA)
      && weed_get_boolean_value(layer, WEED_LEAF_HOST_ORIG_PDATA, NULL) == WEED_TRUE)
    return;

  if (weed_get_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, NULL) != NULL) {
    pd_elements = weed_leaf_num_elements(layer, WEED_LEAF_PIXEL_DATA);
    if (pd_elements > 0) {
      pixel_data = weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, NULL);
      if (pixel_data != NULL) {
        if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS)) {
          if (weed_get_boolean_value(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS, NULL) == WEED_TRUE)
            pd_elements = 1;
        }

        if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_PIXBUF_SRC)) {
          LiVESPixbuf *pixbuf = (LiVESPixbuf *)weed_get_voidptr_value(layer, WEED_LEAF_HOST_PIXBUF_SRC, NULL);
          if (pixbuf != NULL) lives_widget_object_unref(pixbuf);
        } else {
          if (weed_plant_has_leaf(layer, WEED_LEAF_HOST_SURFACE_SRC)) {
            lives_painter_surface_t *surface = (lives_painter_surface_t *)weed_get_voidptr_value(layer,
                                               WEED_LEAF_HOST_SURFACE_SRC, NULL);
            if (surface != NULL) {
              // this is where most surfaces die, as we convert from BGRA -> RGB
              uint8_t *pdata = lives_painter_image_surface_get_data(surface);
#ifdef DEBUG_CAIRO_SURFACE
              g_print("VALaa23rrr = %d %p\n", cairo_surface_get_reference_count(surface), surface);
#endif
              // call twice to remove our extra ref.
              lives_painter_surface_destroy(surface);
              lives_painter_surface_destroy(surface);
              lives_free(pdata);
            }
          } else {
            for (i = 0; i < pd_elements; i++) {
              if (pixel_data[i] != NULL) lives_free(pixel_data[i]);
            }
          }
        }
        lives_free(pixel_data);
        weed_set_voidptr_value(layer, WEED_LEAF_PIXEL_DATA, NULL);
      }
    }
  }

  weed_leaf_delete(layer, WEED_LEAF_HOST_PIXEL_DATA_CONTIGUOUS);
  weed_leaf_delete(layer, WEED_LEAF_HOST_PIXBUF_SRC);
  weed_leaf_delete(layer, WEED_LEAF_HOST_SURFACE_SRC);
}


/** @ brief frees pixel_data for a layer, then the layer itself

    returns (void *)NULL for convenience
 */
void *weed_layer_free(weed_layer_t *layer) {
  if (layer == NULL) return NULL;
  weed_layer_pixel_data_free(layer);
  weed_plant_free(layer);
  return NULL;
}


LIVES_GLOBAL_INLINE void **weed_layer_get_pixel_data(weed_plant_t *layer) {
  if (layer == NULL)  return NULL;
  return weed_get_voidptr_array(layer, WEED_LEAF_PIXEL_DATA, &weed_general_error);
}


LIVES_GLOBAL_INLINE int *weed_layer_get_rowstrides(weed_plant_t *layer) {
  if (layer == NULL)  return NULL;
  return weed_get_int_array(layer, WEED_LEAF_ROWSTRIDES, &weed_general_error);
}


LIVES_GLOBAL_INLINE int weed_layer_get_width(weed_plant_t *layer) {
  if (layer == NULL)  return -1;
  return weed_get_int_value(layer, WEED_LEAF_WIDTH, &weed_general_error);
}


LIVES_GLOBAL_INLINE int weed_layer_get_height(weed_plant_t *layer) {
  if (layer == NULL)  return -1;
  return weed_get_int_value(layer, WEED_LEAF_HEIGHT, &weed_general_error);
}


LIVES_GLOBAL_INLINE int weed_layer_get_palette(weed_layer_t *layer) {
  if (layer == NULL)  return WEED_PALETTE_END;
  return weed_get_int_value(layer, WEED_LEAF_CURRENT_PALETTE, NULL);
}

