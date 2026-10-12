/* Linux V4L2 memory-to-memory H.264 encoder backend for the VirGL video
 * protocol.  Used on hosts whose hardware encoder is exposed as a V4L2 M2M
 * device instead of VA-API (for example the Arm Linlon/Mali-V "mvxenc" on
 * CIX Sky1).  Mirrors the macOS VideoToolbox backend: encode only, NV12 or
 * RGBA guest surfaces, Annex-B output with SPS/PPS on every IDR frame. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <dirent.h>
#include <epoxy/gl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "pipe/p_video_enums.h"
#include "pipe/p_video_state.h"
#include "util/list.h"
#include "virgl_hw.h"
#include "virgl_video_hw.h"
#include "vrend_iov.h"
#include "vrend_renderer.h"
#include "vrend_video.h"

#define V4L2_NUM_OUTPUT 4
#define V4L2_NUM_CAPTURE 4
#define V4L2_ENCODE_TIMEOUT_MS 1000

struct v4l2_plane_map {
   void *ptr;
   size_t length;
};

struct v4l2_buf_map {
   unsigned num_planes;
   struct v4l2_plane_map planes[VIDEO_MAX_PLANES];
};

struct v4l2_codec {
   struct list_head head;
   struct vrend_video_context *ctx;
   uint32_t handle;
   uint32_t width;
   uint32_t height;
   int fd;
   bool streaming;
   uint32_t in_fourcc;
   struct v4l2_format in_fmt;
   unsigned num_output;
   unsigned num_capture;
   struct v4l2_buf_map output[V4L2_NUM_OUTPUT];
   struct v4l2_buf_map capture[V4L2_NUM_CAPTURE];
   bool output_busy[V4L2_NUM_OUTPUT];
   uint32_t bitrate;
   uint8_t *headers;
   size_t headers_size;
   uint8_t *encoded;
   size_t encoded_size;
   uint64_t frame_index;
   /* Worker state; jobs and registry are protected by video_mutex. */
   struct list_head registry;
   struct list_head jobs;
   pthread_cond_t job_cond;
   pthread_t worker;
   bool worker_running;
   bool stopping;
};

struct v4l2_video_buffer {
   struct list_head head;
   struct vrend_video_context *ctx;
   uint32_t handle;
   enum pipe_format format;
   uint32_t width;
   uint32_t height;
   unsigned num_planes;
   uint32_t res_handles[3];
};

struct vrend_video_context {
   struct vrend_context *ctx;
   struct list_head codecs;
   struct list_head buffers;
};

static char v4l2_device[PATH_MAX];

static int xioctl(int fd, unsigned long request, void *arg)
{
   int r;
   do {
      r = ioctl(fd, request, arg);
   } while (r < 0 && errno == EINTR);
   return r;
}

static bool device_encodes_h264(const char *path)
{
   int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
   if (fd < 0)
      return false;
   bool ok = false;
   struct v4l2_capability cap;
   memset(&cap, 0, sizeof(cap));
   if (!xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
      uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps
                                                              : cap.capabilities;
      if ((caps & V4L2_CAP_VIDEO_M2M_MPLANE) && (caps & V4L2_CAP_STREAMING)) {
         struct v4l2_fmtdesc desc;
         for (unsigned i = 0;; ++i) {
            memset(&desc, 0, sizeof(desc));
            desc.index = i;
            desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            if (xioctl(fd, VIDIOC_ENUM_FMT, &desc))
               break;
            if (desc.pixelformat == V4L2_PIX_FMT_H264) {
               ok = true;
               break;
            }
         }
      }
   }
   close(fd);
   return ok;
}

static bool find_encoder(void)
{
   const char *forced = getenv("VIRGL_VIDEO_V4L2_ENCODER");
   if (forced && *forced) {
      snprintf(v4l2_device, sizeof(v4l2_device), "%s", forced);
      return device_encodes_h264(v4l2_device);
   }
   DIR *dir = opendir("/dev");
   if (!dir)
      return false;
   bool found = false;
   struct dirent *entry;
   while (!found && (entry = readdir(dir))) {
      if (strncmp(entry->d_name, "video", 5))
         continue;
      if (strlen(entry->d_name) + 6 > sizeof(v4l2_device))
         continue;
      snprintf(v4l2_device, sizeof(v4l2_device), "/dev/%.*s",
               (int)(sizeof(v4l2_device) - 6), entry->d_name);
      found = device_encodes_h264(v4l2_device);
   }
   closedir(dir);
   return found;
}

static struct v4l2_codec *get_codec(struct vrend_video_context *ctx, uint32_t handle)
{
   list_for_each_entry(struct v4l2_codec, codec, &ctx->codecs, head) {
      if (codec->handle == handle)
         return codec;
   }
   return NULL;
}

static struct v4l2_video_buffer *get_buffer(struct vrend_video_context *ctx, uint32_t handle)
{
   list_for_each_entry(struct v4l2_video_buffer, buffer, &ctx->buffers, head) {
      if (buffer->handle == handle)
         return buffer;
   }
   return NULL;
}

static bool append_bytes(uint8_t **data, size_t *size, const uint8_t *src, size_t len)
{
   uint8_t *next = realloc(*data, *size + len);
   if (!next)
      return false;
   *data = next;
   memcpy(next + *size, src, len);
   *size += len;
   return true;
}

/* Returns a bitmask of the H.264 NAL unit types found in an Annex-B stream. */
static uint32_t nal_types(const uint8_t *data, size_t size)
{
   uint32_t mask = 0;
   for (size_t i = 0; i + 3 < size; ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
         mask |= 1u << (data[i + 3] & 0x1f);
         i += 2;
      }
   }
   return mask;
}

#define NAL_SLICE_MASK ((1u << 1) | (1u << 5))
#define NAL_SPS_MASK (1u << 7)

static void set_ctrl(struct v4l2_codec *codec, uint32_t id, int32_t value)
{
   struct v4l2_control ctrl = { .id = id, .value = value };
   if (xioctl(codec->fd, VIDIOC_S_CTRL, &ctrl))
      fprintf(stderr, "virgl-video-v4l2: control %#x=%d failed: %s\n", id, value,
              strerror(errno));
}

static void unmap_buffers(struct v4l2_buf_map *maps, unsigned count)
{
   for (unsigned i = 0; i < count; ++i) {
      for (unsigned p = 0; p < maps[i].num_planes; ++p) {
         if (maps[i].planes[p].ptr && maps[i].planes[p].ptr != MAP_FAILED)
            munmap(maps[i].planes[p].ptr, maps[i].planes[p].length);
      }
      memset(&maps[i], 0, sizeof(maps[i]));
   }
}

static void stop_stream(struct v4l2_codec *codec)
{
   if (codec->fd < 0)
      return;
   int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
   xioctl(codec->fd, VIDIOC_STREAMOFF, &type);
   type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
   xioctl(codec->fd, VIDIOC_STREAMOFF, &type);
   unmap_buffers(codec->output, codec->num_output);
   unmap_buffers(codec->capture, codec->num_capture);
   struct v4l2_requestbuffers req = { .count = 0, .memory = V4L2_MEMORY_MMAP };
   req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
   xioctl(codec->fd, VIDIOC_REQBUFS, &req);
   req.count = 0;
   req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
   xioctl(codec->fd, VIDIOC_REQBUFS, &req);
   memset(codec->output_busy, 0, sizeof(codec->output_busy));
   codec->num_output = codec->num_capture = 0;
   codec->streaming = false;
}

static bool map_buffers(struct v4l2_codec *codec, uint32_t type, struct v4l2_buf_map *maps,
                        unsigned *count, unsigned wanted)
{
   struct v4l2_requestbuffers req = {
      .count = wanted, .type = type, .memory = V4L2_MEMORY_MMAP,
   };
   if (xioctl(codec->fd, VIDIOC_REQBUFS, &req) || !req.count)
      return false;
   *count = req.count < wanted ? req.count : wanted;
   for (unsigned i = 0; i < *count; ++i) {
      struct v4l2_plane planes[VIDEO_MAX_PLANES];
      struct v4l2_buffer buf = {
         .index = i, .type = type, .memory = V4L2_MEMORY_MMAP,
         .m.planes = planes, .length = VIDEO_MAX_PLANES,
      };
      memset(planes, 0, sizeof(planes));
      if (xioctl(codec->fd, VIDIOC_QUERYBUF, &buf))
         return false;
      maps[i].num_planes = buf.length;
      for (unsigned p = 0; p < buf.length; ++p) {
         maps[i].planes[p].length = planes[p].length;
         maps[i].planes[p].ptr = mmap(NULL, planes[p].length, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, codec->fd, planes[p].m.mem_offset);
         if (maps[i].planes[p].ptr == MAP_FAILED)
            return false;
      }
   }
   return true;
}

static bool queue_capture(struct v4l2_codec *codec, unsigned index)
{
   struct v4l2_plane planes[VIDEO_MAX_PLANES];
   memset(planes, 0, sizeof(planes));
   struct v4l2_buffer buf = {
      .index = index, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
      .memory = V4L2_MEMORY_MMAP, .m.planes = planes,
      .length = codec->capture[index].num_planes,
   };
   for (unsigned p = 0; p < buf.length; ++p)
      planes[p].length = codec->capture[index].planes[p].length;
   return !xioctl(codec->fd, VIDIOC_QBUF, &buf);
}

/* The input format is only known once the guest submits its first surface,
 * so the V4L2 queues are (re)configured lazily per input fourcc. */
static bool start_stream(struct v4l2_codec *codec, uint32_t fourcc)
{
   if (codec->streaming && codec->in_fourcc == fourcc)
      return true;
   stop_stream(codec);

   struct v4l2_format fmt;
   memset(&fmt, 0, sizeof(fmt));
   fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
   fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
   fmt.fmt.pix_mp.width = codec->width;
   fmt.fmt.pix_mp.height = codec->height;
   fmt.fmt.pix_mp.num_planes = 1;
   fmt.fmt.pix_mp.plane_fmt[0].sizeimage = codec->width * codec->height * 3 / 2 + 4096;
   if (xioctl(codec->fd, VIDIOC_S_FMT, &fmt)) {
      fprintf(stderr, "virgl-video-v4l2: S_FMT H264 failed: %s\n", strerror(errno));
      return false;
   }

   memset(&codec->in_fmt, 0, sizeof(codec->in_fmt));
   codec->in_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
   codec->in_fmt.fmt.pix_mp.pixelformat = fourcc;
   codec->in_fmt.fmt.pix_mp.width = codec->width;
   codec->in_fmt.fmt.pix_mp.height = codec->height;
   codec->in_fmt.fmt.pix_mp.num_planes = 1;
   if (xioctl(codec->fd, VIDIOC_S_FMT, &codec->in_fmt) ||
       codec->in_fmt.fmt.pix_mp.pixelformat != fourcc) {
      fprintf(stderr, "virgl-video-v4l2: S_FMT input %.4s failed: %s\n",
              (const char *)&fourcc, strerror(errno));
      return false;
   }

   struct v4l2_streamparm parm;
   memset(&parm, 0, sizeof(parm));
   parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
   parm.parm.output.timeperframe.numerator = 1;
   parm.parm.output.timeperframe.denominator = 30;
   xioctl(codec->fd, VIDIOC_S_PARM, &parm);

   set_ctrl(codec, V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_MAIN);
   set_ctrl(codec, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
   /* Keyframes are requested per frame by the guest. */
   set_ctrl(codec, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0x7fffffff);
   set_ctrl(codec, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
   set_ctrl(codec, V4L2_CID_MPEG_VIDEO_BITRATE, codec->bitrate);

   if (!map_buffers(codec, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, codec->output,
                    &codec->num_output, V4L2_NUM_OUTPUT) ||
       !map_buffers(codec, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, codec->capture,
                    &codec->num_capture, V4L2_NUM_CAPTURE)) {
      fprintf(stderr, "virgl-video-v4l2: buffer allocation failed: %s\n", strerror(errno));
      stop_stream(codec);
      return false;
   }
   for (unsigned i = 0; i < codec->num_capture; ++i) {
      if (!queue_capture(codec, i)) {
         stop_stream(codec);
         return false;
      }
   }
   int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
   if (xioctl(codec->fd, VIDIOC_STREAMON, &type)) {
      stop_stream(codec);
      return false;
   }
   type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
   if (xioctl(codec->fd, VIDIOC_STREAMON, &type)) {
      stop_stream(codec);
      return false;
   }
   codec->in_fourcc = fourcc;
   codec->streaming = true;
   free(codec->headers);
   codec->headers = NULL;
   codec->headers_size = 0;
   fprintf(stderr, "virgl-video-v4l2: %s streaming %ux%u input=%.4s stride=%u\n",
           v4l2_device, codec->width, codec->height, (const char *)&fourcc,
           codec->in_fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
   return true;
}

/* Read a GL texture as tightly packed RGBA8.  FBO + glReadPixels(GL_RGBA)
 * works on both desktop GL and GLES hosts (Mali has no glGetTexImage). */
static bool read_texture_rgba(struct vrend_context *ctx, uint32_t res_handle,
                              uint32_t width, uint32_t height, uint8_t *dst,
                              bool *y_0_top)
{
   struct vrend_resource *res = vrend_renderer_ctx_res_lookup(ctx, res_handle);
   if (!res || !res->gl_id)
      return false;
   if (!vrend_hw_switch_context(ctx, true))
      return false;
   glFinish();
   GLuint fbo = 0;
   GLint old_fbo = 0;
   glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, res->target, res->gl_id, 0);
   bool ok = false;
   GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
   if (status == GL_FRAMEBUFFER_COMPLETE) {
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, dst);
      GLenum error = glGetError();
      ok = error == GL_NO_ERROR;
      if (!ok)
         fprintf(stderr, "virgl-video-v4l2: glReadPixels error=0x%x\n", error);
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
   } else {
      fprintf(stderr, "virgl-video-v4l2: FBO incomplete status=0x%x target=0x%x\n",
              status, res->target);
   }
   glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
   glDeleteFramebuffers(1, &fbo);
   if (y_0_top)
      *y_0_top = res->y_0_top;
   return ok;
}

static bool fill_nv12(struct v4l2_codec *codec, struct v4l2_video_buffer *source,
                      struct v4l2_buf_map *map, uint32_t *bytesused)
{
   const struct v4l2_pix_format_mplane *pix = &codec->in_fmt.fmt.pix_mp;
   const uint32_t w = source->width, h = source->height;
   const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
   uint8_t *rgba = malloc((size_t)w * h * 4);
   if (!rgba)
      return false;
   uint8_t *y_dst = map->planes[0].ptr;
   size_t y_stride = pix->plane_fmt[0].bytesperline;
   uint8_t *uv_dst;
   size_t uv_stride;
   if (pix->num_planes >= 2 && map->num_planes >= 2) {
      uv_dst = map->planes[1].ptr;
      uv_stride = pix->plane_fmt[1].bytesperline;
   } else {
      uv_dst = y_dst + y_stride * pix->height;
      uv_stride = y_stride;
   }
   bool ok = read_texture_rgba(source->ctx->ctx, source->res_handles[0], w, h, rgba, NULL);
   for (uint32_t y = 0; ok && y < h; ++y) {
      const uint8_t *src = rgba + (size_t)y * w * 4;
      uint8_t *dst = y_dst + (size_t)y * y_stride;
      for (uint32_t x = 0; x < w; ++x)
         dst[x] = src[x * 4];
   }
   ok = ok && read_texture_rgba(source->ctx->ctx, source->res_handles[1], cw, ch, rgba, NULL);
   for (uint32_t y = 0; ok && y < ch; ++y) {
      const uint8_t *src = rgba + (size_t)y * cw * 4;
      uint8_t *dst = uv_dst + (size_t)y * uv_stride;
      for (uint32_t x = 0; x < cw; ++x) {
         dst[x * 2] = src[x * 4];
         dst[x * 2 + 1] = src[x * 4 + 1];
      }
   }
   free(rgba);
   bytesused[0] = pix->num_planes >= 2 ? y_stride * pix->height
                                       : pix->plane_fmt[0].sizeimage;
   bytesused[1] = pix->num_planes >= 2 ? pix->plane_fmt[1].sizeimage : 0;
   return ok;
}

static bool fill_rgba(struct v4l2_codec *codec, struct v4l2_video_buffer *source,
                      struct v4l2_buf_map *map, uint32_t *bytesused)
{
   const struct v4l2_pix_format_mplane *pix = &codec->in_fmt.fmt.pix_mp;
   const uint32_t w = source->width, h = source->height;
   const size_t row = (size_t)w * 4;
   uint8_t *rgba = malloc(row * h);
   if (!rgba)
      return false;
   bool y_0_top = false;
   bool ok = read_texture_rgba(source->ctx->ctx, source->res_handles[0], w, h, rgba, &y_0_top);
   size_t stride = pix->plane_fmt[0].bytesperline;
   uint8_t *dst = map->planes[0].ptr;
   for (uint32_t y = 0; ok && y < h; ++y) {
      const uint32_t src_y = y_0_top ? h - 1 - y : y;
      memcpy(dst + (size_t)y * stride, rgba + (size_t)src_y * row, row);
   }
   free(rgba);
   bytesused[0] = pix->plane_fmt[0].sizeimage;
   return ok;
}

static bool reclaim_output(struct v4l2_codec *codec)
{
   struct v4l2_plane planes[VIDEO_MAX_PLANES];
   struct v4l2_buffer buf = {
      .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
      .m.planes = planes, .length = VIDEO_MAX_PLANES,
   };
   bool any = false;
   while (!xioctl(codec->fd, VIDIOC_DQBUF, &buf)) {
      if (buf.index < codec->num_output)
         codec->output_busy[buf.index] = false;
      any = true;
   }
   return any;
}

static uint64_t now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* Encoding is split in two: the VirGL thread reads the guest surface (needs
 * the GL context) and queues it to the encoder, then returns.  A per-codec
 * worker collects the bitstream and writes the destination buffer and then
 * the feedback into guest memory; the guest polls the feedback status.
 * Fences are not held back: QEMU has a single fence timeline, so delaying
 * one would also delay SurfaceFlinger and drop composed frames. */
struct v4l2_job {
   struct list_head head;
   struct vrend_resource *dest;
   struct vrend_resource *feed;
   uint64_t frame;
   uint64_t queued_us;
};

static pthread_mutex_t video_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t video_done_cond = PTHREAD_COND_INITIALIZER;
static struct list_head video_codecs = { &video_codecs, &video_codecs };

/* Rolling per-stage timing, printed every 60 frames. */
static uint64_t stat_frames, stat_fill_us, stat_encode_us;
/* Guest round trip: worker finished a job -> next submit from the guest. */
static uint64_t stat_last_submit_us, stat_last_done_us, stat_period_us, stat_period_max_us,
                stat_gap_us, stat_gap_max_us;

static bool pending_locked(const void *res)
{
   list_for_each_entry(struct v4l2_codec, codec, &video_codecs, registry) {
      list_for_each_entry(struct v4l2_job, job, &codec->jobs, head) {
         if (job->dest == res || job->feed == res)
            return true;
      }
   }
   return false;
}

void vrend_video_wait_resource(const struct vrend_resource *res)
{
   pthread_mutex_lock(&video_mutex);
   while (pending_locked(res))
      pthread_cond_wait(&video_done_cond, &video_mutex);
   pthread_mutex_unlock(&video_mutex);
}

static void drain_jobs(struct v4l2_codec *codec)
{
   pthread_mutex_lock(&video_mutex);
   while (!list_is_empty(&codec->jobs))
      pthread_cond_wait(&video_done_cond, &video_mutex);
   pthread_mutex_unlock(&video_mutex);
}

static void write_result(struct v4l2_job *job, const uint8_t *data, size_t size)
{
   struct virgl_video_encode_feedback feedback;
   memset(&feedback, 0, sizeof(feedback));
   if (data && size && size <= job->dest->base.width0) {
      if (job->dest->iov)
         vrend_write_to_iovec(job->dest->iov, job->dest->num_iovs, 0, (const char *)data, size);
      else if (job->dest->ptr)
         memcpy(job->dest->ptr, data, size);
      feedback.stat = VIRGL_VIDEO_ENCODE_STAT_SUCCESS;
      feedback.coded_size = size;
      /* The guest polls the feedback: publish the coded data first. */
      __atomic_thread_fence(__ATOMIC_RELEASE);
   } else {
      feedback.stat = VIRGL_VIDEO_ENCODE_STAT_FAILURE;
      fprintf(stderr, "virgl-video-v4l2: encode failed coded=%zu dest=%u\n", size,
              job->dest->base.width0);
   }
   if (job->feed->iov)
      vrend_write_to_iovec(job->feed->iov, job->feed->num_iovs, 0, (const char *)&feedback,
                           MIN2(job->feed->base.width0, sizeof(feedback)));
   else if (job->feed->ptr)
      memcpy(job->feed->ptr, &feedback, MIN2(job->feed->base.width0, sizeof(feedback)));
}

/* Collect capture buffers until one carries a coded slice.  Some encoders
 * return SPS/PPS in a separate first buffer.  Worker thread only. */
static bool collect_frame(struct v4l2_codec *codec, uint64_t frame)
{
   free(codec->encoded);
   codec->encoded = NULL;
   codec->encoded_size = 0;
   uint8_t *frame_data = NULL;
   size_t frame_size = 0;
   bool have_slice = false;
   while (!have_slice && !codec->stopping) {
      struct pollfd pfd = { .fd = codec->fd, .events = POLLIN };
      int pr = poll(&pfd, 1, V4L2_ENCODE_TIMEOUT_MS);
      if (pr <= 0) {
         fprintf(stderr, "virgl-video-v4l2: timed out waiting for H.264 output\n");
         break;
      }
      if (pfd.revents & POLLERR) {
         fprintf(stderr, "virgl-video-v4l2: device error\n");
         break;
      }
      struct v4l2_plane cplanes[VIDEO_MAX_PLANES];
      memset(cplanes, 0, sizeof(cplanes));
      struct v4l2_buffer cbuf = {
         .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
         .m.planes = cplanes, .length = VIDEO_MAX_PLANES,
      };
      if (xioctl(codec->fd, VIDIOC_DQBUF, &cbuf)) {
         if (errno == EAGAIN)
            continue;
         fprintf(stderr, "virgl-video-v4l2: DQBUF output failed: %s\n", strerror(errno));
         break;
      }
      if (cbuf.index < codec->num_capture) {
         const uint8_t *data = (const uint8_t *)codec->capture[cbuf.index].planes[0].ptr +
                               cplanes[0].data_offset;
         size_t len = cplanes[0].bytesused > cplanes[0].data_offset
                         ? cplanes[0].bytesused - cplanes[0].data_offset : 0;
         uint32_t types = nal_types(data, len);
         if (types & NAL_SLICE_MASK) {
            append_bytes(&frame_data, &frame_size, data, len);
            have_slice = true;
         } else if (len && (types & NAL_SPS_MASK)) {
            free(codec->headers);
            codec->headers = NULL;
            codec->headers_size = 0;
            append_bytes(&codec->headers, &codec->headers_size, data, len);
         }
         queue_capture(codec, cbuf.index);
      }
      if (cbuf.flags & V4L2_BUF_FLAG_LAST)
         break;
   }

   if (have_slice) {
      uint32_t types = nal_types(frame_data, frame_size);
      if (types & NAL_SPS_MASK) {
         /* Remember in-band parameter sets for later forced IDR frames. */
         size_t end = 0;
         for (size_t i = 0; i + 3 < frame_size; ++i) {
            if (frame_data[i] == 0 && frame_data[i + 1] == 0 && frame_data[i + 2] == 1 &&
                ((frame_data[i + 3] & 0x1f) == 1 || (frame_data[i + 3] & 0x1f) == 5)) {
               end = i > 0 && frame_data[i - 1] == 0 ? i - 1 : i;
               break;
            }
         }
         if (end) {
            free(codec->headers);
            codec->headers = NULL;
            codec->headers_size = 0;
            append_bytes(&codec->headers, &codec->headers_size, frame_data, end);
         }
      } else if ((types & (1u << 5)) && codec->headers_size) {
         append_bytes(&codec->encoded, &codec->encoded_size, codec->headers,
                      codec->headers_size);
      }
      append_bytes(&codec->encoded, &codec->encoded_size, frame_data, frame_size);
   }
   free(frame_data);
   if (frame < 3 && codec->encoded_size) {
      char nals[128] = "", hex[3 * 48 + 1] = "";
      size_t used = 0;
      for (size_t i = 0; i + 3 < codec->encoded_size && used + 4 < sizeof(nals); ++i) {
         if (codec->encoded[i] == 0 && codec->encoded[i + 1] == 0 && codec->encoded[i + 2] == 1) {
            used += snprintf(nals + used, sizeof(nals) - used, "%u ", codec->encoded[i + 3] & 0x1f);
            i += 2;
         }
      }
      for (size_t i = 0; i < 48 && i < codec->encoded_size; ++i)
         snprintf(hex + 3 * i, 4, "%02x ", codec->encoded[i]);
      fprintf(stderr, "virgl-video-v4l2: frame %llu bytes=%zu nal=[%s] head=%s\n",
              (unsigned long long)frame, codec->encoded_size, nals, hex);
   }
   return codec->encoded_size != 0;
}

static void *encode_worker(void *opaque)
{
   struct v4l2_codec *codec = opaque;
   pthread_mutex_lock(&video_mutex);
   while (!codec->stopping) {
      if (list_is_empty(&codec->jobs)) {
         pthread_cond_wait(&codec->job_cond, &video_mutex);
         continue;
      }
      struct v4l2_job *job = list_first_entry(&codec->jobs, struct v4l2_job, head);
      pthread_mutex_unlock(&video_mutex);

      bool ok = collect_frame(codec, job->frame);
      write_result(job, ok ? codec->encoded : NULL, ok ? codec->encoded_size : 0);
      stat_last_done_us = now_us();
      stat_encode_us += stat_last_done_us - job->queued_us;

      pthread_mutex_lock(&video_mutex);
      list_del(&job->head);
      free(job);
      pthread_cond_broadcast(&video_done_cond);
   }
   pthread_mutex_unlock(&video_mutex);
   return NULL;
}

/* VirGL thread: read back the guest surface and queue it to the encoder. */
static bool submit_frame(struct v4l2_codec *codec, struct v4l2_video_buffer *source,
                         const union virgl_picture_desc *desc,
                         struct vrend_resource *dest, struct vrend_resource *feed)
{
   uint32_t fourcc;
   if (source->format == PIPE_FORMAT_NV12 && source->num_planes >= 2) {
      fourcc = V4L2_PIX_FMT_NV12;
   } else if ((source->format == PIPE_FORMAT_R8G8B8A8_UNORM ||
               source->format == PIPE_FORMAT_B8G8R8A8_UNORM ||
               source->format == PIPE_FORMAT_R8G8B8X8_UNORM ||
               source->format == PIPE_FORMAT_B8G8R8X8_UNORM) && source->num_planes >= 1) {
      /* glReadPixels(GL_RGBA) always yields R,G,B,A bytes. */
      fourcc = V4L2_PIX_FMT_RGBA32;
   } else {
      fprintf(stderr, "virgl-video-v4l2: unsupported source format=0x%x planes=%u\n",
              source->format, source->num_planes);
      return false;
   }
   uint32_t bitrate = desc->h264_enc.rate_ctrl[0].target_bitrate;
   if (bitrate && bitrate != codec->bitrate) {
      codec->bitrate = bitrate;
      if (codec->streaming)
         set_ctrl(codec, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate);
   }
   if (!codec->streaming || codec->in_fourcc != fourcc)
      drain_jobs(codec);
   if (!start_stream(codec, fourcc))
      return false;

   reclaim_output(codec);
   unsigned index = 0;
   while (index < codec->num_output && codec->output_busy[index])
      ++index;
   if (index == codec->num_output) {
      struct pollfd pfd = { .fd = codec->fd, .events = POLLOUT };
      poll(&pfd, 1, V4L2_ENCODE_TIMEOUT_MS);
      reclaim_output(codec);
      for (index = 0; index < codec->num_output && codec->output_busy[index]; ++index)
         ;
      if (index == codec->num_output) {
         fprintf(stderr, "virgl-video-v4l2: no free input buffer\n");
         return false;
      }
   }

   struct v4l2_buf_map *map = &codec->output[index];
   uint32_t bytesused[VIDEO_MAX_PLANES] = {0};
   const uint64_t t_fill = now_us();
   if (stat_last_submit_us) {
      uint64_t period = t_fill - stat_last_submit_us;
      stat_period_us += period;
      if (period > stat_period_max_us) stat_period_max_us = period;
   }
   if (stat_last_done_us && stat_last_done_us < t_fill) {
      uint64_t gap = t_fill - stat_last_done_us;
      stat_gap_us += gap;
      if (gap > stat_gap_max_us) stat_gap_max_us = gap;
   }
   stat_last_submit_us = t_fill;
   bool ok = fourcc == V4L2_PIX_FMT_NV12 ? fill_nv12(codec, source, map, bytesused)
                                         : fill_rgba(codec, source, map, bytesused);
   if (!ok)
      return false;

   bool keyframe = desc->h264_enc.picture_type == PIPE_H2645_ENC_PICTURE_TYPE_IDR ||
                   codec->frame_index == 0;
   if (keyframe)
      set_ctrl(codec, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);

   struct v4l2_job *job = calloc(1, sizeof(*job));
   if (!job)
      return false;
   struct v4l2_plane planes[VIDEO_MAX_PLANES];
   memset(planes, 0, sizeof(planes));
   struct v4l2_buffer buf = {
      .index = index, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      .memory = V4L2_MEMORY_MMAP, .m.planes = planes, .length = map->num_planes,
   };
   uint64_t frame = codec->frame_index++;
   buf.timestamp.tv_sec = (time_t)(frame / 30);
   buf.timestamp.tv_usec = (suseconds_t)((frame % 30) * 1000000 / 30);
   for (unsigned p = 0; p < map->num_planes; ++p) {
      planes[p].length = map->planes[p].length;
      planes[p].bytesused = bytesused[p] && bytesused[p] <= map->planes[p].length
                               ? bytesused[p] : map->planes[p].length;
   }

   job->dest = dest;
   job->feed = feed;
   job->frame = frame;
   job->queued_us = now_us();
   stat_fill_us += job->queued_us - t_fill;
   pthread_mutex_lock(&video_mutex);
   if (xioctl(codec->fd, VIDIOC_QBUF, &buf)) {
      pthread_mutex_unlock(&video_mutex);
      fprintf(stderr, "virgl-video-v4l2: QBUF input failed: %s\n", strerror(errno));
      free(job);
      return false;
   }
   codec->output_busy[index] = true;
   list_addtail(&job->head, &codec->jobs);
   pthread_cond_signal(&codec->job_cond);
   pthread_mutex_unlock(&video_mutex);

   if (++stat_frames == 60) {
      fprintf(stderr, "virgl-video-v4l2: avg over 60 frames: readback+fill=%.1fms encode=%.1fms "
              "submit-period=%.1fms (max %.1f) guest-gap=%.1fms (max %.1f)\n",
              stat_fill_us / 60000.0, stat_encode_us / 60000.0,
              stat_period_us / 60000.0, stat_period_max_us / 1000.0,
              stat_gap_us / 60000.0, stat_gap_max_us / 1000.0);
      stat_frames = stat_fill_us = stat_encode_us = 0;
      stat_period_us = stat_period_max_us = stat_gap_us = stat_gap_max_us = 0;
   }
   return true;
}

int vrend_video_init(int drm_fd)
{
   (void)drm_fd;
   bool available = find_encoder();
   if (!available)
      v4l2_device[0] = '\0';
   fprintf(stderr, "virgl-video-v4l2: hardware H.264 encoder %s%s\n",
           available ? "available at " : "unavailable", available ? v4l2_device : "");
   return 0;
}

void vrend_video_fini(void) {}

int vrend_video_fill_caps(union virgl_caps *caps)
{
   if (!v4l2_device[0])
      return -1;
   caps->v2.num_video_caps = 1;
   caps->v2.video_caps[0].profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN;
   caps->v2.video_caps[0].entrypoint = PIPE_VIDEO_ENTRYPOINT_ENCODE;
   caps->v2.video_caps[0].max_level = 41;
   caps->v2.video_caps[0].max_width = 4096;
   caps->v2.video_caps[0].max_height = 4096;
   caps->v2.video_caps[0].supports_progressive = 1;
   fprintf(stderr, "virgl-video-v4l2: advertised AVC encode capset\n");
   return 0;
}

struct vrend_video_context *vrend_video_create_context(struct vrend_context *ctx)
{
   struct vrend_video_context *vctx = calloc(1, sizeof(*vctx));
   if (!vctx)
      return NULL;
   vctx->ctx = ctx;
   list_inithead(&vctx->codecs);
   list_inithead(&vctx->buffers);
   return vctx;
}

static void free_codec(struct v4l2_codec *codec)
{
   pthread_mutex_lock(&video_mutex);
   codec->stopping = true;
   pthread_cond_signal(&codec->job_cond);
   pthread_mutex_unlock(&video_mutex);
   if (codec->worker_running)
      pthread_join(codec->worker, NULL);
   pthread_mutex_lock(&video_mutex);
   /* Unfinished jobs are dropped; release fences waiting on them. */
   list_for_each_entry_safe(struct v4l2_job, job, &codec->jobs, head) {
      list_del(&job->head);
      free(job);
   }
   list_del(&codec->registry);
   pthread_cond_broadcast(&video_done_cond);
   pthread_mutex_unlock(&video_mutex);
   pthread_cond_destroy(&codec->job_cond);
   stop_stream(codec);
   if (codec->fd >= 0)
      close(codec->fd);
   free(codec->headers);
   free(codec->encoded);
   free(codec);
}

void vrend_video_destroy_context(struct vrend_video_context *ctx)
{
   list_for_each_entry_safe(struct v4l2_codec, codec, &ctx->codecs, head) {
      list_del(&codec->head);
      free_codec(codec);
   }
   list_for_each_entry_safe(struct v4l2_video_buffer, buffer, &ctx->buffers, head) {
      list_del(&buffer->head);
      free(buffer);
   }
   free(ctx);
}

int vrend_video_create_codec(struct vrend_video_context *ctx, uint32_t handle,
                             uint32_t profile, uint32_t entrypoint, uint32_t chroma_format,
                             uint32_t level, uint32_t width, uint32_t height,
                             uint32_t max_ref, uint32_t flags)
{
   (void)chroma_format; (void)level; (void)max_ref; (void)flags;
   if (!v4l2_device[0] || entrypoint != PIPE_VIDEO_ENTRYPOINT_ENCODE ||
       profile != PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN || !width || !height)
      return -1;
   if (get_codec(ctx, handle))
      return 0;
   struct v4l2_codec *codec = calloc(1, sizeof(*codec));
   if (!codec)
      return -1;
   codec->ctx = ctx;
   codec->handle = handle;
   codec->width = width;
   codec->height = height;
   codec->bitrate = 8000000;
   list_inithead(&codec->jobs);
   pthread_cond_init(&codec->job_cond, NULL);
   codec->fd = open(v4l2_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
   if (codec->fd < 0) {
      fprintf(stderr, "virgl-video-v4l2: open %s failed: %s\n", v4l2_device, strerror(errno));
      pthread_cond_destroy(&codec->job_cond);
      free(codec);
      return -1;
   }
   pthread_mutex_lock(&video_mutex);
   list_addtail(&codec->registry, &video_codecs);
   pthread_mutex_unlock(&video_mutex);
   codec->worker_running = pthread_create(&codec->worker, NULL, encode_worker, codec) == 0;
   if (!codec->worker_running) {
      free_codec(codec);
      return -1;
   }
   fprintf(stderr, "virgl-video-v4l2: created hardware H.264 encoder %ux%u\n", width, height);
   list_add(&codec->head, &ctx->codecs);
   return 0;
}

void vrend_video_destroy_codec(struct vrend_video_context *ctx, uint32_t handle)
{
   struct v4l2_codec *codec = get_codec(ctx, handle);
   if (!codec)
      return;
   list_del(&codec->head);
   free_codec(codec);
}

int vrend_video_create_buffer(struct vrend_video_context *ctx, uint32_t handle,
                              uint32_t format, uint32_t width, uint32_t height,
                              uint32_t *res_handles, unsigned num_res)
{
   if (!width || !height || !res_handles || !num_res || get_buffer(ctx, handle))
      return -1;
   struct v4l2_video_buffer *buffer = calloc(1, sizeof(*buffer));
   if (!buffer)
      return -1;
   buffer->ctx = ctx;
   buffer->handle = handle;
   buffer->format = format;
   buffer->width = width;
   buffer->height = height;
   buffer->num_planes = num_res > 3 ? 3 : num_res;
   memcpy(buffer->res_handles, res_handles, buffer->num_planes * sizeof(uint32_t));
   list_add(&buffer->head, &ctx->buffers);
   return 0;
}

void vrend_video_destroy_buffer(struct vrend_video_context *ctx, uint32_t handle)
{
   struct v4l2_video_buffer *buffer = get_buffer(ctx, handle);
   if (!buffer)
      return;
   list_del(&buffer->head);
   free(buffer);
}

int vrend_video_begin_frame(struct vrend_video_context *ctx, uint32_t cdc_handle,
                            uint32_t tgt_handle)
{
   return get_codec(ctx, cdc_handle) && get_buffer(ctx, tgt_handle) ? 0 : -1;
}

int vrend_video_decode_bitstream(struct vrend_video_context *ctx, uint32_t cdc_handle,
                                 uint32_t tgt_handle, uint32_t desc_handle,
                                 unsigned num_buffers, const uint32_t *buffer_handles,
                                 const uint32_t *buffer_sizes)
{
   (void)ctx; (void)cdc_handle; (void)tgt_handle; (void)desc_handle;
   (void)num_buffers; (void)buffer_handles; (void)buffer_sizes;
   return -1;
}

int vrend_video_encode_bitstream(struct vrend_video_context *ctx, uint32_t cdc_handle,
                                 uint32_t src_handle, uint32_t dest_handle,
                                 uint32_t desc_handle, uint32_t feed_handle)
{
   struct v4l2_codec *codec = get_codec(ctx, cdc_handle);
   struct v4l2_video_buffer *source = get_buffer(ctx, src_handle);
   struct vrend_resource *desc_res = vrend_renderer_ctx_res_lookup(ctx->ctx, desc_handle);
   struct vrend_resource *dest = vrend_renderer_ctx_res_lookup(ctx->ctx, dest_handle);
   struct vrend_resource *feed = vrend_renderer_ctx_res_lookup(ctx->ctx, feed_handle);
   if (!codec || !source || !desc_res || !dest || !feed)
      return -1;
   union virgl_picture_desc desc;
   memset(&desc, 0, sizeof(desc));
   vrend_read_from_iovec(desc_res->iov, desc_res->num_iovs, 0, (char *)&desc,
                         MIN2(desc_res->base.width0, sizeof(desc)));
   if (submit_frame(codec, source, &desc, dest, feed))
      return 0;  /* the worker writes dest and feedback */
   struct v4l2_job failed = { .dest = dest, .feed = feed };
   write_result(&failed, NULL, 0);
   return -1;
}

int vrend_video_end_frame(struct vrend_video_context *ctx, uint32_t cdc_handle,
                          uint32_t tgt_handle)
{
   return get_codec(ctx, cdc_handle) && get_buffer(ctx, tgt_handle) ? 0 : -1;
}
