/*
 * cv610_ive_bench — measure the Hi3516CV610 IVE operators an optical-flow
 * port would lean on: Shi-Tomasi corners, pyramid construction (resize and
 * DMA interval-copy), and pyramidal Lucas-Kanade.  Synthetic textured frames
 * with a known shift give wall/CPU cost per operator AND an accuracy check,
 * so one run answers "is the hardware path fast enough and does it track".
 *
 * Nothing in this repository has numbers for either side of the comparison:
 * the Star6E OptFlow branch never captured its perf log, and HiSilicon
 * publishes no IVE timings.  This tool exists to settle that before any
 * pipeline code is written (see documentation/CV610_OPTICAL_FLOW.md).
 *
 * DEV-ONLY.  Not installed into any image; push it to /tmp and delete it.
 *
 * Build:
 *   make cv610-ive-bench SOC_BUILD=cv610 CV610_CC=... CV610_SDK_INC=... CV610_SDK_LIB=...
 *
 * Run on the bench with waybeam stopped and the MPP modules (incl. open_ive)
 * loaded:
 *   scp -O out/cv610/cv610_ive_bench root@192.168.2.181:/tmp/
 *   ssh root@192.168.2.181 /tmp/cv610_ive_bench
 *   ssh root@192.168.2.181 /tmp/cv610_ive_bench --iters 100 --sizes 640x360 --points 100
 *
 * Every measurement is one line, "size=WxH op=... wall_us=min/med/max
 * cpu_us=min/med/max ...", so two runs diff cleanly.  A vendor call that
 * fails prints its return code and the run continues: the error codes are
 * part of what this tool is for (aux-buffer sizing rules, level limits).
 */

#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "ot_type.h"
#include "ot_common.h"
#include "ot_common_svp.h"
#include "ot_common_ive.h"
#include "ot_common_vb.h"
#include "ss_mpi_ive.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_sys_mem.h"
#include "ss_mpi_vb.h"

#define MAX_SIZES      4
#define MAX_POINT_SETS 4
#define MAX_ITERS      1000
#define MAX_LEVELS     4   /* L0 + max_level 3 */
#define MIN_LEVEL_SIDE 64u /* IVE: every pyramid level >= 64x64 */
#define MAX_L0_WIDTH   1280u
#define MAX_L0_HEIGHT  720u

/* LK control values the OptFlow branch settled on for SigmaStar; kept so the
 * two SoCs are compared on the same solver settings. */
#define LK_MIN_EIG_THR 8
#define LK_ITER_CNT    20
#define LK_EPS         4
#define ST_QUALITY     25
#define ST_MIN_DIST    8

/* The shift applied to frame B.  Small enough for a single level at 320x180,
 * large enough that level 0 alone cannot follow it at 1280x720 — so the
 * pyramid is actually exercised. */
#define SHIFT_DX 5
#define SHIFT_DY (-3)

typedef struct {
	td_phys_addr_t phys;
	void *virt;
	td_u32 size;
} IveBuf;

typedef struct {
	IveBuf buf;
	ot_svp_img level[MAX_LEVELS];
	unsigned levels;
} Pyramid;

typedef struct {
	unsigned w;
	unsigned h;
} Size;

typedef struct {
	unsigned iters;
	unsigned size_count;
	Size sizes[MAX_SIZES];
	unsigned point_count;
	unsigned points[MAX_POINT_SETS];
	td_u32 resize_mem;
	int no_sys;
} Options;

typedef struct {
	double wall[MAX_ITERS];
	double cpu[MAX_ITERS];
	unsigned n;
} Samples;

static unsigned align16(unsigned v)
{
	return (v + 15u) & ~15u;
}

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static double cpu_us(void)
{
	struct rusage ru;

	getrusage(RUSAGE_THREAD, &ru);
	return (double)ru.ru_utime.tv_sec * 1e6 + (double)ru.ru_utime.tv_usec +
		(double)ru.ru_stime.tv_sec * 1e6 + (double)ru.ru_stime.tv_usec;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static void samples_add(Samples *s, double wall, double cpu)
{
	if (s->n < MAX_ITERS) {
		s->wall[s->n] = wall;
		s->cpu[s->n] = cpu;
		s->n++;
	}
}

/* " wall_us=min/med/max cpu_us=min/med/max [ret=0x..]\n" — the tail of every
 * measurement line. */
static void print_tail(Samples *s, td_s32 ret)
{
	if (s->n == 0) {
		printf(" wall_us=- cpu_us=-");
	} else {
		qsort(s->wall, s->n, sizeof(double), cmp_double);
		qsort(s->cpu, s->n, sizeof(double), cmp_double);
		printf(" wall_us=%.0f/%.0f/%.0f cpu_us=%.0f/%.0f/%.0f",
			s->wall[0], s->wall[s->n / 2], s->wall[s->n - 1],
			s->cpu[0], s->cpu[s->n / 2], s->cpu[s->n - 1]);
	}
	if (ret != TD_SUCCESS)
		printf(" ret=0x%x", (unsigned)ret);
	printf("\n");
}

/* ── MMZ ─────────────────────────────────────────────────────────────── */

/* Non-cached on purpose: IVE reads what the CPU wrote with no flush
 * discipline to get wrong, and the port intends the same for its buffers. */
static int ive_buf_alloc(IveBuf *b, td_u32 size, const char *name)
{
	td_s32 ret;

	memset(b, 0, sizeof(*b));
	ret = ss_mpi_sys_mmz_alloc(&b->phys, &b->virt, name, NULL, size);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "mmz_alloc(%s, %u) failed: 0x%x\n", name, size, ret);
		return -1;
	}
	b->size = size;
	memset(b->virt, 0, size);
	return 0;
}

static void ive_buf_free(IveBuf *b)
{
	if (b->virt)
		(void)ss_mpi_sys_mmz_free(b->phys, b->virt);
	memset(b, 0, sizeof(*b));
}

static void mem_info_set(ot_svp_mem_info *m, const IveBuf *b)
{
	m->phys_addr = (td_u64)b->phys;
	m->virt_addr = (td_u64)(uintptr_t)b->virt;
	m->size = b->size;
}

/* A U8C1 view at byte offset `off` of a buffer; the caller keeps offsets
 * 16-byte aligned by construction (all strides are multiples of 16). */
static void img_set(ot_svp_img *img, const IveBuf *b, td_u32 off, unsigned w, unsigned h)
{
	memset(img, 0, sizeof(*img));
	img->phys_addr[0] = (td_u64)b->phys + off;
	img->virt_addr[0] = (td_u64)(uintptr_t)((uint8_t *)b->virt + off);
	img->stride[0] = align16(w);
	img->width = w;
	img->height = h;
	img->type = OT_SVP_IMG_TYPE_U8C1;
}

static uint8_t *img_pixels(const ot_svp_img *img)
{
	return (uint8_t *)(uintptr_t)img->virt_addr[0];
}

/* Level k is level 0 shifted right k times; every level must stay >= 64 on
 * both sides and even, which is what bounds max_level per tap size. */
static unsigned levels_for(unsigned w, unsigned h)
{
	unsigned n = 1;

	while (n < MAX_LEVELS) {
		unsigned lw = w >> n, lh = h >> n;

		if (lw < MIN_LEVEL_SIDE || lh < MIN_LEVEL_SIDE || (lw & 1u) || (lh & 1u))
			break;
		n++;
	}
	return n;
}

static int pyramid_alloc(Pyramid *p, unsigned w, unsigned h, const char *name)
{
	td_u32 off = 0;
	unsigned k;

	p->levels = levels_for(w, h);
	for (k = 0; k < p->levels; k++)
		off += align16(w >> k) * (h >> k);
	if (ive_buf_alloc(&p->buf, off, name) != 0)
		return -1;
	off = 0;
	for (k = 0; k < p->levels; k++) {
		img_set(&p->level[k], &p->buf, off, w >> k, h >> k);
		off += align16(w >> k) * (h >> k);
	}
	return 0;
}

/* ── synthetic frames ────────────────────────────────────────────────── */

static uint32_t rng_state = 0x9e3779b9u;

static uint32_t rng_next(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* Low-pass random texture: white noise, three 5x5 box passes, contrast
 * stretch.  Enough structure everywhere for corners, no repeating pattern
 * that would let a wrong match score as well as the right one. */
static void make_texture(uint8_t *dst, unsigned stride, unsigned w, unsigned h)
{
	uint16_t *tmp = malloc((size_t)w * h * sizeof(*tmp) * 2);
	uint16_t *a = tmp, *b = tmp + (size_t)w * h;
	unsigned pass, x, y;
	unsigned lo = 0xffff, hi = 0;

	if (!tmp)
		return;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			a[y * w + x] = (uint16_t)(rng_next() & 0xff);
	for (pass = 0; pass < 3; pass++) {
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				unsigned sum = 0, cnt = 0;
				int dx, dy;

				for (dy = -2; dy <= 2; dy++) {
					int yy = (int)y + dy;

					if (yy < 0 || yy >= (int)h)
						continue;
					for (dx = -2; dx <= 2; dx++) {
						int xx = (int)x + dx;

						if (xx < 0 || xx >= (int)w)
							continue;
						sum += a[(unsigned)yy * w + (unsigned)xx];
						cnt++;
					}
				}
				b[y * w + x] = (uint16_t)(sum / cnt);
			}
		}
		memcpy(a, b, (size_t)w * h * sizeof(*a));
	}
	for (y = 0; y < h * w; y++) {
		if (a[y] < lo)
			lo = a[y];
		if (a[y] > hi)
			hi = a[y];
	}
	if (hi == lo)
		hi = lo + 1;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			dst[y * stride + x] = (uint8_t)((a[y * w + x] - lo) * 255u / (hi - lo));
	free(tmp);
}

/* Frame B = frame A translated by (dx, dy) with edge replication and +-2
 * counts of noise, so a solver that only reports zero motion fails loudly. */
static void shift_image(uint8_t *dst, const uint8_t *src, unsigned stride,
	unsigned w, unsigned h, int dx, int dy)
{
	unsigned x, y;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int sx = (int)x - dx, sy = (int)y - dy;
			int v;

			if (sx < 0)
				sx = 0;
			if (sx >= (int)w)
				sx = (int)w - 1;
			if (sy < 0)
				sy = 0;
			if (sy >= (int)h)
				sy = (int)h - 1;
			v = src[(unsigned)sy * stride + (unsigned)sx] + (int)(rng_next() % 5u) - 2;
			dst[y * stride + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	}
}

/* ── IVE helpers ─────────────────────────────────────────────────────── */

static td_s32 ive_wait(ot_ive_handle h)
{
	td_bool finish = TD_FALSE;
	td_s32 ret;

	do {
		ret = ss_mpi_ive_query(h, &finish, TD_TRUE);
	} while (ret == TD_SUCCESS && !finish);
	return ret;
}

/* Completes a timed submission: wait for the handle if the submit succeeded,
 * record wall/CPU since (w0, c0), hand back the final return code. */
static td_s32 ive_finish(Samples *s, td_s32 ret, ot_ive_handle h, double w0, double c0)
{
	if (ret == TD_SUCCESS)
		ret = ive_wait(h);
	samples_add(s, now_us() - w0, cpu_us() - c0);
	return ret;
}

static void svp_data_from_img(ot_svp_data *d, const ot_svp_img *img)
{
	memset(d, 0, sizeof(*d));
	d->phys_addr = img->phys_addr[0];
	d->virt_addr = img->virt_addr[0];
	d->stride = img->stride[0];
	d->width = img->width;
	d->height = img->height;
}

/* Full-frame copy, the way the port would lift the luma plane out of a VPSS
 * frame into its own level-0 buffer. */
static td_s32 ive_copy(ot_ive_handle *h, const ot_svp_img *src, const ot_svp_img *dst,
	td_bool instant)
{
	ot_svp_data s, d;
	ot_ive_dma_ctrl ctrl;

	svp_data_from_img(&s, src);
	svp_data_from_img(&d, dst);
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.mode = OT_IVE_DMA_MODE_DIRECT_COPY;
	return ss_mpi_ive_dma(h, &s, &d, &ctrl, instant);
}

/* Nearest-neighbour 2x decimation by interval copy: one byte of every two,
 * one row of every two.  This is how the vendor LK sample builds pyramids. */
static td_s32 ive_decimate(ot_ive_handle *h, const ot_svp_img *src, const ot_svp_img *dst,
	td_bool instant)
{
	ot_svp_data s, d;
	ot_ive_dma_ctrl ctrl;

	svp_data_from_img(&s, src);
	svp_data_from_img(&d, dst);
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.mode = OT_IVE_DMA_MODE_INTERVAL_COPY;
	ctrl.hor_seg_size = 2;
	ctrl.elem_size = 1;
	ctrl.ver_seg_rows = 2;
	return ss_mpi_ive_dma(h, &s, &d, &ctrl, instant);
}

/* Bilinear: every level straight from level 0 in one submission. */
static td_s32 ive_resize_levels(ot_ive_handle *h, const Pyramid *p, const IveBuf *mem,
	td_bool instant)
{
	ot_svp_src_img src[MAX_LEVELS];
	ot_ive_resize_ctrl ctrl;
	unsigned k;

	for (k = 1; k < p->levels; k++)
		src[k - 1] = p->level[0];
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.mode = OT_IVE_RESIZE_MODE_LINEAR;
	mem_info_set(&ctrl.mem, mem);
	ctrl.num = (td_u16)(p->levels - 1);
	return ss_mpi_ive_resize(h, src, &p->level[1], &ctrl, instant);
}

static td_s32 ive_decimate_levels(ot_ive_handle *h, const Pyramid *p, td_bool instant)
{
	td_s32 ret = TD_SUCCESS;
	unsigned k;

	for (k = 1; k < p->levels && ret == TD_SUCCESS; k++)
		ret = ive_decimate(h, &p->level[k - 1], &p->level[k],
			(k + 1 == p->levels) ? instant : TD_FALSE);
	return ret;
}

/* ── per-size state ──────────────────────────────────────────────────── */

typedef struct {
	unsigned w, h;
	ot_svp_img frame;      /* stands in for the VPSS tap frame */
	IveBuf frame_buf;
	Pyramid prev, cur;
	ot_svp_img cand;       /* st_cand_corner eigen output, U8C1 */
	IveBuf cand_buf, cand_mem, corner_mem, resize_mem;
	IveBuf prev_pts, next_pts, status, err;
	ot_ive_st_corner_info *corners;
} Scene;

static int scene_alloc(Scene *s, unsigned w, unsigned h, td_u32 resize_mem)
{
	td_u32 stride = align16(w);

	memset(s, 0, sizeof(*s));
	s->w = w;
	s->h = h;
	if (ive_buf_alloc(&s->frame_buf, stride * h, "ivb_frame") != 0 ||
	    pyramid_alloc(&s->prev, w, h, "ivb_prev") != 0 ||
	    pyramid_alloc(&s->cur, w, h, "ivb_cur") != 0 ||
	    ive_buf_alloc(&s->cand_buf, stride * h, "ivb_cand") != 0 ||
	    /* Vendor rule for the candidate-corner scratch: 4 * stride * height
	     * plus the 16-byte max-eigenvalue record.  Its return code tells us
	     * whether CV610 kept it. */
	    ive_buf_alloc(&s->cand_mem, 4 * stride * h + sizeof(ot_ive_st_max_eig_val),
		    "ivb_cand_mem") != 0 ||
	    ive_buf_alloc(&s->corner_mem, align16(sizeof(ot_ive_st_corner_info)),
		    "ivb_corner") != 0 ||
	    ive_buf_alloc(&s->resize_mem, resize_mem, "ivb_resize_mem") != 0 ||
	    ive_buf_alloc(&s->prev_pts, OT_IVE_ST_MAX_CORNER_NUM * sizeof(ot_svp_point_s25q7),
		    "ivb_prev_pts") != 0 ||
	    ive_buf_alloc(&s->next_pts, OT_IVE_ST_MAX_CORNER_NUM * sizeof(ot_svp_point_s25q7),
		    "ivb_next_pts") != 0 ||
	    ive_buf_alloc(&s->status, OT_IVE_ST_MAX_CORNER_NUM, "ivb_status") != 0 ||
	    ive_buf_alloc(&s->err, OT_IVE_ST_MAX_CORNER_NUM * sizeof(td_u9q7), "ivb_err") != 0)
		return -1;
	img_set(&s->frame, &s->frame_buf, 0, w, h);
	img_set(&s->cand, &s->cand_buf, 0, w, h);
	s->corners = (ot_ive_st_corner_info *)s->corner_mem.virt;
	make_texture(img_pixels(&s->prev.level[0]), stride, w, h);
	shift_image(img_pixels(&s->frame), img_pixels(&s->prev.level[0]), stride, w, h,
		SHIFT_DX, SHIFT_DY);
	return 0;
}

static void scene_free(Scene *s)
{
	ive_buf_free(&s->frame_buf);
	ive_buf_free(&s->prev.buf);
	ive_buf_free(&s->cur.buf);
	ive_buf_free(&s->cand_buf);
	ive_buf_free(&s->cand_mem);
	ive_buf_free(&s->corner_mem);
	ive_buf_free(&s->resize_mem);
	ive_buf_free(&s->prev_pts);
	ive_buf_free(&s->next_pts);
	ive_buf_free(&s->status);
	ive_buf_free(&s->err);
}

static void print_head(const Scene *s, const char *op)
{
	printf("size=%ux%u op=%s", s->w, s->h, op);
}

/* ── benchmarks ──────────────────────────────────────────────────────── */

/* L0 copy and both pyramid builders on the current frame, each op waited on
 * individually so the figures are per operator. */
static void bench_pyramid(Scene *s, unsigned iters)
{
	Samples copy = { .n = 0 }, resize = { .n = 0 }, decim = { .n = 0 };
	td_s32 ret_copy = TD_SUCCESS, ret_resize = TD_SUCCESS, ret_decim = TD_SUCCESS;
	unsigned i;

	for (i = 0; i < iters; i++) {
		ot_ive_handle h;
		double w0 = now_us(), c0 = cpu_us();

		ret_copy = ive_finish(&copy, ive_copy(&h, &s->frame, &s->cur.level[0], TD_TRUE),
			h, w0, c0);
		if (ret_copy != TD_SUCCESS)
			break;
		if (s->cur.levels > 1) {
			w0 = now_us();
			c0 = cpu_us();
			ret_resize = ive_finish(&resize,
				ive_resize_levels(&h, &s->cur, &s->resize_mem, TD_TRUE), h, w0, c0);
			w0 = now_us();
			c0 = cpu_us();
			ret_decim = ive_finish(&decim, ive_decimate_levels(&h, &s->cur, TD_TRUE),
				h, w0, c0);
		}
	}
	print_head(s, "dma_copy_l0");
	print_tail(&copy, ret_copy);
	if (s->cur.levels > 1) {
		print_head(s, "resize_levels");
		printf(" levels=%u mem=%u", s->cur.levels - 1, s->resize_mem.size);
		print_tail(&resize, ret_resize);
		print_head(s, "dma_decimate_levels");
		printf(" levels=%u", s->cur.levels - 1);
		print_tail(&decim, ret_decim);
	}
}

/* Corners on the previous frame: the hardware eigenvalue pass and the CPU
 * selection pass timed apart, since only the latter costs the daemon CPU. */
static void bench_corners(Scene *s, unsigned iters, unsigned max_corners)
{
	Samples hw = { .n = 0 }, cpu = { .n = 0 };
	ot_ive_st_cand_corner_ctrl cand_ctrl;
	ot_ive_st_corner_ctrl corner_ctrl;
	ot_svp_dst_mem_info corner_out;
	td_s32 ret_hw = TD_SUCCESS, ret_cpu = TD_SUCCESS;
	unsigned i;

	memset(&cand_ctrl, 0, sizeof(cand_ctrl));
	mem_info_set(&cand_ctrl.mem, &s->cand_mem);
	cand_ctrl.quality_level = ST_QUALITY;
	corner_ctrl.max_corner_num = (td_u16)max_corners;
	corner_ctrl.min_dist = ST_MIN_DIST;
	mem_info_set(&corner_out, &s->corner_mem);

	for (i = 0; i < iters; i++) {
		ot_ive_handle h;
		double w0 = now_us(), c0 = cpu_us();

		ret_hw = ive_finish(&hw, ss_mpi_ive_st_cand_corner(&h, &s->prev.level[0], &s->cand,
			&cand_ctrl, TD_TRUE), h, w0, c0);
		if (ret_hw != TD_SUCCESS)
			break;
		w0 = now_us();
		c0 = cpu_us();
		ret_cpu = ss_mpi_ive_st_corner(&s->cand, &corner_out, &corner_ctrl);
		samples_add(&cpu, now_us() - w0, cpu_us() - c0);
		if (ret_cpu != TD_SUCCESS)
			break;
	}
	print_head(s, "st_cand_corner_hw");
	printf(" mem=%u", s->cand_mem.size);
	print_tail(&hw, ret_hw);
	print_head(s, "st_corner_cpu");
	printf(" max=%u found=%u", max_corners, s->corners->corner_num);
	print_tail(&cpu, ret_cpu);
}

/* prev_points for LK: the corners just found, topped up from a grid when the
 * texture gave fewer than asked (the count under test must be the count
 * submitted). */
static void fill_points(Scene *s, unsigned n)
{
	ot_svp_point_s25q7 *pts = (ot_svp_point_s25q7 *)s->prev_pts.virt;
	unsigned found = s->corners->corner_num, i;
	unsigned margin = 16, cols = 10;

	for (i = 0; i < n && i < found; i++) {
		pts[i].x = (td_s25q7)(s->corners->corner[i].x << 7);
		pts[i].y = (td_s25q7)(s->corners->corner[i].y << 7);
	}
	for (; i < n; i++) {
		unsigned gx = margin + (i % cols) * (s->w - 2 * margin) / cols;
		unsigned gy = margin + (i / cols) * (s->h - 2 * margin) / ((n + cols - 1) / cols);

		pts[i].x = (td_s25q7)(gx << 7);
		pts[i].y = (td_s25q7)(gy << 7);
	}
}

typedef struct {
	unsigned status_one;
	unsigned status_zero;
	unsigned status_other;
	double med_err_px;
} LkCheck;

/* Flow = next - prev against the known shift, over the points the hardware
 * flagged.  Both status polarities are counted so the run itself shows which
 * value means "tracked" on this SoC. */
static void lk_check(const Scene *s, unsigned n, LkCheck *out)
{
	const ot_svp_point_s25q7 *p = (const ot_svp_point_s25q7 *)s->prev_pts.virt;
	const ot_svp_point_s25q7 *q = (const ot_svp_point_s25q7 *)s->next_pts.virt;
	const td_u8 *st = (const td_u8 *)s->status.virt;
	double *errs = malloc(n * sizeof(*errs));
	unsigned i, m = 0;

	memset(out, 0, sizeof(*out));
	if (!errs)
		return;
	for (i = 0; i < n; i++) {
		double dx, dy;

		if (st[i] == 1)
			out->status_one++;
		else if (st[i] == 0)
			out->status_zero++;
		else
			out->status_other++;
		if (st[i] != 1)
			continue;
		dx = (double)(q[i].x - p[i].x) / 128.0 - SHIFT_DX;
		dy = (double)(q[i].y - p[i].y) / 128.0 - SHIFT_DY;
		errs[m++] = sqrt(dx * dx + dy * dy);
	}
	if (m) {
		qsort(errs, m, sizeof(double), cmp_double);
		out->med_err_px = errs[m / 2];
	} else {
		out->med_err_px = -1.0;
	}
	free(errs);
}

static td_s32 lk_submit(ot_ive_handle *h, Scene *s, unsigned n, unsigned max_level,
	td_bool instant)
{
	ot_ive_lk_optical_flow_pyr_ctrl ctrl;
	ot_svp_src_mem_info prev_pts, status, err;
	ot_svp_mem_info next_pts;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.out_mode = OT_IVE_LK_OPTICAL_FLOW_PYR_OUT_MODE_BOTH;
	ctrl.points_num = (td_u16)n;
	ctrl.max_level = (td_u8)max_level;
	ctrl.min_eig_val_threshold = LK_MIN_EIG_THR;
	ctrl.iter_cnt = LK_ITER_CNT;
	ctrl.eps = LK_EPS;
	mem_info_set(&prev_pts, &s->prev_pts);
	mem_info_set(&next_pts, &s->next_pts);
	mem_info_set(&status, &s->status);
	mem_info_set(&err, &s->err);
	prev_pts.size = next_pts.size = n * sizeof(ot_svp_point_s25q7);
	status.size = n;
	err.size = n * sizeof(td_u9q7);
	memset(s->next_pts.virt, 0, next_pts.size);
	memset(s->status.virt, 0xee, OT_IVE_ST_MAX_CORNER_NUM);
	return ss_mpi_ive_lk_optical_flow_pyr(h, s->prev.level, s->cur.level, &prev_pts,
		&next_pts, &status, &err, &ctrl, instant);
}

/* LK alone, pyramids already built, for every point count x max_level the
 * size allows.  Levels above the legal maximum are tried once on purpose so
 * the refusal code is on record. */
static void bench_lk(Scene *s, const Options *o)
{
	unsigned pi, lvl;

	for (pi = 0; pi < o->point_count; pi++) {
		unsigned n = o->points[pi];

		fill_points(s, n);
		for (lvl = 0; lvl < MAX_LEVELS; lvl++) {
			Samples smp = { .n = 0 };
			LkCheck chk;
			td_s32 ret = TD_SUCCESS;
			unsigned iters = (lvl < s->cur.levels) ? o->iters : 1, i;

			for (i = 0; i < iters; i++) {
				ot_ive_handle h;
				double w0 = now_us(), c0 = cpu_us();

				ret = ive_finish(&smp, lk_submit(&h, s, n, lvl, TD_TRUE), h, w0, c0);
				if (ret != TD_SUCCESS)
					break;
			}
			lk_check(s, n, &chk);
			print_head(s, "lk_pyr");
			printf(" points=%u max_level=%u%s status1=%u status0=%u other=%u med_err_px=%.2f",
				n, lvl, lvl < s->cur.levels ? "" : " (above-size-limit)",
				chk.status_one, chk.status_zero, chk.status_other, chk.med_err_px);
			print_tail(&smp, ret);
		}
	}
}

/* The port's whole per-solve submission as one chain: L0 copy, pyramid,
 * LK, next frame's candidate corners, one blocking query at the end, then
 * the CPU corner pick.  This is the number the go/no-go rule reads. */
static void bench_chain(Scene *s, const Options *o, unsigned n, int use_resize)
{
	Samples smp = { .n = 0 };
	ot_ive_st_cand_corner_ctrl cand_ctrl;
	ot_ive_st_corner_ctrl corner_ctrl;
	ot_svp_dst_mem_info corner_out;
	LkCheck chk;
	td_s32 ret = TD_SUCCESS;
	unsigned max_level = s->cur.levels - 1, i;

	memset(&cand_ctrl, 0, sizeof(cand_ctrl));
	mem_info_set(&cand_ctrl.mem, &s->cand_mem);
	cand_ctrl.quality_level = ST_QUALITY;
	corner_ctrl.max_corner_num = (td_u16)n;
	corner_ctrl.min_dist = ST_MIN_DIST;
	mem_info_set(&corner_out, &s->corner_mem);
	fill_points(s, n);

	for (i = 0; i < o->iters; i++) {
		ot_ive_handle h;
		double w0 = now_us(), c0 = cpu_us();

		ret = ive_copy(&h, &s->frame, &s->cur.level[0], TD_FALSE);
		if (ret == TD_SUCCESS && s->cur.levels > 1)
			ret = use_resize ? ive_resize_levels(&h, &s->cur, &s->resize_mem, TD_FALSE)
					 : ive_decimate_levels(&h, &s->cur, TD_FALSE);
		if (ret == TD_SUCCESS)
			ret = lk_submit(&h, s, n, max_level, TD_FALSE);
		if (ret == TD_SUCCESS)
			ret = ss_mpi_ive_st_cand_corner(&h, &s->cur.level[0], &s->cand, &cand_ctrl,
				TD_TRUE);
		if (ret == TD_SUCCESS)
			ret = ive_wait(h);
		if (ret == TD_SUCCESS)
			ret = ss_mpi_ive_st_corner(&s->cand, &corner_out, &corner_ctrl);
		samples_add(&smp, now_us() - w0, cpu_us() - c0);
		if (ret != TD_SUCCESS)
			break;
	}
	lk_check(s, n, &chk);
	print_head(s, use_resize ? "chain_resize" : "chain_decimate");
	printf(" points=%u max_level=%u status1=%u med_err_px=%.2f corners_next=%u", n,
		max_level, chk.status_one, chk.med_err_px, s->corners->corner_num);
	print_tail(&smp, ret);
}

static void bench_size(const Options *o, unsigned w, unsigned h)
{
	Scene s;
	ot_ive_handle hdl;
	td_s32 ret;
	unsigned pi;

	if (w > MAX_L0_WIDTH || h > MAX_L0_HEIGHT || w < MIN_LEVEL_SIDE || h < MIN_LEVEL_SIDE) {
		printf("size=%ux%u skipped: level 0 must be %ux%u..%ux%u\n", w, h,
			MIN_LEVEL_SIDE, MIN_LEVEL_SIDE, MAX_L0_WIDTH, MAX_L0_HEIGHT);
		return;
	}
	if (scene_alloc(&s, w, h, o->resize_mem) != 0) {
		scene_free(&s);
		return;
	}
	printf("size=%ux%u levels=%u (max_level=%u) shift=%d,%d\n", w, h, s.cur.levels,
		s.cur.levels - 1, SHIFT_DX, SHIFT_DY);

	/* The previous frame's pyramid is built once, before timing, with the
	 * decimation path; the LK rows then measure LK alone. */
	ret = ive_decimate_levels(&hdl, &s.prev, TD_TRUE);
	if (ret == TD_SUCCESS)
		ret = ive_wait(hdl);
	if (ret != TD_SUCCESS)
		printf("size=%ux%u prev pyramid build failed: 0x%x\n", w, h, (unsigned)ret);

	bench_pyramid(&s, o->iters);
	bench_corners(&s, o->iters, OT_IVE_ST_MAX_CORNER_NUM);
	bench_lk(&s, o);
	for (pi = 0; pi < o->point_count; pi++) {
		bench_chain(&s, o, o->points[pi], 0);
		bench_chain(&s, o, o->points[pi], 1);
	}
	scene_free(&s);
}

/* ── MPP bring-up ────────────────────────────────────────────────────── */

/* The daemon's sys_setup() discipline in miniature: pre-clean whatever a
 * crashed process left, then VB (one token pool — IVE itself needs none, but
 * SYS init insists on a VB config) and SYS. */
static int mpp_init(void)
{
	ot_vb_cfg vb;
	td_s32 ret;

	(void)ss_mpi_sys_exit();
	(void)ss_mpi_vb_exit();
	memset(&vb, 0, sizeof(vb));
	vb.max_pool_cnt = 1;
	vb.common_pool[0].blk_size = 64 * 1024;
	vb.common_pool[0].blk_cnt = 1;
	ret = ss_mpi_vb_set_cfg(&vb);
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "vb_set_cfg failed: 0x%x (is waybeam still running?)\n", ret);
		return -1;
	}
	ret = ss_mpi_vb_init();
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "vb_init failed: 0x%x\n", ret);
		return -1;
	}
	ret = ss_mpi_sys_init();
	if (ret != TD_SUCCESS) {
		fprintf(stderr, "sys_init failed: 0x%x\n", ret);
		(void)ss_mpi_vb_exit();
		return -1;
	}
	return 0;
}

static void mpp_exit(void)
{
	(void)ss_mpi_sys_exit();
	(void)ss_mpi_vb_exit();
}

/* ── options ─────────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--iters N] [--sizes WxH,...] [--points N,...] "
		"[--resize-mem BYTES] [--no-sys]\n"
		"  --iters N          runs per measurement (default 50, max %d)\n"
		"  --sizes WxH,...    level-0 sizes (default 320x180,640x360,1280x720)\n"
		"  --points N,...     LK point counts (default 50,100,500; max %d)\n"
		"  --resize-mem BYTES ss_mpi_ive_resize aux buffer (default 65536)\n"
		"  --no-sys           skip VB/SYS init (try alongside a running waybeam)\n",
		argv0, MAX_ITERS, OT_IVE_ST_MAX_CORNER_NUM);
}

static int parse_sizes(Options *o, const char *arg)
{
	char *copy = strdup(arg), *tok, *save = NULL;

	o->size_count = 0;
	for (tok = strtok_r(copy, ",", &save); tok && o->size_count < MAX_SIZES;
	     tok = strtok_r(NULL, ",", &save)) {
		Size *s = &o->sizes[o->size_count];

		if (sscanf(tok, "%ux%u", &s->w, &s->h) != 2) {
			free(copy);
			return -1;
		}
		o->size_count++;
	}
	free(copy);
	return o->size_count ? 0 : -1;
}

static int parse_points(Options *o, const char *arg)
{
	char *copy = strdup(arg), *tok, *save = NULL;

	o->point_count = 0;
	for (tok = strtok_r(copy, ",", &save); tok && o->point_count < MAX_POINT_SETS;
	     tok = strtok_r(NULL, ",", &save)) {
		unsigned n = (unsigned)strtoul(tok, NULL, 10);

		if (n == 0 || n > OT_IVE_ST_MAX_CORNER_NUM) {
			free(copy);
			return -1;
		}
		o->points[o->point_count++] = n;
	}
	free(copy);
	return o->point_count ? 0 : -1;
}

static int parse_options(Options *o, int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "iters", required_argument, NULL, 'i' },
		{ "sizes", required_argument, NULL, 's' },
		{ "points", required_argument, NULL, 'p' },
		{ "resize-mem", required_argument, NULL, 'r' },
		{ "no-sys", no_argument, NULL, 'n' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	int c;

	memset(o, 0, sizeof(*o));
	o->iters = 50;
	o->size_count = 3;
	o->sizes[0] = (Size){ 320, 180 };
	o->sizes[1] = (Size){ 640, 360 };
	o->sizes[2] = (Size){ 1280, 720 };
	o->point_count = 3;
	o->points[0] = 50;
	o->points[1] = 100;
	o->points[2] = 500;
	o->resize_mem = 65536;

	while ((c = getopt_long(argc, argv, "i:s:p:r:nh", longopts, NULL)) != -1) {
		switch (c) {
		case 'i':
			o->iters = (unsigned)strtoul(optarg, NULL, 10);
			if (o->iters == 0 || o->iters > MAX_ITERS)
				return -1;
			break;
		case 's':
			if (parse_sizes(o, optarg) != 0)
				return -1;
			break;
		case 'p':
			if (parse_points(o, optarg) != 0)
				return -1;
			break;
		case 'r':
			o->resize_mem = (td_u32)strtoul(optarg, NULL, 10);
			break;
		case 'n':
			o->no_sys = 1;
			break;
		default:
			return -1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	Options o;
	unsigned i;

	if (parse_options(&o, argc, argv) != 0) {
		usage(argv[0]);
		return 2;
	}
	if (!o.no_sys && mpp_init() != 0)
		return 1;
	printf("cv610_ive_bench iters=%u lk{min_eig=%d iter=%d eps=%d} "
		"st{quality=%d min_dist=%d}\n", o.iters, LK_MIN_EIG_THR, LK_ITER_CNT, LK_EPS,
		ST_QUALITY, ST_MIN_DIST);
	for (i = 0; i < o.size_count; i++)
		bench_size(&o, o.sizes[i].w, o.sizes[i].h);
	if (!o.no_sys)
		mpp_exit();
	return 0;
}
