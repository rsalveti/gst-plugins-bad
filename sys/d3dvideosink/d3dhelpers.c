/* GStreamer
 * Copyright (C) 2012 Roland Krikava <info@bluedigits.com>
 * Copyright (C) 2010-2011 David Hoyt <dhoyt@hoytsoft.org>
 * Copyright (C) 2010 Andoni Morales <ylatuya@gmail.com>
 * Copyright (C) 2012 Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "d3dvideosink.h"
#include "d3dhelpers.h"

#include <stdio.h>

/** FWD DECLS **/

static gboolean d3d_hidden_window_thread (GstD3DVideoSinkClass * class);
static gboolean d3d_window_wndproc_set (GstD3DVideoSink * sink);
static void d3d_window_wndproc_unset (GstD3DVideoSink * sink);
static gboolean d3d_init_swap_chain (GstD3DVideoSink * sink, HWND hWnd);
static gboolean d3d_release_swap_chain (GstD3DVideoSink * sink);
static gboolean d3d_resize_swap_chain (GstD3DVideoSink * sink);
static gboolean d3d_present_swap_chain (GstD3DVideoSink * sink);
static gboolean d3d_copy_buffer_to_surface (GstD3DVideoSink * sink,
    GstBuffer * buffer);
static gboolean d3d_stretch_and_copy (GstD3DVideoSink * sink,
    LPDIRECT3DSURFACE9 back_buffer);
static HWND d3d_create_internal_window (GstD3DVideoSink * sink);

static void d3d_class_notify_device_lost (GstD3DVideoSink * sink);


static LRESULT APIENTRY d3d_wnd_proc_internal (HWND hWnd, UINT message,
    WPARAM wParam, LPARAM lParam);
static LRESULT APIENTRY d3d_wnd_proc (HWND hWnd, UINT message, WPARAM wParam,
    LPARAM lParam);


GST_DEBUG_CATEGORY_EXTERN (gst_d3dvideosink_debug);
#define GST_CAT_DEFAULT gst_d3dvideosink_debug

static gint WM_D3DVIDEO_NOTIFY_DEVICE_LOST = 0;
#define IDT_DEVICE_RESET_TIMER 0

/** Helpers **/

#define ERROR_CHECK_HR(hr)                          \
  if(hr != S_OK) {                                  \
    const gchar * str_err=NULL, *t1=NULL;           \
    gchar tmp[128]="";                              \
    switch(hr)
#define CASE_HR_ERR(hr_err)                         \
      case hr_err: str_err = #hr_err; break;
#define CASE_HR_DBG_ERR_END(sink, gst_err_msg, level) \
      default:                                      \
        t1=gst_err_msg;                             \
      sprintf(tmp, "HR-SEV:%u HR-FAC:%u HR-CODE:%u", (guint)HRESULT_SEVERITY(hr), (guint)HRESULT_FACILITY(hr), (guint)HRESULT_CODE(hr)); \
        str_err = tmp;                              \
    } /* end switch */                              \
    GST_CAT_LEVEL_LOG(GST_CAT_DEFAULT, level, sink, "%s HRESULT: %s", t1?t1:"", str_err);
#define CASE_HR_ERR_END(sink, gst_err_msg)          \
  CASE_HR_DBG_ERR_END(sink, gst_err_msg, GST_LEVEL_ERROR)
#define CASE_HR_DBG_END(sink, gst_err_msg)          \
  CASE_HR_DBG_ERR_END(sink, gst_err_msg, GST_LEVEL_DEBUG)

#define CHECK_D3D_DEVICE(class, sink, goto_label)                       \
  if(!class->d3d.d3d || !class->d3d.device.d3d_device) {                \
    GST_ERROR_OBJECT(sink, "Direct3D device or object does not exist"); \
    goto goto_label;                                                    \
  }
#define CHECK_D3D_SWAPCHAIN(sink, goto_label)                       \
  if(!sink->d3d.swapchain) {                                        \
    GST_ERROR_OBJECT(sink, "Direct3D swap chain does not exist");   \
    goto goto_label;                                                \
  }
#define CHECK_D3D_SURFACE(sink, goto_label)                 \
  if(!sink->d3d.surface) {                                  \
    GST_ERROR_OBJECT(sink, "NULL D3D offscreen surface");   \
    goto goto_label;                                        \
  }
#define CHECK_WINDOW_HANDLE(sink, goto_label, is_error)             \
  if(!sink->d3d.window_handle) {                                    \
    GST_CAT_LEVEL_LOG(GST_CAT_DEFAULT,                              \
                      (is_error?GST_LEVEL_ERROR:GST_LEVEL_DEBUG),   \
                      sink, "No window handle is set");             \
    goto goto_label;                                                \
  }

#ifndef D3DFMT_YV12
#define D3DFMT_YV12 MAKEFOURCC ('Y', 'V', '1', '2')
#endif
#ifndef D3DFMT_NV12
#define D3DFMT_NV12 MAKEFOURCC ('N', 'V', '1', '2')
#endif

/** FORMATS **/

#define CASE(x) case x: return #x;
static const gchar *
d3d_format_to_string (D3DFORMAT format)
{
  /* Self defined up above */
  if (format == D3DFMT_YV12)
    return "D3DFMT_YV12";
  else if (format == D3DFMT_NV12)
    return "D3DFMT_NV12";

  switch (format) {
      /* From D3D enum */
      CASE (D3DFMT_UNKNOWN);
      CASE (D3DFMT_X8R8G8B8);
      CASE (D3DFMT_YUY2);
      CASE (D3DFMT_A8R8G8B8);
      CASE (D3DFMT_UYVY);
      CASE (D3DFMT_R8G8B8);
      CASE (D3DFMT_R5G6B5);
      CASE (D3DFMT_X1R5G5B5);
      CASE (D3DFMT_A1R5G5B5);
      CASE (D3DFMT_A4R4G4B4);
      CASE (D3DFMT_R3G3B2);
      CASE (D3DFMT_A8);
      CASE (D3DFMT_A8R3G3B2);
      CASE (D3DFMT_X4R4G4B4);
      CASE (D3DFMT_A2B10G10R10);
      CASE (D3DFMT_A8B8G8R8);
      CASE (D3DFMT_X8B8G8R8);
      CASE (D3DFMT_G16R16);
      CASE (D3DFMT_A2R10G10B10);
      CASE (D3DFMT_A16B16G16R16);
      CASE (D3DFMT_A8P8);
      CASE (D3DFMT_P8);
      CASE (D3DFMT_L8);
      CASE (D3DFMT_A8L8);
      CASE (D3DFMT_A4L4);
      CASE (D3DFMT_V8U8);
      CASE (D3DFMT_L6V5U5);
      CASE (D3DFMT_X8L8V8U8);
      CASE (D3DFMT_Q8W8V8U8);
      CASE (D3DFMT_V16U16);
      CASE (D3DFMT_A2W10V10U10);
      CASE (D3DFMT_DXT1);
      CASE (D3DFMT_DXT2);
      CASE (D3DFMT_DXT3);
      CASE (D3DFMT_DXT4);
      CASE (D3DFMT_DXT5);
      CASE (D3DFMT_MULTI2_ARGB8);
      CASE (D3DFMT_G8R8_G8B8);
      CASE (D3DFMT_R8G8_B8G8);
      CASE (D3DFMT_D16_LOCKABLE);
      CASE (D3DFMT_D32);
      CASE (D3DFMT_D15S1);
      CASE (D3DFMT_D24S8);
      CASE (D3DFMT_D24X8);
      CASE (D3DFMT_D24X4S4);
      CASE (D3DFMT_D16);
      CASE (D3DFMT_L16);
      CASE (D3DFMT_D32F_LOCKABLE);
      CASE (D3DFMT_D24FS8);
      CASE (D3DFMT_VERTEXDATA);
      CASE (D3DFMT_INDEX16);
      CASE (D3DFMT_INDEX32);
      CASE (D3DFMT_Q16W16V16U16);
      CASE (D3DFMT_R16F);
      CASE (D3DFMT_G16R16F);
      CASE (D3DFMT_A16B16G16R16F);
      CASE (D3DFMT_R32F);
      CASE (D3DFMT_G32R32F);
      CASE (D3DFMT_A32B32G32R32F);
      CASE (D3DFMT_CxV8U8);
      CASE (D3DFMT_FORCE_DWORD);
  }

  return "UNKNOWN";
}

#undef CASE

static const struct
{
  GstVideoFormat gst_format;
  D3DFORMAT d3d_format;
} gst_d3d_format_map[] = {
  {
  GST_VIDEO_FORMAT_BGRx, D3DFMT_X8R8G8B8}, {
  GST_VIDEO_FORMAT_RGBx, D3DFMT_X8B8G8R8}, {
  GST_VIDEO_FORMAT_BGRA, D3DFMT_A8R8G8B8}, {
  GST_VIDEO_FORMAT_RGBA, D3DFMT_A8B8G8R8}, {
  GST_VIDEO_FORMAT_BGR, D3DFMT_R8G8B8}, {
  GST_VIDEO_FORMAT_RGB16, D3DFMT_R5G6B5}, {
  GST_VIDEO_FORMAT_RGB15, D3DFMT_X1R5G5B5}, {
  GST_VIDEO_FORMAT_I420, D3DFMT_YV12}, {
  GST_VIDEO_FORMAT_YV12, D3DFMT_YV12}, {
  GST_VIDEO_FORMAT_NV12, D3DFMT_NV12}, {
  GST_VIDEO_FORMAT_YUY2, D3DFMT_YUY2}, {
  GST_VIDEO_FORMAT_UYVY, D3DFMT_UYVY}
};

static D3DFORMAT
gst_video_format_to_d3d_format (GstVideoFormat format)
{
  gint i;

  for (i = 0; i < G_N_ELEMENTS (gst_d3d_format_map); i++)
    if (gst_d3d_format_map[i].gst_format == format)
      return gst_d3d_format_map[i].d3d_format;
  return D3DFMT_UNKNOWN;
}

static gboolean
gst_video_d3d_format_check (GstD3DVideoSink * sink, D3DFORMAT fmt)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  HRESULT hr;
  gboolean ret = FALSE;

  hr = IDirect3D9_CheckDeviceFormat (class->d3d.d3d,
      class->d3d.device.adapter,
      D3DDEVTYPE_HAL, class->d3d.device.format, 0, D3DRTYPE_SURFACE, fmt);
  if (hr == D3D_OK) {
    /* test whether device can perform color-conversion
     * from that format to target format
     */
    hr = IDirect3D9_CheckDeviceFormatConversion (class->d3d.d3d,
        class->d3d.device.adapter,
        D3DDEVTYPE_HAL, fmt, class->d3d.device.format);
    if (hr == D3D_OK)
      ret = TRUE;
  }
  GST_DEBUG_OBJECT (sink, "Checking: %s - %s", d3d_format_to_string (fmt),
      ret ? "TRUE" : "FALSE");

  return ret;
}

static gboolean
gst_video_query_d3d_format (GstD3DVideoSink * sink, D3DFORMAT d3dformat)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);

  /* If it's the display adapter format we don't need to probe */
  if (d3dformat == class->d3d.device.format)
    return TRUE;

  if (gst_video_d3d_format_check (sink, d3dformat))
    return TRUE;

  return FALSE;
}

typedef struct
{
  GstVideoFormat fmt;
  D3DFORMAT d3d_fmt;
  gboolean display;
} D3DFormatComp;

static void
d3d_format_comp_free (D3DFormatComp * comp)
{
  g_slice_free (D3DFormatComp, comp);
}

static gint
d3d_format_comp_rate (const D3DFormatComp * comp)
{
  gint points = 0;
  const GstVideoFormatInfo *info;

  info = gst_video_format_get_info (comp->fmt);

  if (comp->display)
    points += 10;
  if (GST_VIDEO_FORMAT_INFO_IS_YUV (info))
    points += 5;
  else if (GST_VIDEO_FORMAT_INFO_IS_RGB (info)) {
    guint i, bit_depth = 0;
    for (i = 0; i < GST_VIDEO_FORMAT_INFO_N_COMPONENTS (info); i++)
      bit_depth += GST_VIDEO_FORMAT_INFO_DEPTH (info, i);
    if (bit_depth >= 24)
      points += 1;
  }

  return points;
}

static gint
d3d_format_comp_compare (gconstpointer a, gconstpointer b)
{
  gint ptsa = 0, ptsb = 0;

  ptsa = d3d_format_comp_rate ((const D3DFormatComp *) a);
  ptsb = d3d_format_comp_rate ((const D3DFormatComp *) b);

  if (ptsa < ptsb)
    return -1;
  else if (ptsa == ptsb)
    return 0;
  else
    return 1;
}

GstCaps *
d3d_supported_caps (GstD3DVideoSink * sink)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  int i;
  GList *fmts = NULL, *l;
  GstCaps *caps = NULL;
  GstVideoFormat gst_format;
  D3DFORMAT d3d_format;
  GValue va = { 0, };
  GValue v = { 0, };

  LOCK_SINK (sink);

  if (sink->supported_caps) {
    caps = gst_caps_ref (sink->supported_caps);
    goto unlock;
  }

  for (i = 0; i < G_N_ELEMENTS (gst_d3d_format_map); i++) {
    D3DFormatComp *comp;

    gst_format = gst_d3d_format_map[i].gst_format;
    d3d_format = gst_d3d_format_map[i].d3d_format;
    if (!gst_video_query_d3d_format (sink, d3d_format))
      continue;

    comp = g_slice_new0 (D3DFormatComp);
    comp->fmt = (GstVideoFormat) gst_format;
    comp->d3d_fmt = d3d_format;
    comp->display = (d3d_format == class->d3d.device.format);
    fmts = g_list_insert_sorted (fmts, comp, d3d_format_comp_compare);
  }

  GST_DEBUG_OBJECT (sink, "Supported Caps:");

  g_value_init (&va, GST_TYPE_LIST);
  g_value_init (&v, G_TYPE_STRING);

  for (l = fmts; l; l = g_list_next (l)) {
    D3DFormatComp *comp = (D3DFormatComp *) l->data;

    GST_DEBUG_OBJECT (sink, "%s -> %s %s",
        gst_video_format_to_string (comp->fmt),
        d3d_format_to_string (comp->d3d_fmt), comp->display ? "[display]" : "");
    g_value_set_string (&v, gst_video_format_to_string (comp->fmt));
    gst_value_list_append_value (&va, &v);
  }

  caps = gst_caps_new_simple ("video/x-raw",
      "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
      "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
      "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
  gst_caps_set_value (caps, "format", &va);
  g_value_unset (&v);
  g_value_unset (&va);
  g_list_free_full (fmts, (GDestroyNotify) d3d_format_comp_free);

  sink->supported_caps = gst_caps_ref (caps);

#ifndef GST_DISABLE_GST_DEBUG
  {
    gchar *tmp = gst_caps_to_string (caps);
    GST_DEBUG_OBJECT (sink, "Supported caps: %s", tmp);
    g_free (tmp);
  }
#endif

unlock:
  UNLOCK_SINK (sink);

  return caps;
}

gboolean
d3d_set_render_format (GstD3DVideoSink * sink)
{
  D3DFORMAT fmt;
  gboolean ret = FALSE;

  LOCK_SINK (sink);

  fmt = gst_video_format_to_d3d_format (sink->format);
  if (fmt == D3DFMT_UNKNOWN) {
    GST_ERROR_OBJECT (sink, "Unsupported video format %s",
        gst_video_format_to_string (sink->format));
    goto end;
  }

  if (!gst_video_query_d3d_format (sink, fmt)) {
    GST_ERROR_OBJECT (sink, "Failed to query a D3D render format for %s",
        gst_video_format_to_string (sink->format));
    goto end;
  }

  GST_DEBUG_OBJECT (sink, "Selected %s -> %s",
      gst_video_format_to_string (sink->format), d3d_format_to_string (fmt));

  sink->d3d.format = fmt;

  ret = TRUE;

end:
  UNLOCK_SINK (sink);

  return ret;
}

static gboolean
d3d_get_hwnd_window_size (HWND hwnd, gint * width, gint * height)
{
  RECT sz;

  g_return_val_if_fail (width != NULL, FALSE);
  g_return_val_if_fail (height != NULL, FALSE);

  *width = 0;
  *height = 0;

  if (!hwnd)
    return FALSE;

  GetClientRect (hwnd, &sz);

  *width = MAX (1, ABS (sz.right - sz.left));
  *height = MAX (1, ABS (sz.bottom - sz.top));

  return TRUE;
}

static gboolean
d3d_get_render_rects (GstVideoRectangle * rr, RECT * dst, RECT * src)
{
  if (!rr)
    return FALSE;

  /* Rect on target */
  if (dst) {
    dst->left = rr->x;
    dst->top = rr->y;
    dst->right = rr->x + rr->w;
    dst->bottom = rr->y + rr->h;
  }

  /* Rect on source */
  if (src) {
    src->left = 0;
    src->top = 0;
    src->right = rr->w;
    src->bottom = rr->h;
  }

  return TRUE;
}

static gboolean
d3d_get_render_coordinates (GstD3DVideoSink * sink, gint in_x, gint in_y,
    gdouble * out_x, gdouble * out_y)
{
  GstVideoRectangle r_area;
  gdouble tmp;
  gboolean ret = FALSE;

  g_return_val_if_fail (out_x != NULL, FALSE);
  g_return_val_if_fail (out_y != NULL, FALSE);

  LOCK_SINK (sink);
  CHECK_WINDOW_HANDLE (sink, end, FALSE);

  /* Get renderable area of the window */
  if (sink->d3d.render_rect) {
    memcpy (&r_area, sink->d3d.render_rect, sizeof (r_area));
  } else {
    memset (&r_area, 0, sizeof (r_area));
    d3d_get_hwnd_window_size (sink->d3d.window_handle, &r_area.w, &r_area.h);
  }

  /* If window coords outside render area.. return */
  if (in_x < r_area.x || in_x > r_area.x + r_area.w ||
      in_y < r_area.y || in_y > r_area.y + r_area.h)
    goto end;

  /* Convert window coordinates to source frame pixel coordinates */
  if (sink->force_aspect_ratio) {
    GstVideoRectangle tmp = { 0, 0, 0, 0 };
    GstVideoRectangle dst = { 0, 0, 0, 0 };

    tmp.w = GST_VIDEO_SINK_WIDTH (sink);
    tmp.h = GST_VIDEO_SINK_HEIGHT (sink);
    gst_video_sink_center_rect (tmp, r_area, &dst, TRUE);

    r_area.x = r_area.x + dst.x;
    r_area.y = r_area.y + dst.y;
    r_area.w = dst.w;
    r_area.h = dst.h;

    /* If window coords outside render area.. return */
    if (in_x < r_area.x || in_x > (r_area.x + r_area.w) ||
        in_y < r_area.y || in_y > (r_area.y + r_area.h))
      goto end;
  }

  tmp = in_x - r_area.x;
  if (r_area.w == GST_VIDEO_SINK_WIDTH (sink))
    *out_x = tmp;
  else if (r_area.w > GST_VIDEO_SINK_WIDTH (sink))
    *out_x =
        ((gdouble) tmp / ((gdouble) r_area.w /
            (gdouble) GST_VIDEO_SINK_WIDTH (sink)));
  else
    *out_x =
        ((gdouble) GST_VIDEO_SINK_WIDTH (sink) / (gdouble) r_area.w) *
        (gdouble) tmp;

  tmp = in_y - r_area.y;
  if (r_area.h == GST_VIDEO_SINK_HEIGHT (sink))
    *out_y = tmp;
  else if (r_area.h > GST_VIDEO_SINK_HEIGHT (sink))
    *out_y =
        ((gdouble) tmp / ((gdouble) r_area.h /
            (gdouble) GST_VIDEO_SINK_HEIGHT (sink)));
  else
    *out_y =
        ((gdouble) GST_VIDEO_SINK_HEIGHT (sink) / (gdouble) r_area.h) *
        (gdouble) tmp;

  ret = TRUE;
end:
  UNLOCK_SINK (sink);
  return ret;
}

/** Windows for rendering (User Set or Internal) **/

static void
d3d_window_wndproc_unset (GstD3DVideoSink * sink)
{
  WNDPROC cur_wnd_proc = NULL;

  LOCK_SINK (sink);

  GST_DEBUG_OBJECT (sink, " ");

  if (sink->d3d.window_handle == NULL) {
    GST_WARNING_OBJECT (sink, "D3D window_handle is NULL");
    goto end;
  }

  cur_wnd_proc =
      (WNDPROC) GetWindowLongPtr (sink->d3d.window_handle, GWLP_WNDPROC);

  if (cur_wnd_proc != d3d_wnd_proc) {
    GST_WARNING_OBJECT (sink, "D3D window proc is not set on current window");
    goto end;
  }

  if (sink->d3d.orig_wnd_proc == NULL) {
    GST_WARNING_OBJECT (sink, "D3D orig window proc is NULL, can not restore");
    goto end;
  }

  /* Restore orignal WndProc for window_handle */
  if (!SetWindowLongPtr (sink->d3d.window_handle, GWLP_WNDPROC,
          (LONG_PTR) sink->d3d.orig_wnd_proc)) {
    GST_WARNING_OBJECT (sink, "D3D failed to set original WndProc");
    goto end;
  }

end:
  sink->d3d.orig_wnd_proc = NULL;
  sink->d3d.window_handle = NULL;

  UNLOCK_SINK (sink);
}

static gboolean
d3d_window_wndproc_set (GstD3DVideoSink * sink)
{
  WNDPROC cur_wnd_proc;
  gboolean ret = FALSE;

  LOCK_SINK (sink);

  cur_wnd_proc =
      (WNDPROC) GetWindowLongPtr (sink->d3d.window_handle, GWLP_WNDPROC);

  if (cur_wnd_proc != NULL && cur_wnd_proc == d3d_wnd_proc) {
    GST_DEBUG_OBJECT (sink,
        "D3D window proc func is already set on the current window");
    ret = TRUE;
    goto end;
  }

  /* Store the original window proc function */
  sink->d3d.orig_wnd_proc =
      (WNDPROC) SetWindowLongPtr (sink->d3d.window_handle, GWLP_WNDPROC,
      (LONG_PTR) d3d_wnd_proc);

  /* Note: If the window belongs to another process this will fail */
  if (sink->d3d.orig_wnd_proc == NULL) {
    GST_ERROR_OBJECT (sink,
        "Failed to set WndProc function on window. Error: %d",
        (gint) GetLastError ());
    goto end;
  }

  /* Make sink accessible to d3d_wnd_proc */
  SetProp (sink->d3d.window_handle, TEXT ("GstD3DVideoSink"), sink);

  ret = TRUE;

end:
  UNLOCK_SINK (sink);
  return ret;
}

static void
d3d_prepare_render_window (GstD3DVideoSink * sink)
{
  LOCK_SINK (sink);

  if (sink->d3d.window_handle == NULL) {
    GST_DEBUG_OBJECT (sink, "No window handle has been set..");
    goto end;
  }

  if (d3d_init_swap_chain (sink, sink->d3d.window_handle)) {
    d3d_window_wndproc_set (sink);
    sink->d3d.renderable = TRUE;
    GST_DEBUG_OBJECT (sink, "Prepared window for render [HWND:%p]",
        sink->d3d.window_handle);
  } else {
    GST_ERROR_OBJECT (sink, "Failed preparing window for render [HWND:%p]",
        sink->d3d.window_handle);
  }

end:
  UNLOCK_SINK (sink);

}

void
d3d_set_window_handle (GstD3DVideoSink * sink, guintptr window_id,
    gboolean is_internal)
{
  LOCK_SINK (sink);

  if (sink->d3d.window_handle == (HWND) window_id) {
    GST_WARNING_OBJECT (sink, "Window HWND already set to: %" G_GUINTPTR_FORMAT,
        window_id);
    goto end;
  }

  /* Unset current window  */
  if (sink->d3d.window_handle != NULL) {
    GST_DEBUG_OBJECT (sink, "Unsetting window [HWND:%p]",
        sink->d3d.window_handle);
    d3d_window_wndproc_unset (sink);
    d3d_release_swap_chain (sink);
    sink->d3d.window_handle = NULL;
    sink->d3d.window_is_internal = FALSE;
    sink->d3d.renderable = FALSE;
  }

  /* Set new one */
  if (window_id) {
    sink->d3d.window_handle = (HWND) window_id;
    sink->d3d.window_is_internal = is_internal;
    /* If caps have been set.. prepare window */
    if (sink->format != 0)
      d3d_prepare_render_window (sink);
  }

end:
  UNLOCK_SINK (sink);
}

void
d3d_set_render_rectangle (GstD3DVideoSink * sink)
{
  LOCK_SINK (sink);
  /* Setting the pointer lets us know render rect is set */
  sink->d3d.render_rect = &sink->render_rect;
  d3d_resize_swap_chain (sink);
  d3d_present_swap_chain (sink);
  UNLOCK_SINK (sink);
}

void
d3d_expose_window (GstD3DVideoSink * sink)
{
  GST_DEBUG_OBJECT (sink, "EXPOSE");
  d3d_present_swap_chain (sink);
}

gboolean
d3d_prepare_window (GstD3DVideoSink * sink)
{
  HWND hWnd;
  gboolean ret = FALSE;

  LOCK_SINK (sink);

  /* Give the app a last chance to set a window id */
  if (!sink->d3d.window_handle)
    gst_video_overlay_prepare_window_handle (GST_VIDEO_OVERLAY (sink));

  /* If the user did not set a window id .. check if we should create one */
  if (!sink->d3d.window_handle) {
    if (sink->create_internal_window) {
      if ((hWnd = d3d_create_internal_window (sink))) {
        GST_DEBUG_OBJECT (sink,
            "No window id was set.. creating internal window");
        d3d_set_window_handle (sink, (guintptr) hWnd, TRUE);
      } else {
        GST_ERROR_OBJECT (sink, "Failed to create internal window");
        goto end;
      }
    } else {
      GST_DEBUG_OBJECT (sink, "No window id is set..");
      goto end;
    }
  } else {
    d3d_prepare_render_window (sink);
  }

  ret = TRUE;

end:
  UNLOCK_SINK (sink);

  return ret;
}

gboolean
d3d_stop (GstD3DVideoSink * sink)
{
  /* Release D3D resources */
  d3d_set_window_handle (sink, 0, FALSE);
  return TRUE;
}

/** D3D Lost and Reset Device **/

static void
d3d_notify_device_lost (GstD3DVideoSink * sink)
{
  gboolean notify = FALSE;

  g_return_if_fail (GST_IS_D3DVIDEOSINK (sink));

  LOCK_SINK (sink);

  if (!sink->d3d.device_lost) {
    GST_WARNING_OBJECT (sink,
        "D3D Device has been lost. Cleanup up resources..");

    /* Stream will continue with GST_FLOW_OK, until device has been reset */
    sink->d3d.device_lost = TRUE;

    /* First we clean up all resources in this d3dvideo instance */
    d3d_release_swap_chain (sink);

    /* Notify our hidden thread */
    notify = TRUE;
  }

  UNLOCK_SINK (sink);

  if (notify)
    d3d_class_notify_device_lost (sink);
}

static void
d3d_notify_device_reset (GstD3DVideoSink * sink)
{
  LOCK_SINK (sink);

  if (sink->d3d.device_lost) {
    GST_DEBUG_OBJECT (sink,
        "D3D Device has been reset. Re-init swap chain if still streaming");
    /* If we're still streaming.. reset swap chain */
    if (sink->d3d.window_handle != NULL)
      d3d_init_swap_chain (sink, sink->d3d.window_handle);
    sink->d3d.device_lost = FALSE;
  }

  UNLOCK_SINK (sink);
}

/** Swap Chains **/

static gboolean
d3d_init_swap_chain (GstD3DVideoSink * sink, HWND hWnd)
{
  D3DPRESENT_PARAMETERS present_params;
  LPDIRECT3DSWAPCHAIN9 d3d_swapchain = NULL;
  LPDIRECT3DSURFACE9 d3d_surface = NULL;
  D3DTEXTUREFILTERTYPE d3d_filtertype;
  HRESULT hr;
  GstD3DVideoSinkClass *class;
  gboolean ret = FALSE;

  g_return_val_if_fail (sink != NULL, FALSE);
  class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  g_return_val_if_fail (class != NULL, FALSE);

  LOCK_SINK (sink);
  LOCK_CLASS (sink, class);

  /* We need a display device */
  CHECK_D3D_DEVICE (class, sink, error);

  GST_DEBUG ("Initializing Direct3D swap chain");

  GST_DEBUG ("Direct3D back buffer size: %dx%d", GST_VIDEO_SINK_WIDTH (sink),
      GST_VIDEO_SINK_HEIGHT (sink));

  /* When windowed, width and height determined by HWND */
  ZeroMemory (&present_params, sizeof (present_params));
  present_params.Windowed = TRUE;
  present_params.SwapEffect = D3DSWAPEFFECT_DISCARD;    //D3DSWAPEFFECT_COPY
  present_params.hDeviceWindow = hWnd;
  present_params.BackBufferFormat = class->d3d.device.format;   //d3d_format;

  hr = IDirect3DDevice9_CreateAdditionalSwapChain (class->d3d.device.d3d_device,
      &present_params, &d3d_swapchain);
  ERROR_CHECK_HR (hr) {
    CASE_HR_ERR (D3DERR_NOTAVAILABLE);
    CASE_HR_ERR (D3DERR_DEVICELOST);
    CASE_HR_ERR (D3DERR_INVALIDCALL);
    CASE_HR_ERR (D3DERR_OUTOFVIDEOMEMORY);
    CASE_HR_ERR (E_OUTOFMEMORY);
    CASE_HR_ERR_END (sink, "Error creating D3D swapchian");
    goto error;
  }

  hr = IDirect3DDevice9_CreateOffscreenPlainSurface (class->d3d.
      device.d3d_device, GST_VIDEO_SINK_WIDTH (sink),
      GST_VIDEO_SINK_HEIGHT (sink), sink->d3d.format, D3DPOOL_DEFAULT,
      &d3d_surface, NULL);
  if (hr != D3D_OK) {
    GST_ERROR_OBJECT (sink, "Failed to create D3D surface");
    goto error;
  }

  /* Determine texture filtering support. If it's supported for this format,
   * use the filter type determined when we created the dev and checked the 
   * dev caps.
   */
  hr = IDirect3D9_CheckDeviceFormat (class->d3d.d3d,
      class->d3d.device.adapter,
      D3DDEVTYPE_HAL,
      class->d3d.device.format,
      D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, sink->d3d.format);
  if (hr == D3D_OK)
    d3d_filtertype = class->d3d.device.filter_type;
  else
    d3d_filtertype = D3DTEXF_NONE;

  GST_DEBUG ("Direct3D stretch rect texture filter: %d", d3d_filtertype);

  sink->d3d.filtertype = d3d_filtertype;
  sink->d3d.swapchain = d3d_swapchain;
  sink->d3d.surface = d3d_surface;

  ret = TRUE;

error:
  if (!ret) {
    if (d3d_swapchain)
      IDirect3DSwapChain9_Release (d3d_swapchain);
    if (d3d_surface)
      IDirect3DSurface9_Release (d3d_surface);
  }

  UNLOCK_CLASS (sink, class);
  UNLOCK_SINK (sink);

  return ret;
}

static gboolean
d3d_release_swap_chain (GstD3DVideoSink * sink)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  int ref_count;
  gboolean ret = FALSE;

  LOCK_SINK (sink);

  GST_DEBUG_OBJECT (sink, "Releasing Direct3D swap chain");

  CHECK_D3D_DEVICE (class, sink, end);

  if (!sink->d3d.swapchain && !sink->d3d.surface) {
    ret = TRUE;
    goto end;
  }

  if (sink->d3d.surface) {
    ref_count = IDirect3DSurface9_Release (sink->d3d.surface);
    if (ref_count > 0)
      while (IDirect3DSurface9_Release (sink->d3d.surface) > 0);
    sink->d3d.surface = NULL;
    GST_DEBUG_OBJECT (sink, "D3D surface released. Ref count: %d", ref_count);
  }

  if (sink->d3d.swapchain) {
    ref_count = IDirect3DSwapChain9_Release (sink->d3d.swapchain);
    if (ref_count > 0)
      while (IDirect3DSwapChain9_Release (sink->d3d.swapchain) > 0);
    sink->d3d.swapchain = NULL;
    GST_DEBUG_OBJECT (sink, "D3D swapchain released. Ref count: %d", ref_count);
  }

  ret = TRUE;

end:
  UNLOCK_SINK (sink);

  return ret;
}

static gboolean
d3d_resize_swap_chain (GstD3DVideoSink * sink)
{
  GstD3DVideoSinkClass *class;
  D3DPRESENT_PARAMETERS d3d_pp;
  LPDIRECT3DSWAPCHAIN9 swapchain = NULL;
  gint w = 0, h = 0, ref_count = 0;
  gboolean ret = FALSE;
  HRESULT hr;
  gboolean need_new = FALSE;

  g_return_val_if_fail (sink != NULL, FALSE);
  class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  g_return_val_if_fail (class != NULL, FALSE);

  LOCK_SINK (sink);

  if (!sink->d3d.renderable || sink->d3d.device_lost) {
    UNLOCK_SINK (sink);
    return FALSE;
  }

  LOCK_CLASS (sink, class);

  CHECK_WINDOW_HANDLE (sink, end, FALSE);
  CHECK_D3D_DEVICE (class, sink, end);
  CHECK_D3D_SWAPCHAIN (sink, end);

  d3d_get_hwnd_window_size (sink->d3d.window_handle, &w, &h);
  ZeroMemory (&d3d_pp, sizeof (d3d_pp));

  /* Get the parameters used to create this swap chain */
  hr = IDirect3DSwapChain9_GetPresentParameters (sink->d3d.swapchain, &d3d_pp);
  if (hr != D3D_OK) {
    GST_ERROR_OBJECT (sink,
        "Unable to determine Direct3D present parameters for swap chain");
    goto end;
  }

  /* Reisze needed? */
  if (d3d_pp.BackBufferWidth != w || d3d_pp.BackBufferHeight != h)
    need_new = TRUE;
#if 0
  /* Render rect set or unset? */
  if ((d3d_pp.SwapEffect != D3DSWAPEFFECT_COPY && sink->d3d.render_rect) ||
      (d3d_pp.SwapEffect != D3DSWAPEFFECT_DISCARD
          && sink->d3d.render_rect == NULL)) {
    d3d_pp.SwapEffect =
        (sink->d3d.render_rect ==
        NULL) ? D3DSWAPEFFECT_DISCARD : D3DSWAPEFFECT_COPY;
    GST_DEBUG_OBJECT (sink, "Setting SwapEffect: %s",
        sink->d3d.render_rect ? "COPY" : "DISCARD");
    need_new = TRUE;
  }
#endif
  if (!need_new) {
    ret = TRUE;
    goto end;
  }

  GST_DEBUG_OBJECT (sink, "Resizing swapchain %dx%d to %dx%d",
      d3d_pp.BackBufferWidth, d3d_pp.BackBufferHeight, w, h);


  /* As long as present params windowed == TRUE, width or height
   * of 0 will force use of HWND's size.
   */
  d3d_pp.BackBufferWidth = 0;
  d3d_pp.BackBufferHeight = 0;

  /* Release current swapchain */
  if (sink->d3d.swapchain != NULL) {
    ref_count = IDirect3DSwapChain9_Release (sink->d3d.swapchain);
    if (ref_count > 0)
      while (IDirect3DSwapChain9_Release (sink->d3d.swapchain) > 0);
    if (ref_count > 0) {
      GST_WARNING_OBJECT (sink, "Release swapchain refcount: %d", ref_count);
    }
    sink->d3d.swapchain = NULL;
  }

  hr = IDirect3DDevice9_CreateAdditionalSwapChain (class->d3d.device.d3d_device,
      &d3d_pp, &swapchain);
  ERROR_CHECK_HR (hr) {
    CASE_HR_ERR (D3DERR_NOTAVAILABLE);
    CASE_HR_ERR (D3DERR_DEVICELOST);
    CASE_HR_ERR (D3DERR_INVALIDCALL);
    CASE_HR_ERR (D3DERR_OUTOFVIDEOMEMORY);
    CASE_HR_ERR (E_OUTOFMEMORY);
    CASE_HR_ERR_END (sink, "Error creating swapchian");
    goto end;
  }

  sink->d3d.swapchain = swapchain;
  ret = TRUE;

end:
  UNLOCK_CLASS (sink, class);
  UNLOCK_SINK (sink);

  return ret;
}

static gboolean
d3d_copy_buffer_to_surface (GstD3DVideoSink * sink, GstBuffer * buffer)
{
  D3DLOCKED_RECT lr;
  guint8 *dest;
  int deststride;
  gboolean ret = FALSE;
  gint unhdl_line = 0;
  GstVideoFrame frame;
  LOCK_SINK (sink);

  if (!sink->d3d.renderable || sink->d3d.device_lost)
    goto end;

  if (!buffer
      || !gst_video_frame_map (&frame, &sink->info, buffer, GST_MAP_READ)) {
    GST_ERROR_OBJECT (sink, "NULL GstBuffer");
    goto end;
  }

  CHECK_D3D_SURFACE (sink, end);

  IDirect3DSurface9_LockRect (sink->d3d.surface, &lr, NULL, 0);
  dest = (guint8 *) lr.pBits;

  if (!dest) {
    GST_ERROR_OBJECT (sink, "No D3D surface dest buffer");
    goto unlock_surface;
  }

  deststride = lr.Pitch;

  switch (sink->format) {
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_UYVY:{
      const guint8 *src;
      gint srcstride;
      gint i, h, w;

      src = GST_VIDEO_FRAME_PLANE_DATA (&frame, 0);
      srcstride = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);
      h = GST_VIDEO_FRAME_HEIGHT (&frame);
      w = GST_ROUND_UP_4 (GST_VIDEO_FRAME_WIDTH (&frame) * 2);

      for (i = 0; i < h; i++) {
        memcpy (dest, src, w);
        dest += deststride;
        src += srcstride;
      }

      break;
    }
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YV12:{
      const guint8 *src;
      gint srcstride, deststride_;
      guint8 *dest_;
      gint i, j, h, h_, w_;

      h = GST_VIDEO_FRAME_HEIGHT (&frame);

      for (i = 0; i < 3; i++) {
        src = GST_VIDEO_FRAME_COMP_DATA (&frame, i);
        srcstride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, i);
        h_ = GST_VIDEO_FRAME_COMP_HEIGHT (&frame, i);
        w_ = GST_VIDEO_FRAME_COMP_WIDTH (&frame, i);

        switch (i) {
          case 0:
            deststride_ = deststride;
            dest_ = dest;
            break;
          case 2:
            deststride_ = deststride / 2;
            dest_ = dest + h * deststride;
            break;
          case 1:
            deststride_ = deststride / 2;
            dest_ = dest + h * deststride + h_ * deststride_;
            break;
        }

        for (j = 0; j < h_; j++) {
          memcpy (dest_, src, w_);
          dest_ += deststride_;
          src += srcstride;
        }
      }

      break;
    }
    case GST_VIDEO_FORMAT_NV12:{
      const guint8 *src;
      gint srcstride;
      guint8 *dest_;
      gint i, j, h, h_, w_;

      h = GST_VIDEO_FRAME_HEIGHT (&frame);

      for (i = 0; i < 2; i++) {
        src = GST_VIDEO_FRAME_PLANE_DATA (&frame, i);
        srcstride = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, i);
        h_ = GST_VIDEO_FRAME_COMP_HEIGHT (&frame, i);
        w_ = GST_VIDEO_FRAME_COMP_WIDTH (&frame, i);

        switch (i) {
          case 0:
            dest_ = dest;
            break;
          case 1:
            dest_ = dest + h * deststride;
            break;
        }

        for (j = 0; j < h_; j++) {
          memcpy (dest_, src, w_ * 2);
          dest_ += deststride;
          src += srcstride;
        }
      }

      break;
    }
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_RGBx:{
      const guint8 *src;
      gint srcstride;
      gint i, h, w;

      src = GST_VIDEO_FRAME_PLANE_DATA (&frame, 0);
      srcstride = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);
      h = GST_VIDEO_FRAME_HEIGHT (&frame);
      w = GST_VIDEO_FRAME_WIDTH (&frame) * 4;

      for (i = 0; i < h; i++) {
        memcpy (dest, src, w);
        dest += deststride;
        src += srcstride;
      }

      break;
    }
    case GST_VIDEO_FORMAT_BGR:{
      const guint8 *src;
      gint srcstride;
      gint i, h, w;

      src = GST_VIDEO_FRAME_PLANE_DATA (&frame, 0);
      srcstride = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);
      h = GST_VIDEO_FRAME_HEIGHT (&frame);
      w = GST_VIDEO_FRAME_WIDTH (&frame) * 3;

      for (i = 0; i < h; i++) {
        memcpy (dest, src, w);
        dest += deststride;
        src += srcstride;
      }

      break;
    }
    case GST_VIDEO_FORMAT_RGB16:
    case GST_VIDEO_FORMAT_RGB15:{
      const guint8 *src;
      gint srcstride;
      gint i, h, w;

      src = GST_VIDEO_FRAME_PLANE_DATA (&frame, 0);
      srcstride = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);
      h = GST_VIDEO_FRAME_HEIGHT (&frame);
      w = GST_VIDEO_FRAME_WIDTH (&frame) * 2;

      for (i = 0; i < h; i++) {
        memcpy (dest, src, w);
        dest += deststride;
        src += srcstride;
      }

      break;
    }
    default:
      unhdl_line = __LINE__;
      goto unhandled_format;
  }

  goto done;

unhandled_format:
  GST_ERROR_OBJECT (sink,
      "Unhandled format [LN:%d] '%s' -> '%s' (should not get here)", unhdl_line,
      gst_video_format_to_string (sink->format),
      d3d_format_to_string (sink->d3d.format));
  goto unlock_surface;

done:
  ret = TRUE;
unlock_surface:
  IDirect3DSurface9_UnlockRect (sink->d3d.surface);
  gst_video_frame_unmap (&frame);

end:
  UNLOCK_SINK (sink);
  return ret;
}

static gboolean
d3d_present_swap_chain (GstD3DVideoSink * sink)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  LPDIRECT3DSURFACE9 back_buffer = NULL;
  gboolean ret = FALSE;
  HRESULT hr;
  RECT dstr, srcr, *pDestRect = NULL, *pSrcRect = NULL;

  LOCK_SINK (sink);

  if (!sink->d3d.renderable || sink->d3d.device_lost) {
    UNLOCK_SINK (sink);
    return FALSE;
  }

  LOCK_CLASS (sink, class);

  CHECK_WINDOW_HANDLE (sink, end, FALSE);
  CHECK_D3D_DEVICE (class, sink, end);
  CHECK_D3D_SWAPCHAIN (sink, end);

  /* Set the render target to our swap chain */
  IDirect3DSwapChain9_GetBackBuffer (sink->d3d.swapchain, 0,
      D3DBACKBUFFER_TYPE_MONO, &back_buffer);
  IDirect3DDevice9_SetRenderTarget (class->d3d.device.d3d_device, 0,
      back_buffer);
  IDirect3DSurface9_Release (back_buffer);

  /* Clear the target */
  IDirect3DDevice9_Clear (class->d3d.device.d3d_device, 0, NULL,
      D3DCLEAR_TARGET, D3DCOLOR_XRGB (0, 0, 0), 1.0f, 0);

  hr = IDirect3DDevice9_BeginScene (class->d3d.device.d3d_device);
  ERROR_CHECK_HR (hr) {
    CASE_HR_ERR (D3DERR_INVALIDCALL);
    CASE_HR_ERR_END (sink, "IDirect3DDevice9_BeginScene");
    goto end;
  }

  /* Stretch and blit ops, to copy offscreen surface buffer
   * to Display back buffer.
   */
  d3d_stretch_and_copy (sink, back_buffer);
  IDirect3DDevice9_EndScene (class->d3d.device.d3d_device);

  if (d3d_get_render_rects (sink->d3d.render_rect, &dstr, &srcr)) {
    pDestRect = &dstr;
    pSrcRect = &srcr;
  }

  /*
   * Swap back and front buffers on video card and present to the user
   */
  hr = IDirect3DSwapChain9_Present (sink->d3d.swapchain, pSrcRect, pDestRect,
      NULL, NULL, 0);
  if (hr == D3DERR_DEVICELOST) {
    d3d_notify_device_lost (sink);
    ret = TRUE;
    goto end;
  }
  ERROR_CHECK_HR (hr) {
    CASE_HR_ERR (D3DERR_DEVICELOST);
    CASE_HR_ERR (D3DERR_DRIVERINTERNALERROR);
    CASE_HR_ERR (D3DERR_INVALIDCALL);
    CASE_HR_ERR (D3DERR_OUTOFVIDEOMEMORY);
    CASE_HR_ERR (E_OUTOFMEMORY);
    CASE_HR_DBG_END (sink, "IDirect3DSwapChain9_Present failure");
    goto end;
  }

  ret = TRUE;

end:
  UNLOCK_SINK (sink);
  UNLOCK_CLASS (sink, class);
  return ret;
}

static gboolean
d3d_stretch_and_copy (GstD3DVideoSink * sink, LPDIRECT3DSURFACE9 back_buffer)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  GstVideoRectangle *render_rect = NULL;
  RECT *r_ptr = NULL;
  RECT r;
  HRESULT hr;
  gboolean ret = FALSE;

  LOCK_SINK (sink);

  CHECK_WINDOW_HANDLE (sink, end, FALSE);
  CHECK_D3D_DEVICE (class, sink, end);
  CHECK_D3D_SURFACE (sink, end);

  render_rect = sink->d3d.render_rect;

  if (sink->force_aspect_ratio) {
    gint window_width;
    gint window_height;
    GstVideoRectangle src;
    GstVideoRectangle dst;
    GstVideoRectangle result;

    memset (&dst, 0, sizeof (dst));
    memset (&src, 0, sizeof (src));

    /* Set via GstXOverlay set_render_rect */
    if (render_rect) {
      memcpy (&dst, render_rect, sizeof (dst));
    } else {
      d3d_get_hwnd_window_size (sink->d3d.window_handle, &window_width,
          &window_height);
      dst.w = window_width;
      dst.h = window_height;
    }

    src.w = GST_VIDEO_SINK_WIDTH (sink);
    src.h = GST_VIDEO_SINK_HEIGHT (sink);

    gst_video_sink_center_rect (src, dst, &result, TRUE);

    r.left = result.x;
    r.top = result.y;
    r.right = result.x + result.w;
    r.bottom = result.y + result.h;
    r_ptr = &r;
  } else if (render_rect) {
    r.left = 0;
    r.top = 0;
    r.right = render_rect->w;
    r.bottom = render_rect->h;
    r_ptr = &r;
  }

  /* TODO: StretchRect returns error if the dest rect is outside
   * the backbuffer area. So we need to calc how much of the src
   * surface is being scaled / copied to the render rect..
   */

  hr = IDirect3DDevice9_StretchRect (class->d3d.device.d3d_device, sink->d3d.surface,   /* Source Surface */
      NULL,                     /* Source Surface Rect (NULL: Whole) */
      back_buffer,              /* Dest Surface */
      r_ptr,                    /* Dest Surface Rect (NULL: Whole) */
      class->d3d.device.filter_type);

  if (hr == D3D_OK) {
    ret = TRUE;
  } else {
    GST_ERROR_OBJECT (sink, "Failure calling Direct3DDevice9_StretchRect");
  }

end:
  UNLOCK_SINK (sink);

  return ret;
}

GstFlowReturn
d3d_render_buffer (GstD3DVideoSink * sink, GstBuffer * buf)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstMapInfo map;

  g_return_val_if_fail (gst_buffer_map (buf, &map, GST_MAP_READ) != FALSE,
      GST_FLOW_ERROR);

  LOCK_SINK (sink);

  if (!sink->d3d.window_handle) {
    if (sink->stream_stop_on_close) {
      GST_LOG_OBJECT (sink, "Stopping stream. No render window");
      ret = GST_FLOW_ERROR;
    }
    goto end;
  }

  if (sink->d3d.device_lost) {
    GST_LOG_OBJECT (sink, "Device lost, waiting for reset..");
    goto end;
  }

  GST_INFO_OBJECT (sink, "%s %" GST_TIME_FORMAT,
      (sink->d3d.window_handle != NULL) ? "Render" : "No Win",
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buf)));

  /* Reize swapchain if needed */
  if (!d3d_resize_swap_chain (sink)) {
    ret = GST_FLOW_ERROR;
    goto end;
  }

  d3d_copy_buffer_to_surface (sink, buf);

  if (!d3d_present_swap_chain (sink)) {
    ret = GST_FLOW_ERROR;
    goto end;
  }

end:
  UNLOCK_SINK (sink);
  return ret;
}


/** D3D Window Proc Functions **/

static LRESULT APIENTRY
d3d_wnd_proc (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  GstD3DVideoSink *sink =
      (GstD3DVideoSink *) GetProp (hWnd, TEXT ("GstD3DVideoSink"));
  WNDPROC proc;
  LRESULT ret = 0;

  LOCK_SINK (sink);
  proc = sink->d3d.orig_wnd_proc;
  UNLOCK_SINK (sink);

  switch (message) {
    case WM_ERASEBKGND:
      return TRUE;
    case WM_PAINT:{
      if (proc)
        ret = CallWindowProc (proc, hWnd, message, wParam, lParam);
      /* Call this afterwards to ensure that our paint happens last */
      d3d_present_swap_chain (sink);
      goto end;
    }
    case WM_SIZE:{
      if (proc)
        ret = CallWindowProc (proc, hWnd, message, wParam, lParam);
      d3d_resize_swap_chain (sink);
      goto end;
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
      if (sink->enable_navigation_events) {
        gunichar2 wcrep[128];
        if (GetKeyNameTextW (lParam, (LPWSTR) wcrep, 128)) {
          gchar *utfrep = g_utf16_to_utf8 (wcrep, 128, NULL, NULL, NULL);
          if (utfrep) {
            if (message == WM_KEYDOWN)
              gst_navigation_send_key_event (GST_NAVIGATION (sink), "key-press",
                  utfrep);
            else if (message == WM_KEYUP)
              gst_navigation_send_key_event (GST_NAVIGATION (sink),
                  "key-release", utfrep);
            g_free (utfrep);
          }
        }
      }
      break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE:{
      gdouble x = 0, y = 0;
      if (sink->enable_navigation_events
          && d3d_get_render_coordinates (sink, LOWORD (lParam), HIWORD (lParam),
              &x, &y)) {
        gint button;
        const gchar *action = NULL;
        switch (message) {
          case WM_MOUSEMOVE:
            button = 0;
            action = "mouse-move";
            break;
          case WM_LBUTTONDOWN:
            button = 1;
            action = "mouse-button-press";
            break;
          case WM_LBUTTONUP:
            button = 1;
            action = "mouse-button-release";
            break;
          case WM_RBUTTONDOWN:
            button = 2;
            action = "mouse-button-press";
            break;
          case WM_RBUTTONUP:
            button = 2;
            action = "mouse-button-release";
            break;
          case WM_MBUTTONDOWN:
            button = 3;
            action = "mouse-button-press";
            break;
          case WM_MBUTTONUP:
            button = 3;
            action = "mouse-button-release";
            break;
          default:
            break;
        }
        if (action) {
          //GST_DEBUG_OBJECT(sink, "%s: %lfx%lf", action, x, y);
          gst_navigation_send_mouse_event (GST_NAVIGATION (sink), action,
              button, x, y);
        }
      }
      break;
    }
    case WM_CLOSE:
      d3d_set_window_handle (sink, 0, FALSE);
      break;
    default:
      break;
  }

  if (proc)
    ret = CallWindowProc (proc, hWnd, message, wParam, lParam);
  else
    ret = DefWindowProc (hWnd, message, wParam, lParam);

end:
  return ret;
}

/** Internal Window **/

static LRESULT APIENTRY
d3d_wnd_proc_internal (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  //GstD3DVideoSink * sink = (GstD3DVideoSink*)GetProp(hWnd, TEXT("GstD3DVideoSink"));

  switch (message) {
    case WM_DESTROY:
      GST_DEBUG ("Internal window: WM_DESTROY");
      /* Tell the internal window thread to shut down */
      PostQuitMessage (0);
      GST_DEBUG ("Posted quit..");
      break;
  }

  return DefWindowProc (hWnd, message, wParam, lParam);
}

static HWND
_d3d_create_internal_window (GstD3DVideoSink * sink)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  int width, height;
  int offx, offy;
  DWORD exstyle, style;
  HWND video_window;
  RECT rect;
  int screenwidth;
  int screenheight;

  /*
   * GST_VIDEO_SINK_WIDTH() is the aspect-ratio-corrected size of the video.
   * GetSystemMetrics() returns the width of the dialog's border (doubled
   * b/c of left and right borders).
   */
  width = GST_VIDEO_SINK_WIDTH (sink) + GetSystemMetrics (SM_CXSIZEFRAME) * 2;
  height =
      GST_VIDEO_SINK_HEIGHT (sink) + GetSystemMetrics (SM_CYCAPTION) +
      (GetSystemMetrics (SM_CYSIZEFRAME) * 2);

  SystemParametersInfo (SPI_GETWORKAREA, 0, &rect, 0);
  screenwidth = rect.right - rect.left;
  screenheight = rect.bottom - rect.top;
  offx = rect.left;
  offy = rect.top;

  /* Make it fit into the screen without changing the aspect ratio. */
  if (width > screenwidth) {
    double ratio = (double) screenwidth / (double) width;
    width = screenwidth;
    height = (int) (height * ratio);
  }

  if (height > screenheight) {
    double ratio = (double) screenheight / (double) height;
    height = screenheight;
    width = (int) (width * ratio);
  }

  style = WS_OVERLAPPEDWINDOW;  /* Normal top-level window */
  exstyle = 0;
  video_window = CreateWindowEx (exstyle,
      class->d3d.wnd_class.lpszClassName,
      TEXT ("GStreamer D3D video sink (internal window)"),
      style, offx, offy, width, height,
      NULL, NULL, class->d3d.wnd_class.hInstance, sink);

  if (video_window == NULL) {
    GST_ERROR_OBJECT (sink, "Failed to create internal window: %lu",
        GetLastError ());
    return NULL;
  }

  /* Now show the window, as appropriate */
  ShowWindow (video_window, SW_SHOWNORMAL);

  /* Trigger the initial paint of the window */
  UpdateWindow (video_window);

  return video_window;
}

typedef struct
{
  GstD3DVideoSink *sink;
  gboolean running;
  HWND hWnd;
} D3DInternalWindowDat;

static gpointer
d3d_internal_window_thread (D3DInternalWindowDat * dat)
{
  GstD3DVideoSink *sink;
  HWND hWnd;
  MSG msg;

  g_return_val_if_fail (dat != NULL, NULL);

  sink = dat->sink;
  GST_DEBUG_OBJECT (sink, "Entering internal window thread: %p",
      g_thread_self ());

  /* Create internal window */
  hWnd = _d3d_create_internal_window (sink);
  if (!hWnd) {
    GST_ERROR_OBJECT (sink, "Failed to create internal window");
    goto end;
  }

  dat->hWnd = hWnd;
  dat->running = TRUE;

  /*
   * Internal window message loop
   */
  while (GetMessage (&msg, NULL, 0, 0)) {
    TranslateMessage (&msg);
    DispatchMessage (&msg);
  }

end:
  GST_DEBUG_OBJECT (sink, "Exiting internal window thread: %p",
      g_thread_self ());
  return NULL;
}

static HWND
d3d_create_internal_window (GstD3DVideoSink * sink)
{
  GThread *thread;
  D3DInternalWindowDat dat;
  gulong timeout_interval = 10000;      /* 10 ms interval */
  gulong intervals = (10000000 / timeout_interval);     /* 10 secs */
  gulong i;

  dat.sink = sink;
  dat.running = FALSE;
  dat.hWnd = 0;

  thread =
      g_thread_new ("d3dvideosink-window-thread",
      (GThreadFunc) d3d_internal_window_thread, &dat);
  if (!thread) {
    GST_ERROR ("Failed to created internal window thread");
    return 0;
  }

  /* Wait 10 seconds for window proc loop to start up */
  for (i = 0; dat.running == FALSE && i < intervals; i++) {
    g_usleep (timeout_interval);
  }

  GST_DEBUG_OBJECT (sink, "Created window: %p (intervals: %lu)", dat.hWnd, i);

  return dat.hWnd;
}

/*** D3D Video Class Methdos ***/

gboolean
d3d_class_init (GstD3DVideoSink * sink)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  gulong timeout_interval = 10000;      /* 10 ms interval */
  gulong intervals = (10000000 / timeout_interval);     /* 10 secs */
  gboolean ret = FALSE;
  gulong i;

  g_return_val_if_fail (class != NULL, FALSE);

  LOCK_CLASS (sink, class);

  class->d3d.refs += 1;
  GST_DEBUG ("D3D class init [refs:%u]", class->d3d.refs);
  class->d3d.sink_list = g_list_append (class->d3d.sink_list, sink);

  if (class->d3d.refs > 1)
    goto end;

  WM_D3DVIDEO_NOTIFY_DEVICE_LOST =
      RegisterWindowMessage ("WM_D3DVIDEO_NOTIFY_DEVICE_LOST");

  class->d3d.d3d = Direct3DCreate9 (D3D_SDK_VERSION);
  if (!class->d3d.d3d) {
    GST_ERROR ("Unable to create Direct3D interface");
    goto error;
  }

  /* Register Window Class for internal Windows */
  memset (&class->d3d.wnd_class, 0, sizeof (WNDCLASS));
  class->d3d.wnd_class.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
  class->d3d.wnd_class.hInstance = GetModuleHandle (NULL);
  class->d3d.wnd_class.lpszClassName = TEXT ("GstD3DVideoSinkInternalWindow");
  class->d3d.wnd_class.hbrBackground = (HBRUSH) GetStockObject (BLACK_BRUSH);
  class->d3d.wnd_class.hCursor = LoadCursor (NULL, IDC_ARROW);
  class->d3d.wnd_class.hIcon = LoadIcon (NULL, IDI_APPLICATION);
  class->d3d.wnd_class.cbClsExtra = 0;
  class->d3d.wnd_class.cbWndExtra = 0;
  class->d3d.wnd_class.lpfnWndProc = d3d_wnd_proc_internal;

  if (RegisterClass (&class->d3d.wnd_class) == 0) {
    GST_ERROR ("Failed to register window class: %lu", GetLastError ());
    goto error;
  }

  class->d3d.running = FALSE;
  class->d3d.error_exit = FALSE;
  UNLOCK_CLASS (sink, class);
  class->d3d.thread =
      g_thread_new ("d3dvideosink-window-thread",
      (GThreadFunc) d3d_hidden_window_thread, class);
  LOCK_CLASS (sink, class);

  if (!class->d3d.thread) {
    GST_ERROR ("Failed to created hidden window thread");
    goto error;
  }

  UNLOCK_CLASS (sink, class);
  /* Wait 10 seconds for window proc loop to start up */
  for (i = 0; class->d3d.running == FALSE && i < intervals; i++) {
    g_usleep (timeout_interval);
  }
  LOCK_CLASS (sink, class);

  if (class->d3d.error_exit)
    goto error;

  if (!class->d3d.running) {
    GST_ERROR ("Waited %lu ms, window proc loop has not started",
        (timeout_interval * intervals) / 1000);
    goto error;
  }

  GST_DEBUG ("Hidden window message loop is running..");

end:
  ret = TRUE;
error:
  UNLOCK_CLASS (sink, class);

  if (!ret)
    d3d_class_destroy (sink);

  return ret;
}

void
d3d_class_destroy (GstD3DVideoSink * sink)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);

  g_return_if_fail (class != NULL);

  LOCK_CLASS (sink, class);

  class->d3d.refs -= 1;

  GST_DEBUG ("D3D class destroy [refs:%u]", class->d3d.refs);

  class->d3d.sink_list = g_list_remove (class->d3d.sink_list, sink);

  if (class->d3d.refs >= 1)
    goto end;

  UNLOCK_CLASS (sink, class);

  if (class->d3d.running) {
    GST_DEBUG ("Shutting down window proc thread, waiting to join..");
    PostMessage (class->d3d.hidden_window, WM_QUIT, 0, 0);
    g_thread_join (class->d3d.thread);
    GST_DEBUG ("Joined..");
  }

  LOCK_CLASS (sink, class);

  if (class->d3d.d3d) {
    int ref_count;
    ref_count = IDirect3D9_Release (class->d3d.d3d);
    GST_DEBUG ("Direct3D object released. Reference count: %d", ref_count);
  }

  UnregisterClass (class->d3d.wnd_class.lpszClassName,
      class->d3d.wnd_class.hInstance);

  memset (&class->d3d, 0, sizeof (GstD3DDataClass));

end:
  UNLOCK_CLASS (sink, class);
}

static gboolean
d3d_class_display_device_create (GstD3DVideoSinkClass * class, UINT adapter)
{
  LPDIRECT3D9 d3d;
  GstD3DDisplayDevice *device;
  HWND hwnd;
  D3DCAPS9 caps;
  D3DDISPLAYMODE disp_mode;
  DWORD create_mask = 0;
  HRESULT hr;
  gboolean ret = FALSE;

  g_return_val_if_fail (class != NULL, FALSE);

  GST_DEBUG (" ");

  LOCK_CLASS (NULL, class);

  d3d = class->d3d.d3d;
  device = &class->d3d.device;
  hwnd = class->d3d.hidden_window;

  memset (&caps, 0, sizeof (caps));
  memset (&disp_mode, 0, sizeof (disp_mode));
  memset (&device->present_params, 0, sizeof (device->present_params));

  device->adapter = adapter;

  if (IDirect3D9_GetAdapterDisplayMode (d3d, adapter, &disp_mode) != D3D_OK) {
    GST_ERROR ("Unable to request adapter[%u] display mode", adapter);
    goto error;
  }

  if (IDirect3D9_GetDeviceCaps (d3d, adapter, D3DDEVTYPE_HAL, &caps) != D3D_OK) {
    GST_ERROR ("Unable to request adapter[%u] device caps", adapter);
    goto error;
  }

  /* Ask DirectX to please not clobber the FPU state when making DirectX
   * API calls. This can cause libraries such as cairo to misbehave in
   * certain scenarios.
   */
  create_mask = 0 | D3DCREATE_FPU_PRESERVE;

  /* Determine vertex processing capabilities. Some cards have issues
   * using software vertex processing. Courtesy:
   * http://www.chadvernon.com/blog/resources/directx9/improved-direct3d-initialization/
   */
  if ((caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) ==
      D3DDEVCAPS_HWTRANSFORMANDLIGHT) {
    create_mask |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
    /* if ((d3dcaps.DevCaps & D3DDEVCAPS_PUREDEVICE) == D3DDEVCAPS_PUREDEVICE) */
    /*  d3dcreate |= D3DCREATE_PUREDEVICE; */
  } else {
    create_mask |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
  }

  /* Check the filter type. */
  if ((caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) ==
      D3DPTFILTERCAPS_MINFLINEAR
      && (caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR) ==
      D3DPTFILTERCAPS_MAGFLINEAR) {
    device->filter_type = D3DTEXF_LINEAR;
  } else {
    device->filter_type = D3DTEXF_NONE;
  }

  /* Setup the display mode format. */
  device->format = disp_mode.Format;

  //present_params.Flags = D3DPRESENTFLAG_VIDEO;
  device->present_params.Windowed = TRUE;
  device->present_params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  device->present_params.BackBufferCount = 1;
  device->present_params.BackBufferFormat = device->format;
  device->present_params.BackBufferWidth = 1;
  device->present_params.BackBufferHeight = 1;
  device->present_params.MultiSampleType = D3DMULTISAMPLE_NONE;
  device->present_params.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;    //D3DPRESENT_INTERVAL_IMMEDIATE;

  GST_DEBUG ("Creating Direct3D device for hidden window %p", NULL);    //shared.hidden_window_handle);

  if ((hr = IDirect3D9_CreateDevice (d3d, adapter, D3DDEVTYPE_HAL, hwnd,
              create_mask, &device->present_params,
              &device->d3d_device)) != D3D_OK) {
    GST_ERROR ("Unable to create Direct3D device. Result: %ld (0x%lx)", hr, hr);
    goto error;
  }

  GST_DEBUG ("Display Device format: %s",
      d3d_format_to_string (disp_mode.Format));

  ret = TRUE;
  goto end;
error:
  memset (device, 0, sizeof (GstD3DDisplayDevice));
end:
  UNLOCK_CLASS (NULL, class);

  return ret;
}

static void
d3d_class_display_device_destroy (GstD3DVideoSinkClass * class)
{
  g_return_if_fail (class != NULL);

  LOCK_CLASS (NULL, class);
  if (class->d3d.device.d3d_device) {
    int ref_count;
    ref_count = IDirect3DDevice9_Release (class->d3d.device.d3d_device);
    GST_DEBUG ("Direct3D device [adapter:%u] released. Reference count: %d",
        class->d3d.device.adapter, ref_count);
  }
  memset (&class->d3d.device, 0, sizeof (GstD3DDisplayDevice));
  UNLOCK_CLASS (NULL, class);
}

static void
d3d_class_notify_device_lost (GstD3DVideoSink * sink)
{
  GstD3DVideoSinkClass *class = GST_D3DVIDEOSINK_GET_CLASS (sink);
  PostMessage (class->d3d.hidden_window, WM_D3DVIDEO_NOTIFY_DEVICE_LOST, 0, 0);
}

static void
d3d_class_notify_device_lost_all (GstD3DVideoSinkClass * class)
{
  g_return_if_fail (class != NULL);

  LOCK_CLASS (NULL, class);
  if (!class->d3d.device_lost) {
    GList *lst, *clst;
    class->d3d.device_lost = TRUE;

    GST_DEBUG ("Notifying all instances of device loss");

    clst = g_list_copy (class->d3d.sink_list);
    UNLOCK_CLASS (NULL, class);

    for (lst = clst; lst != NULL; lst = lst->next) {
      GstD3DVideoSink *sink = (GstD3DVideoSink *) lst->data;
      if (!sink)
        continue;
      d3d_notify_device_lost (sink);
    }
    g_list_free (clst);
    LOCK_CLASS (NULL, class);

    /* Set timer to try reset at given interval */
    SetTimer (class->d3d.hidden_window, IDT_DEVICE_RESET_TIMER, 500, NULL);
  }
  UNLOCK_CLASS (NULL, class);
}

static void
d3d_class_reset_display_device (GstD3DVideoSinkClass * class)
{
  HRESULT hr;

  g_return_if_fail (class != NULL);

  LOCK_CLASS (NULL, class);
  hr = IDirect3DDevice9_Reset (class->d3d.device.d3d_device,
      &class->d3d.device.present_params);
  ERROR_CHECK_HR (hr) {
    CASE_HR_ERR (D3DERR_DEVICELOST);
    CASE_HR_ERR (D3DERR_DEVICEREMOVED);
    CASE_HR_ERR (D3DERR_DRIVERINTERNALERROR);
    CASE_HR_ERR (D3DERR_OUTOFVIDEOMEMORY);
    CASE_HR_DBG_END (NULL, "Attempt device reset.. failed");
    goto end;
  }

  GST_INFO ("Attempt device reset.. success");

  class->d3d.device_lost = FALSE;
  KillTimer (class->d3d.hidden_window, IDT_DEVICE_RESET_TIMER);

  g_list_foreach (class->d3d.sink_list, (GFunc) d3d_notify_device_reset, NULL);
end:;
  UNLOCK_CLASS (NULL, class);
}

/** Hidden Window Loop Thread **/

static LRESULT APIENTRY
D3DHiddenWndProc (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  switch (message) {
    case WM_TIMER:
      switch (wParam) {
        case IDT_DEVICE_RESET_TIMER:
          d3d_class_reset_display_device ((GstD3DVideoSinkClass *)
              GetWindowLongPtr (hWnd, GWLP_USERDATA));
          break;
        default:;
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage (0);
      return 0;
    default:
      /* non constants */
      if (message == WM_D3DVIDEO_NOTIFY_DEVICE_LOST) {
        d3d_class_notify_device_lost_all ((GstD3DVideoSinkClass *)
            GetWindowLongPtr (hWnd, GWLP_USERDATA));
        return 0;
      }
  }

  return DefWindowProc (hWnd, message, wParam, lParam);
}

static gboolean
d3d_hidden_window_thread (GstD3DVideoSinkClass * class)
{
  WNDCLASS WndClass;
  gboolean reged = FALSE;
  HWND hWnd = 0;
  gboolean ret = FALSE;

  g_return_val_if_fail (class != NULL, FALSE);

  memset (&WndClass, 0, sizeof (WNDCLASS));
  WndClass.hInstance = GetModuleHandle (NULL);
  WndClass.lpszClassName = TEXT ("gstd3dvideo-hidden-window-class");
  WndClass.lpfnWndProc = D3DHiddenWndProc;

  if (!RegisterClass (&WndClass)) {
    GST_ERROR ("Unable to register Direct3D hidden window class");
    goto error;
  }
  reged = TRUE;

  hWnd = CreateWindowEx (0,
      WndClass.lpszClassName,
      TEXT ("GStreamer Direct3D hidden window"),
      WS_POPUP, 0, 0, 1, 1, HWND_MESSAGE, NULL, WndClass.hInstance, class);

  if (hWnd == NULL) {
    GST_ERROR ("Failed to create Direct3D hidden window");
    goto error;
  }

  GST_DEBUG ("Direct3D hidden window handle: %p", hWnd);

  class->d3d.hidden_window = hWnd;

  /* TODO: Multi-monitor setup? */
  if (!d3d_class_display_device_create (class, D3DADAPTER_DEFAULT)) {
    GST_ERROR ("Failed to initiazlize adapter: %u", D3DADAPTER_DEFAULT);
    goto error;
  }

  /* Attach data to window */
  SetWindowLongPtr (hWnd, GWLP_USERDATA, (LONG_PTR) class);

  GST_DEBUG ("Entering Direct3D hidden window message loop");

  class->d3d.running = TRUE;

  /* Hidden Window Message Loop */
  while (1) {
    MSG msg;
    while (GetMessage (&msg, NULL, 0, 0)) {
      TranslateMessage (&msg);
      DispatchMessage (&msg);
    }
    if (msg.message == WM_QUIT || msg.message == WM_CLOSE)
      break;
  }

  class->d3d.running = FALSE;

  GST_DEBUG ("Leaving Direct3D hidden window message loop");

  ret = TRUE;

error:
  if (!ret)
    class->d3d.error_exit = TRUE;
  if (reged)
    UnregisterClass (WndClass.lpszClassName, WndClass.hInstance);
  if (hWnd) {
    DestroyWindow (hWnd);
    class->d3d.hidden_window = 0;
  }
  d3d_class_display_device_destroy (class);

  return ret;
}