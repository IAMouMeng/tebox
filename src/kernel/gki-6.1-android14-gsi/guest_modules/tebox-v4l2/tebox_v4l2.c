// SPDX-License-Identifier: GPL-2.0
/*
 * tebox-v4l2: guest V4L2 driver for the QEMU tebox-v4l2 virtio device, so the
 * stock CIX c2.cix Codec2 HAL drives the host MVX VPU.
 *
 * Stage 2 (control plane): bind virtio id 60, register a decoder and an encoder
 * V4L2 M2M node, forward QUERYCAP/ENUM_FMT/G_FMT/S_FMT over the CMD virtqueue.
 * Stage 3a (this file, data plane): real v4l2-mem2mem + vb2 buffers. REQBUFS/
 * QBUF/DQBUF/STREAMON flow; device_run() forwards the OUTPUT (coded) buffer to
 * the host over the CMD vq and reads the CAPTURE (NV12) buffer back. The buffer
 * bytes ride as extra scatter-gather entries over the guest vb2 pages (one copy
 * host-side between mvx and these pages). Stage 3b swaps that for zero-copy via
 * virtio-gpu resource ids. See thirdparty/vendor-drivers/cix/codec2/
 * PASSTHROUGH-DESIGN.md and tebox-v4l2-abi.h.
 */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/completion.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>
#include "tebox-v4l2-abi.h"

#define TBX_CMD_BUF 4096

struct tebox_dev {
	struct virtio_device *vdev;
	struct virtqueue *cmd_vq;
	struct virtqueue *evt_vq;
	struct v4l2_device v4l2_dev;

	struct mutex cmd_lock;		/* one CMD request at a time */
	struct completion cmd_done;
	void *cmd_req;			/* TBX_CMD_BUF */
	void *cmd_resp;			/* TBX_CMD_BUF */
	u32 seq;

	struct tebox_node {
		struct video_device vdev;
		struct v4l2_m2m_dev *m2m_dev;
		struct tebox_dev *td;
		u32 stream;		/* host stream id from OP_OPEN */
		bool encoder;
		bool open_ok;
	} dec, enc;
};

/* Per-open m2m context. */
struct tebox_ctx {
	struct v4l2_fh fh;
	struct tebox_node *node;
	struct v4l2_m2m_ctx *m2m_ctx;
	struct mutex lock;		/* serialises this ctx's vb2 queues */
	struct v4l2_pix_format_mplane out_fmt;	/* OUTPUT: coded (dec) / raw (enc) */
	struct v4l2_pix_format_mplane cap_fmt;	/* CAPTURE: raw (dec) / coded (enc) */
};

static inline struct tebox_ctx *file2ctx(struct file *file)
{
	return container_of(file->private_data, struct tebox_ctx, fh);
}

/* ---- CMD transport: synchronous request/response over the CMD vq ---------
 * A request may carry extra OUT data (host reads it) and the response extra IN
 * data (host writes it) as additional scatter-gather entries after the fixed
 * header structs.  The data buffers are guest vb2 pages, mapped page-by-page
 * into the vq (virtio indirect descriptors carry the whole request). */

static void tebox_cmd_done(struct virtqueue *vq)
{
	struct tebox_dev *td = vq->vdev->priv;

	complete(&td->cmd_done);
}

/* Build a scatter-gather table over a kernel buffer (vb2 vmalloc pages). */
static int tbx_build_sgt(struct sg_table *t, void *buf, u32 len)
{
	unsigned int npages, off, i;
	struct scatterlist *sg;
	char *p = buf;
	u32 rem = len;
	int ret;

	off = offset_in_page(buf);
	npages = DIV_ROUND_UP(off + len, PAGE_SIZE);
	ret = sg_alloc_table(t, npages, GFP_KERNEL);
	if (ret)
		return ret;
	for_each_sgtable_sg(t, sg, i) {
		struct page *pg = is_vmalloc_addr(p) ? vmalloc_to_page(p)
						     : virt_to_page(p);
		unsigned int n = min_t(u32, rem, PAGE_SIZE - off);

		sg_set_page(sg, pg, n, off);
		p += n;
		rem -= n;
		off = 0;
	}
	return 0;
}

/* op with optional out-data (coded in) and in-data (NV12 out). Either data
 * pointer may be NULL. |out|/|out_cap| is the small response payload struct. */
static int tebox_cmd_data(struct tebox_dev *td, u32 op, u32 stream,
			  const void *in, u32 in_len,
			  void *out_data, u32 out_data_len,
			  void *in_data, u32 in_data_cap,
			  void *out, u32 out_cap, u32 *out_len)
{
	struct tebox_v4l2_cmd_hdr *hdr;
	struct tebox_v4l2_cmd_resp *resp;
	struct scatterlist sg_req, sg_resp;
	struct sg_table out_t = {}, in_t = {};
	struct scatterlist *sgs[4];
	bool have_out = out_data && out_data_len;
	bool have_in = in_data && in_data_cap;
	unsigned int out_sgs, in_sgs, idx, len;
	int ret;

	if (sizeof(*hdr) + in_len > TBX_CMD_BUF ||
	    sizeof(*resp) + out_cap > TBX_CMD_BUF)
		return -EMSGSIZE;

	mutex_lock(&td->cmd_lock);
	hdr = td->cmd_req;
	hdr->op = op;
	hdr->stream = stream;
	hdr->seq = ++td->seq;
	hdr->len = in_len;
	if (in_len)
		memcpy(hdr + 1, in, in_len);

	if (have_out) {
		ret = tbx_build_sgt(&out_t, out_data, out_data_len);
		if (ret)
			goto unlock;
	}
	if (have_in) {
		ret = tbx_build_sgt(&in_t, in_data, in_data_cap);
		if (ret)
			goto free_out;
	}

	sg_init_one(&sg_req, td->cmd_req, sizeof(*hdr) + in_len);
	sg_init_one(&sg_resp, td->cmd_resp, sizeof(*resp) + out_cap);
	idx = 0;
	sgs[idx++] = &sg_req;
	if (have_out)
		sgs[idx++] = out_t.sgl;
	out_sgs = idx;
	sgs[idx++] = &sg_resp;
	if (have_in)
		sgs[idx++] = in_t.sgl;
	in_sgs = idx - out_sgs;

	reinit_completion(&td->cmd_done);
	ret = virtqueue_add_sgs(td->cmd_vq, sgs, out_sgs, in_sgs, td, GFP_KERNEL);
	if (ret)
		goto free_in;
	virtqueue_kick(td->cmd_vq);

	if (!wait_for_completion_timeout(&td->cmd_done, msecs_to_jiffies(2000))) {
		ret = -ETIMEDOUT;
		goto free_in;
	}
	if (!virtqueue_get_buf(td->cmd_vq, &len) || len < sizeof(*resp)) {
		ret = -EIO;
		goto free_in;
	}
	resp = td->cmd_resp;
	ret = resp->result;
	if (ret == 0 && out && out_len) {
		*out_len = min_t(u32, resp->len, out_cap);
		if (*out_len)
			memcpy(out, resp + 1, *out_len);
	}
free_in:
	if (have_in)
		sg_free_table(&in_t);
free_out:
	if (have_out)
		sg_free_table(&out_t);
unlock:
	mutex_unlock(&td->cmd_lock);
	return ret;
}

static int tebox_cmd(struct tebox_dev *td, u32 op, u32 stream, const void *in,
		     u32 in_len, void *out, u32 out_cap, u32 *out_len)
{
	return tebox_cmd_data(td, op, stream, in, in_len, NULL, 0, NULL, 0,
			      out, out_cap, out_len);
}

/* ---- format helpers ------------------------------------------------------ */

static u32 type_dir(enum v4l2_buf_type type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? TEBOX_V4L2_DIR_OUTPUT
					 : TEBOX_V4L2_DIR_CAPTURE;
}

static void tbx_resp_to_pix(const struct tebox_v4l2_format *rsp,
			    struct v4l2_pix_format_mplane *pix)
{
	u32 p;

	pix->pixelformat = rsp->pixelformat;
	pix->width = rsp->width;
	pix->height = rsp->height;
	pix->num_planes = clamp_t(u32, rsp->num_planes, 1, VIDEO_MAX_PLANES);
	pix->field = V4L2_FIELD_NONE;
	for (p = 0; p < pix->num_planes && p < 4; p++) {
		pix->plane_fmt[p].sizeimage = rsp->plane_size[p];
		pix->plane_fmt[p].bytesperline = rsp->plane_stride[p];
	}
}

/* Query the host's current format for a direction into |pix|. */
static void tbx_query_fmt(struct tebox_node *n, u32 dir,
			  struct v4l2_pix_format_mplane *pix)
{
	struct tebox_v4l2_format req = { .dir = dir };
	struct tebox_v4l2_format rsp;
	u32 rlen = 0;

	memset(pix, 0, sizeof(*pix));
	if (tebox_cmd(n->td, TEBOX_V4L2_OP_G_FMT, n->stream, &req, sizeof(req),
		      &rsp, sizeof(rsp), &rlen) || rlen < sizeof(rsp))
		return;
	tbx_resp_to_pix(&rsp, pix);
}

/* ---- V4L2 ioctl ops: forward to the host mvx node ------------------------ */

static int tbx_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct tebox_ctx *ctx = file2ctx(file);

	strscpy(cap->driver, "tebox-v4l2", sizeof(cap->driver));
	snprintf(cap->card, sizeof(cap->card), "tebox mvx %s",
		 ctx->node->encoder ? "encoder" : "decoder");
	strscpy(cap->bus_info, "virtio:tebox-v4l2", sizeof(cap->bus_info));
	cap->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int tbx_enum_fmt(struct tebox_ctx *ctx, u32 dir, struct v4l2_fmtdesc *f)
{
	struct tebox_v4l2_enum_fmt req = { .dir = dir, .index = f->index };
	struct tebox_v4l2_enum_fmt rsp;
	u32 rlen = 0;
	int ret = tebox_cmd(ctx->node->td, TEBOX_V4L2_OP_ENUM_FMT,
			    ctx->node->stream, &req, sizeof(req), &rsp,
			    sizeof(rsp), &rlen);

	if (ret)
		return ret;
	if (rlen < sizeof(rsp) || rsp.pixelformat == 0)
		return -EINVAL;	/* end of list */
	f->pixelformat = rsp.pixelformat;
	f->flags = rsp.flags;
	/* v4l_fill_fmtdesc WARNs (and dump_stacks) on unknown fourccs when
	 * description is empty — MVX proprietary formats trip it on every
	 * ENUM_FMT and peg the guest. Prefill so the kernel skips the WARN. */
	if (!f->description[0]) {
		snprintf(f->description, sizeof(f->description), "%c%c%c%c",
			 (char)(f->pixelformat),
			 (char)(f->pixelformat >> 8),
			 (char)(f->pixelformat >> 16),
			 (char)(f->pixelformat >> 24));
	}
	return 0;
}

static int tbx_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	return tbx_enum_fmt(file2ctx(file), TEBOX_V4L2_DIR_CAPTURE, f);
}

static int tbx_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	return tbx_enum_fmt(file2ctx(file), TEBOX_V4L2_DIR_OUTPUT, f);
}

/* MVX stateful encoder couples CAPTURE coded size to OUTPUT raw size. c2.cix
 * S_FMTs CAPTURE (H264) before OUTPUT; if OUTPUT is still the 64x64 default the
 * host clamps CAPTURE to 64x64 and the component later tears the queue down.
 * Seed OUTPUT to NV12 at the requested CAPTURE geometry first. */
static int tbx_encoder_seed_output(struct tebox_ctx *ctx, u32 width, u32 height)
{
	struct tebox_v4l2_format req = {
		.dir = TEBOX_V4L2_DIR_OUTPUT,
		.pixelformat = V4L2_PIX_FMT_NV12,
		.width = width,
		.height = height,
		.num_planes = 1,
	};
	struct tebox_v4l2_format rsp;
	u32 rlen = 0;
	int ret;

	if (!ctx->node->encoder || !width || !height)
		return 0;
	if (ctx->out_fmt.width == width && ctx->out_fmt.height == height &&
	    ctx->out_fmt.pixelformat)
		return 0;

	req.plane_size[0] = width * height * 3 / 2;
	req.plane_stride[0] = width;
	ret = tebox_cmd(ctx->node->td, TEBOX_V4L2_OP_S_FMT, ctx->node->stream,
			&req, sizeof(req), &rsp, sizeof(rsp), &rlen);
	if (!ret && rlen >= sizeof(rsp))
		tbx_resp_to_pix(&rsp, &ctx->out_fmt);
	return ret;
}

static int tbx_do_fmt(struct tebox_ctx *ctx, u32 op, struct v4l2_format *f)
{
	struct tebox_node *n = ctx->node;
	u32 dir = type_dir(f->type);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	struct tebox_v4l2_format req = {
		.dir = dir, .pixelformat = pix->pixelformat,
		.width = pix->width, .height = pix->height,
		.num_planes = pix->num_planes,
	};
	struct tebox_v4l2_format rsp;
	u32 rlen = 0, p;
	int ret;

	/* The c2.cix encoder sets only the CAPTURE (coded) format, never the
	 * OUTPUT (raw input) one, yet REQBUFS(OUTPUT, DMABUF) needs a valid OUTPUT
	 * format for queue_setup; seed it with NV12 at the same resolution. */
	if (op == TEBOX_V4L2_OP_S_FMT && dir == TEBOX_V4L2_DIR_CAPTURE &&
	    n->encoder && pix->width && pix->height)
		tbx_encoder_seed_output(ctx, pix->width, pix->height);

	for (p = 0; p < pix->num_planes && p < 4 && p < VIDEO_MAX_PLANES; p++) {
		req.plane_size[p] = pix->plane_fmt[p].sizeimage;
		req.plane_stride[p] = pix->plane_fmt[p].bytesperline;
	}
	ret = tebox_cmd(n->td, op, n->stream, &req, sizeof(req), &rsp,
			sizeof(rsp), &rlen);
	if (ret)
		return ret;
	if (rlen < sizeof(rsp))
		return -EIO;
	tbx_resp_to_pix(&rsp, pix);
	if (op == TEBOX_V4L2_OP_S_FMT) {
		if (dir == TEBOX_V4L2_DIR_OUTPUT)
			ctx->out_fmt = *pix;
		else
			ctx->cap_fmt = *pix;
	}
	return 0;
}

static int tbx_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	return tbx_do_fmt(file2ctx(file), TEBOX_V4L2_OP_G_FMT, f);
}

static int tbx_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	return tbx_do_fmt(file2ctx(file), TEBOX_V4L2_OP_S_FMT, f);
}

/* VIDIOC_(TRY_)ENCODER_CMD: forward to the host mvx encoder. The c2.cix
 * encoder probes V4L2_ENC_CMD_STOP support right after open and aborts if the
 * node does not implement these, so both the try and the real command must be
 * honoured (flush/drain at EOS also rides this). */
static int tbx_do_encoder_cmd(struct tebox_ctx *ctx, struct v4l2_encoder_cmd *ec,
			      bool is_try)
{
	struct tebox_node *n = ctx->node;
	struct tebox_v4l2_encoder_cmd req = {
		.cmd = ec->cmd, .flags = ec->flags, .is_try = is_try ? 1 : 0,
	};
	u32 rlen = 0;

	return tebox_cmd(n->td, TEBOX_V4L2_OP_ENCODER_CMD, n->stream,
			 &req, sizeof(req), &req, sizeof(req), &rlen);
}

static int tbx_encoder_cmd(struct file *file, void *priv,
			   struct v4l2_encoder_cmd *ec)
{
	return tbx_do_encoder_cmd(file2ctx(file), ec, false);
}

static int tbx_try_encoder_cmd(struct file *file, void *priv,
			       struct v4l2_encoder_cmd *ec)
{
	return tbx_do_encoder_cmd(file2ctx(file), ec, true);
}

/* Controls: the c2.cix encoder sets bitrate/bitrate-mode/profile/level/GOP via
 * VIDIOC_S_EXT_CTRLS right after open and treats a failure as fatal ("Set
 * bitrate control enable failed" -> Failed to create V4L2Encoder). Forward each
 * (integer) control to the host mvx as an OP_S_CTRL/OP_G_CTRL. */
static int tbx_one_ctrl(struct tebox_ctx *ctx, u32 op, u32 id, s32 *val)
{
	struct tebox_v4l2_ctrl req = { .id = id, .value = *val };
	u32 rlen = 0;
	int ret = tebox_cmd(ctx->node->td, op, ctx->node->stream,
			    &req, sizeof(req), &req, sizeof(req), &rlen);
	if (!ret && op == TEBOX_V4L2_OP_G_CTRL && rlen >= sizeof(req))
		*val = req.value;
	return ret;
}

static int tbx_s_ctrl(struct file *file, void *priv, struct v4l2_control *c)
{
	return tbx_one_ctrl(file2ctx(file), TEBOX_V4L2_OP_S_CTRL, c->id, &c->value);
}

static int tbx_g_ctrl(struct file *file, void *priv, struct v4l2_control *c)
{
	return tbx_one_ctrl(file2ctx(file), TEBOX_V4L2_OP_G_CTRL, c->id, &c->value);
}

static int tbx_ext_ctrls(struct tebox_ctx *ctx, u32 op, struct v4l2_ext_controls *ec)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ec->count; i++) {
		struct v4l2_ext_control *k = &ec->controls[i];
		s32 v = k->value;

		ret = tbx_one_ctrl(ctx, op, k->id, &v);
		if (ret) {
			ec->error_idx = i;
			return ret;
		}
		if (op == TEBOX_V4L2_OP_G_CTRL)
			k->value = v;
	}
	return 0;
}

static int tbx_s_ext_ctrls(struct file *file, void *priv, struct v4l2_ext_controls *ec)
{
	return tbx_ext_ctrls(file2ctx(file), TEBOX_V4L2_OP_S_CTRL, ec);
}

static int tbx_try_ext_ctrls(struct file *file, void *priv, struct v4l2_ext_controls *ec)
{
	/* Accept; the real set happens at S_EXT_CTRLS. mvx validates there. */
	return 0;
}

static int tbx_g_ext_ctrls(struct file *file, void *priv, struct v4l2_ext_controls *ec)
{
	return tbx_ext_ctrls(file2ctx(file), TEBOX_V4L2_OP_G_CTRL, ec);
}

/* The c2.cix encoder subscribes to V4L2_EVENT_EOS (and the decoder to
 * SOURCE_CHANGE) and runs a device poll thread that waits on those; without
 * subscribe-event support the ioctl returns ENOTTY and the poll thread aborts.
 * The fh already carries an event queue; just register the handlers and mirror
 * the subscription to the host so mvx raises the events. */
static int tbx_subscribe_event(struct v4l2_fh *fh,
			       const struct v4l2_event_subscription *sub)
{
	struct tebox_ctx *ctx = container_of(fh, struct tebox_ctx, fh);
	struct tebox_v4l2_dir_arg arg = { .dir = 0 };

	tebox_cmd(ctx->node->td, TEBOX_V4L2_OP_SUBSCRIBE_EVENT, ctx->node->stream,
		  &arg, sizeof(arg), NULL, 0, NULL);
	return v4l2_event_subscribe(fh, sub, 2, NULL);
}

/* The c2.cix encoder sets the frame rate with VIDIOC_S_PARM and treats ENOTTY
 * as fatal (it is the last ioctl before the encoder gives up). The rate is also
 * carried in the ext-ctrls we forward, so accept S_PARM/G_PARM here (echo the
 * requested timeperframe, advertise TIMEPERFRAME capability) to let it proceed. */
static int tbx_s_parm(struct file *file, void *priv, struct v4l2_streamparm *a)
{
	if (V4L2_TYPE_IS_OUTPUT(a->type))
		a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	else
		a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	return 0;
}

static int tbx_g_parm(struct file *file, void *priv, struct v4l2_streamparm *a)
{
	if (V4L2_TYPE_IS_OUTPUT(a->type)) {
		a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
		if (!a->parm.output.timeperframe.denominator) {
			a->parm.output.timeperframe.numerator = 1;
			a->parm.output.timeperframe.denominator = 30;
		}
	} else {
		a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
		if (!a->parm.capture.timeperframe.denominator) {
			a->parm.capture.timeperframe.numerator = 1;
			a->parm.capture.timeperframe.denominator = 30;
		}
	}
	return 0;
}

/* c2.cix probes VIDIOC_ENUM_FRAMESIZES; without it GetSupportedResolution falls
 * back to 16x16..1920x1088 and scrcpy may reject some sizes. Advertise a
 * stepwise range that covers phone portrait + 1080p. */
static int tbx_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index > 0)
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = 16;
	fsize->stepwise.min_height = 16;
	/* Match media_codecs_c2.xml encoder max (8192) so portrait 1080x2400
	 * and scrcpy -m* sizes are not rejected at GetSupportedResolution. */
	fsize->stepwise.max_width = 8192;
	fsize->stepwise.max_height = 8192;
	fsize->stepwise.step_width = 2;
	fsize->stepwise.step_height = 2;
	return 0;
}

/* Host mvxenc has no VIDIOC_S_SELECTION / S_CROP (ENOTTY). c2.cix still calls
 * them after CAPTURE REQBUFS to set the OUTPUT visible rectangle; without a
 * guest stub it logs "Failed to crop to specified visible rectangle" and
 * tears the session down (C2_CORRUPTED 0xe). Accept full-frame crop locally. */
static struct v4l2_pix_format_mplane *tbx_fmt_for_buf_type(struct tebox_ctx *ctx,
							  __u32 type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &ctx->out_fmt : &ctx->cap_fmt;
}

static void tbx_full_rect(const struct v4l2_pix_format_mplane *fmt,
			  struct v4l2_rect *r)
{
	r->left = 0;
	r->top = 0;
	r->width = fmt->width ? fmt->width : 1;
	r->height = fmt->height ? fmt->height : 1;
}

static int tbx_g_selection(struct file *file, void *priv,
			   struct v4l2_selection *s)
{
	struct tebox_ctx *ctx = file2ctx(file);
	struct v4l2_pix_format_mplane *fmt = tbx_fmt_for_buf_type(ctx, s->type);

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		tbx_full_rect(fmt, &s->r);
		return 0;
	default:
		return -EINVAL;
	}
}

static int tbx_s_selection(struct file *file, void *priv,
			   struct v4l2_selection *s)
{
	struct tebox_ctx *ctx = file2ctx(file);
	struct v4l2_pix_format_mplane *fmt = tbx_fmt_for_buf_type(ctx, s->type);
	struct v4l2_rect full;

	if (s->target != V4L2_SEL_TGT_CROP && s->target != V4L2_SEL_TGT_COMPOSE)
		return -EINVAL;

	tbx_full_rect(fmt, &full);
	/* Soft-accept: clamp to the current format. Encoder visible-rect crop
	 * equal to the full frame is the only case we need for c2.cix/scrcpy. */
	if (s->r.left < 0 || s->r.top < 0 ||
	    s->r.width == 0 || s->r.height == 0 ||
	    (u32)s->r.left + s->r.width > full.width ||
	    (u32)s->r.top + s->r.height > full.height) {
		pr_info("tbxv: S_SELECTION clamp %dx%d@%d,%d -> full %ux%u\n",
			s->r.width, s->r.height, s->r.left, s->r.top,
			full.width, full.height);
		s->r = full;
	}
	pr_info("tbxv: S_SELECTION type=%u tgt=%u -> %dx%d@%d,%d\n",
		s->type, s->target, s->r.width, s->r.height, s->r.left, s->r.top);
	return 0;
}

static int tbx_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	/* Ask the host to adjust the requested format to what the node supports
	 * (VIDIOC_TRY_FMT: honours the requested width/height/fourcc, aligns to
	 * the hardware, no commit). Forwarding this as G_FMT was wrong -- it
	 * ignored the request and always echoed the 64x64 default, so an encoder
	 * negotiating e.g. 1280x720 saw 64x64 and gave up before S_FMT. */
	return tbx_do_fmt(file2ctx(file), TEBOX_V4L2_OP_TRY_FMT, f);
}

/* REQBUFS: do the guest vb2 allocation, then mirror to the host mvx queue. */
static int tbx_reqbufs(struct file *file, void *priv,
		       struct v4l2_requestbuffers *rb)
{
	struct tebox_ctx *ctx = file2ctx(file);
	struct tebox_v4l2_reqbufs req = {
		.dir = type_dir(rb->type), .count = rb->count,
	};
	u32 rlen = 0;
	int ret = v4l2_m2m_ioctl_reqbufs(file, priv, rb);

	if (ret)
		return ret;
	tebox_cmd(ctx->node->td, TEBOX_V4L2_OP_REQBUFS, ctx->node->stream,
		  &req, sizeof(req), &req, sizeof(req), &rlen);
	return 0;
}

static const struct v4l2_ioctl_ops tebox_ioctl_ops = {
	.vidioc_querycap = tbx_querycap,
	.vidioc_enum_fmt_vid_cap = tbx_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out = tbx_enum_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = tbx_g_fmt,
	.vidioc_g_fmt_vid_out_mplane = tbx_g_fmt,
	.vidioc_s_fmt_vid_cap_mplane = tbx_s_fmt,
	.vidioc_s_fmt_vid_out_mplane = tbx_s_fmt,
	.vidioc_try_fmt_vid_cap_mplane = tbx_try_fmt,
	.vidioc_try_fmt_vid_out_mplane = tbx_try_fmt,

	.vidioc_reqbufs = tbx_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_encoder_cmd = tbx_encoder_cmd,
	.vidioc_try_encoder_cmd = tbx_try_encoder_cmd,

	.vidioc_s_ctrl = tbx_s_ctrl,
	.vidioc_g_ctrl = tbx_g_ctrl,
	.vidioc_s_ext_ctrls = tbx_s_ext_ctrls,
	.vidioc_g_ext_ctrls = tbx_g_ext_ctrls,
	.vidioc_try_ext_ctrls = tbx_try_ext_ctrls,

	.vidioc_subscribe_event = tbx_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_s_parm = tbx_s_parm,
	.vidioc_g_parm = tbx_g_parm,

	.vidioc_enum_framesizes = tbx_enum_framesizes,

	/* Legacy VIDIOC_G/S_CROP is translated by v4l2-core to these. */
	.vidioc_g_selection = tbx_g_selection,
	.vidioc_s_selection = tbx_s_selection,
};

/* ---- vb2 queue ops ------------------------------------------------------- */

static int tebox_queue_setup(struct vb2_queue *vq, unsigned int *nbufs,
			     unsigned int *nplanes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	struct tebox_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *fmt =
		V4L2_TYPE_IS_OUTPUT(vq->type) ? &ctx->out_fmt : &ctx->cap_fmt;
	unsigned int np = fmt->num_planes ? fmt->num_planes : 1;
	unsigned int p;

	if (*nplanes) {
		if (*nplanes != np)
			return -EINVAL;
	} else {
		*nplanes = np;
	}
	for (p = 0; p < np; p++) {
		u32 sz = fmt->plane_fmt[p].sizeimage;

		if (!sz) {
			/* Raw planes: YUV 4:2:0 footprint. Coded (H264/…) CAPTURE
			 * planes: a compressed bitstream budget — never the YUV
			 * size, or c2.cix frees the queue after QUERYBUF. */
			if (V4L2_TYPE_IS_OUTPUT(vq->type) ||
			    fmt->pixelformat == V4L2_PIX_FMT_NV12 ||
			    fmt->pixelformat == V4L2_PIX_FMT_NV12M ||
			    fmt->pixelformat == V4L2_PIX_FMT_YUV420) {
				sz = fmt->width * fmt->height * 3 / 2;
				if (np > 1 && p > 0)
					sz = fmt->width * fmt->height / 2;
			} else {
				sz = max_t(u32, fmt->width * fmt->height, SZ_1M);
			}
		}
		if (!sz)
			sz = SZ_1M;
		sizes[p] = sz;
		/* Keep ctx in sync so later QUERYBUF / userspace see a real size. */
		fmt->plane_fmt[p].sizeimage = sz;
	}
	return 0;
}

static int tebox_buf_prepare(struct vb2_buffer *vb)
{
	struct tebox_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *fmt =
		V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type) ? &ctx->out_fmt
							 : &ctx->cap_fmt;
	unsigned int p, np = fmt->num_planes ? fmt->num_planes : 1;

	for (p = 0; p < np; p++) {
		u32 sz = fmt->plane_fmt[p].sizeimage;

		/* CAPTURE payload is set by the host at decode time; only make
		 * sure the buffer is large enough. */
		if (sz && vb2_plane_size(vb, p) < sz)
			return -EINVAL;
	}
	return 0;
}

static void tebox_buf_queue(struct vb2_buffer *vb)
{
	struct tebox_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int tebox_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct tebox_ctx *ctx = vb2_get_drv_priv(vq);
	struct tebox_v4l2_dir_arg arg = { .dir = type_dir(vq->type) };
	int ret = tebox_cmd(ctx->node->td, TEBOX_V4L2_OP_STREAMON,
			    ctx->node->stream, &arg, sizeof(arg), NULL, 0, NULL);

	pr_info("tebox_v4l2: start_streaming dir=%u count=%u -> ret=%d\n",
		type_dir(vq->type), count, ret);
	if (ret) {
		struct vb2_v4l2_buffer *vbuf;

		while ((vbuf = V4L2_TYPE_IS_OUTPUT(vq->type)
				? v4l2_m2m_src_buf_remove(ctx->m2m_ctx)
				: v4l2_m2m_dst_buf_remove(ctx->m2m_ctx)))
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_QUEUED);
	}
	return ret;
}

static void tebox_stop_streaming(struct vb2_queue *vq)
{
	struct tebox_ctx *ctx = vb2_get_drv_priv(vq);
	struct tebox_v4l2_dir_arg arg = { .dir = type_dir(vq->type) };
	struct vb2_v4l2_buffer *vbuf;

	tebox_cmd(ctx->node->td, TEBOX_V4L2_OP_STREAMOFF, ctx->node->stream,
		  &arg, sizeof(arg), NULL, 0, NULL);
	while ((vbuf = V4L2_TYPE_IS_OUTPUT(vq->type)
			? v4l2_m2m_src_buf_remove(ctx->m2m_ctx)
			: v4l2_m2m_dst_buf_remove(ctx->m2m_ctx)))
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops tebox_vb2_ops = {
	.queue_setup = tebox_queue_setup,
	.buf_prepare = tebox_buf_prepare,
	.buf_queue = tebox_buf_queue,
	.start_streaming = tebox_start_streaming,
	.stop_streaming = tebox_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

/* ---- m2m ops ------------------------------------------------------------- */

/* 1:1 device_run for both decoder (OUTPUT=coded, CAPTURE=NV12) and encoder
 * (OUTPUT=raw, CAPTURE=coded). Multi-plane OUTPUT (e.g. NV12 Y+UV) is
 * concatenated into one contiguous out-sg so the host can peel planes by
 * plane_bytesused[]. Stage 3b replaces this with async EVT completions +
 * virtio-gpu resource_id (Surface/AB24 CPU vmap is zeros on gfxstream). */
static void tebox_device_run(void *priv)
{
	struct tebox_ctx *ctx = priv;
	struct tebox_node *n = ctx->node;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);
	struct tebox_v4l2_qbuf qb;
	struct tebox_v4l2_dqbuf dqreq, dqrsp;
	struct v4l2_pix_format_mplane *ofmt = &ctx->out_fmt;
	void *cap_ptr, *out_ptr = NULL, *tmp = NULL;
	u32 np, p, total = 0, capcap, rlen = 0;
	int ret;

	if (!src || !dst) {
		v4l2_m2m_job_finish(n->m2m_dev, ctx->m2m_ctx);
		return;
	}

	np = ofmt->num_planes ? ofmt->num_planes : 1;
	if (np > 4)
		np = 4;
	for (p = 0; p < np; p++)
		total += vb2_get_plane_payload(&src->vb2_buf, p);

	memset(&qb, 0, sizeof(qb));
	qb.dir = TEBOX_V4L2_DIR_OUTPUT;
	qb.index = src->vb2_buf.index;
	qb.num_planes = np;
	qb.timestamp_us = src->vb2_buf.timestamp;
	if (src->flags & V4L2_BUF_FLAG_KEYFRAME)
		qb.flags |= V4L2_BUF_FLAG_KEYFRAME;
	for (p = 0; p < np; p++)
		qb.plane_bytesused[p] = vb2_get_plane_payload(&src->vb2_buf, p);

	if (np == 1) {
		out_ptr = vb2_plane_vaddr(&src->vb2_buf, 0);
	} else {
		u32 off = 0;

		tmp = kmalloc(total, GFP_KERNEL);
		if (!tmp) {
			ret = -ENOMEM;
			goto done;
		}
		for (p = 0; p < np; p++) {
			void *v = vb2_plane_vaddr(&src->vb2_buf, p);
			u32 len = qb.plane_bytesused[p];

			if (v && len)
				memcpy(tmp + off, v, len);
			off += len;
		}
		out_ptr = tmp;
	}

	ret = tebox_cmd_data(n->td, TEBOX_V4L2_OP_QBUF, n->stream,
			     &qb, sizeof(qb), out_ptr, total, NULL, 0,
			     NULL, 0, NULL);

	cap_ptr = vb2_plane_vaddr(&dst->vb2_buf, 0);
	capcap = vb2_plane_size(&dst->vb2_buf, 0);
	memset(&dqreq, 0, sizeof(dqreq));
	dqreq.dir = TEBOX_V4L2_DIR_CAPTURE;
	memset(&dqrsp, 0, sizeof(dqrsp));
	if (!ret)
		ret = tebox_cmd_data(n->td, TEBOX_V4L2_OP_DQBUF, n->stream,
				     &dqreq, sizeof(dqreq), NULL, 0, cap_ptr,
				     capcap, &dqrsp, sizeof(dqrsp), &rlen);

done:
	kfree(tmp);
	src = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);

	if (!ret && rlen >= sizeof(dqrsp)) {
		vb2_set_plane_payload(&dst->vb2_buf, 0, dqrsp.plane_bytesused[0]);
		dst->vb2_buf.timestamp = dqrsp.timestamp_us;
		dst->field = V4L2_FIELD_NONE;
		if (dqrsp.flags & V4L2_BUF_FLAG_LAST)
			dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
	} else {
		vb2_set_plane_payload(&dst->vb2_buf, 0, 0);
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst, ret && ret != -EAGAIN
					? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	}
	v4l2_m2m_job_finish(n->m2m_dev, ctx->m2m_ctx);
}

static const struct v4l2_m2m_ops tebox_m2m_ops = {
	.device_run = tebox_device_run,
};

/* ---- queue init / file ops ----------------------------------------------- */

static int tebox_queue_init(void *priv, struct vb2_queue *src_vq,
			    struct vb2_queue *dst_vq)
{
	struct tebox_ctx *ctx = priv;
	int ret;

	/* Both queues need DMABUF: c2.cix hard-requires it on CAPTURE (type 9 =
	 * CAPTURE_MPLANE, coded out) and uses it on OUTPUT for surface frames.
	 * vb2_vmalloc_memops provides attach/map via dma_buf_vmap for the copy
	 * datapath. */
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &tebox_vb2_ops;
	src_vq->mem_ops = &vb2_vmalloc_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->lock;
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &tebox_vb2_ops;
	dst_vq->mem_ops = &vb2_vmalloc_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->lock;
	return vb2_queue_init(dst_vq);
}

static int tebox_open(struct file *file)
{
	struct tebox_node *n = video_drvdata(file);
	struct tebox_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->node = n;
	mutex_init(&ctx->lock);
	v4l2_fh_init(&ctx->fh, &n->vdev);
	file->private_data = &ctx->fh;

	/* Seed both formats from the host's current state. */
	tbx_query_fmt(n, TEBOX_V4L2_DIR_OUTPUT, &ctx->out_fmt);
	tbx_query_fmt(n, TEBOX_V4L2_DIR_CAPTURE, &ctx->cap_fmt);

	ctx->m2m_ctx = v4l2_m2m_ctx_init(n->m2m_dev, ctx, tebox_queue_init);
	if (IS_ERR(ctx->m2m_ctx)) {
		ret = PTR_ERR(ctx->m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		mutex_destroy(&ctx->lock);
		kfree(ctx);
		return ret;
	}
	ctx->fh.m2m_ctx = ctx->m2m_ctx;
	v4l2_fh_add(&ctx->fh);
	return 0;
}

static int tebox_release(struct file *file)
{
	struct tebox_ctx *ctx = file2ctx(file);

	v4l2_m2m_ctx_release(ctx->m2m_ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	mutex_destroy(&ctx->lock);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations tebox_fops = {
	.owner = THIS_MODULE,
	.open = tebox_open,
	.release = tebox_release,
	.poll = v4l2_m2m_fop_poll,
	.mmap = v4l2_m2m_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* ---- probe / remove ------------------------------------------------------ */

static int tebox_open_stream(struct tebox_dev *td, struct tebox_node *n, u32 node_id)
{
	struct tebox_v4l2_open req = { .node = node_id };
	u32 sid = 0, rlen = 0;
	int ret = tebox_cmd(td, TEBOX_V4L2_OP_OPEN, 0, &req, sizeof(req),
			    &sid, sizeof(sid), &rlen);

	if (ret || rlen < sizeof(sid))
		return ret ? ret : -EIO;
	n->stream = sid;
	n->open_ok = true;
	return 0;
}

static int tebox_register_node(struct tebox_dev *td, struct tebox_node *n,
			       bool encoder, u32 node_id, const char *name)
{
	int ret;

	n->td = td;
	n->encoder = encoder;
	ret = tebox_open_stream(td, n, node_id);
	if (ret) {
		dev_err(&td->vdev->dev, "OPEN %s failed: %d\n", name, ret);
		return ret;
	}
	n->m2m_dev = v4l2_m2m_init(&tebox_m2m_ops);
	if (IS_ERR(n->m2m_dev)) {
		dev_err(&td->vdev->dev, "m2m_init %s failed\n", name);
		return PTR_ERR(n->m2m_dev);
	}
	n->vdev.v4l2_dev = &td->v4l2_dev;
	n->vdev.fops = &tebox_fops;
	n->vdev.ioctl_ops = &tebox_ioctl_ops;
	n->vdev.release = video_device_release_empty;
	n->vdev.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	n->vdev.vfl_dir = VFL_DIR_M2M;
	snprintf(n->vdev.name, sizeof(n->vdev.name), "%s", name);
	video_set_drvdata(&n->vdev, n);
	ret = video_register_device(&n->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&td->vdev->dev, "register %s failed: %d\n", name, ret);
		v4l2_m2m_release(n->m2m_dev);
		return ret;
	}
	dev_info(&td->vdev->dev, "%s = /dev/video%d (stream %u)\n", name,
		 n->vdev.num, n->stream);
	return 0;
}

static void tebox_unregister_node(struct tebox_node *n)
{
	if (!n->open_ok)
		return;
	video_unregister_device(&n->vdev);
	if (!IS_ERR_OR_NULL(n->m2m_dev))
		v4l2_m2m_release(n->m2m_dev);
}

static int tebox_probe(struct virtio_device *vdev)
{
	struct tebox_dev *td;
	struct virtqueue *vqs[TEBOX_V4L2_VQ_MAX];
	vq_callback_t *cbs[TEBOX_V4L2_VQ_MAX] = { tebox_cmd_done, NULL };
	static const char * const names[TEBOX_V4L2_VQ_MAX] = { "cmd", "evt" };
	int ret;

	td = devm_kzalloc(&vdev->dev, sizeof(*td), GFP_KERNEL);
	if (!td)
		return -ENOMEM;
	td->vdev = vdev;
	vdev->priv = td;
	mutex_init(&td->cmd_lock);
	init_completion(&td->cmd_done);
	td->cmd_req = devm_kzalloc(&vdev->dev, TBX_CMD_BUF, GFP_KERNEL);
	td->cmd_resp = devm_kzalloc(&vdev->dev, TBX_CMD_BUF, GFP_KERNEL);
	if (!td->cmd_req || !td->cmd_resp)
		return -ENOMEM;

	ret = virtio_find_vqs(vdev, TEBOX_V4L2_VQ_MAX, vqs, cbs, names, NULL);
	if (ret)
		return ret;
	td->cmd_vq = vqs[TEBOX_V4L2_VQ_CMD];
	td->evt_vq = vqs[TEBOX_V4L2_VQ_EVT];

	ret = v4l2_device_register(&vdev->dev, &td->v4l2_dev);
	if (ret)
		goto del_vqs;

	virtio_device_ready(vdev);

	ret = tebox_register_node(td, &td->dec, false, TEBOX_V4L2_NODE_DECODER,
				  "tebox-mvx-dec");
	if (ret)
		goto unreg_v4l2;
	ret = tebox_register_node(td, &td->enc, true, TEBOX_V4L2_NODE_ENCODER,
				  "tebox-mvx-enc");
	if (ret)
		goto unreg_dec;

	dev_info(&vdev->dev, "tebox-v4l2 ready\n");
	return 0;

unreg_dec:
	tebox_unregister_node(&td->dec);
unreg_v4l2:
	v4l2_device_unregister(&td->v4l2_dev);
del_vqs:
	vdev->config->del_vqs(vdev);
	return ret;
}

static void tebox_remove(struct virtio_device *vdev)
{
	struct tebox_dev *td = vdev->priv;

	tebox_unregister_node(&td->enc);
	tebox_unregister_node(&td->dec);
	v4l2_device_unregister(&td->v4l2_dev);
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
}

static const struct virtio_device_id tebox_id_table[] = {
	{ TEBOX_V4L2_VIRTIO_ID, VIRTIO_DEV_ANY_ID },
	{ 0 },
};
MODULE_DEVICE_TABLE(virtio, tebox_id_table);

static struct virtio_driver tebox_v4l2_driver = {
	.driver.name = "tebox-v4l2",
	.id_table = tebox_id_table,
	.probe = tebox_probe,
	.remove = tebox_remove,
};
module_virtio_driver(tebox_v4l2_driver);

MODULE_DESCRIPTION("tebox MVX V4L2 passthrough (guest)");
MODULE_LICENSE("GPL");
