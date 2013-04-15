/*
 * Copyright (c) 2003 Michael Niedermayer
 * Copyright (c) 2012 Jeremy Tran
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 * Apply a hue/saturation filter to the input video
 * Ported from MPlayer libmpcodecs/vf_hue.c.
 */

#include <float.h>
#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define SAT_MIN_VAL -10
#define SAT_MAX_VAL 10

static const char *const var_names[] = {
    "n",   // frame count
    "pts", // presentation timestamp expressed in AV_TIME_BASE units
    "r",   // frame rate
    "t",   // timestamp expressed in seconds
    "tb",  // timebase
    NULL
};

enum var_name {
    VAR_N,
    VAR_PTS,
    VAR_R,
    VAR_T,
    VAR_TB,
    VAR_NB
};

typedef struct {
    const    AVClass *class;
    float    hue_deg; /* hue expressed in degrees */
    float    hue; /* hue expressed in radians */
    char     *hue_deg_expr;
    char     *hue_expr;
    AVExpr   *hue_deg_pexpr;
    AVExpr   *hue_pexpr;
    float    saturation;
    char     *saturation_expr;
    AVExpr   *saturation_pexpr;
    int      hsub;
    int      vsub;
    int32_t hue_sin;
    int32_t hue_cos;
    double   var_values[VAR_NB];
} HueContext;

#define OFFSET(x) offsetof(HueContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
static const AVOption hue_options[] = {
    { "h", "set the hue angle degrees expression", OFFSET(hue_deg_expr), AV_OPT_TYPE_STRING,
      { .str = NULL }, .flags = FLAGS },
    { "s", "set the saturation expression", OFFSET(saturation_expr), AV_OPT_TYPE_STRING,
      { .str = "1" }, .flags = FLAGS },
    { "H", "set the hue angle radians expression", OFFSET(hue_expr), AV_OPT_TYPE_STRING,
      { .str = NULL }, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(hue);

static inline void compute_sin_and_cos(HueContext *hue)
{
    /*
     * Scale the value to the norm of the resulting (U,V) vector, that is
     * the saturation.
     * This will be useful in the process_chrominance function.
     */
    hue->hue_sin = rint(sin(hue->hue) * (1 << 16) * hue->saturation);
    hue->hue_cos = rint(cos(hue->hue) * (1 << 16) * hue->saturation);
}

static int set_expr(AVExpr **pexpr, const char *expr, const char *option, void *log_ctx)
{
    int ret;
    AVExpr *old = NULL;

    if (*pexpr)
        old = *pexpr;
    ret = av_expr_parse(pexpr, expr, var_names,
                        NULL, NULL, NULL, NULL, 0, log_ctx);
    if (ret < 0) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Error when evaluating the expression '%s' for %s\n",
               expr, option);
        *pexpr = old;
        return ret;
    }
    av_expr_free(old);
    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    HueContext *hue = ctx->priv;
    int ret;

    if (hue->hue_expr && hue->hue_deg_expr) {
        av_log(ctx, AV_LOG_ERROR,
               "H and h options are incompatible and cannot be specified "
               "at the same time\n");
        return AVERROR(EINVAL);
    }

#define SET_EXPR(expr, option)                                          \
    if (hue->expr##_expr) do {                                          \
        ret = set_expr(&hue->expr##_pexpr, hue->expr##_expr, option, ctx); \
        if (ret < 0)                                                    \
            return ret;                                                 \
    } while (0)
    SET_EXPR(saturation, "s");
    SET_EXPR(hue_deg,    "h");
    SET_EXPR(hue,        "H");

    av_log(ctx, AV_LOG_VERBOSE,
           "H_expr:%s h_deg_expr:%s s_expr:%s\n",
           hue->hue_expr, hue->hue_deg_expr, hue->saturation_expr);
    compute_sin_and_cos(hue);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    HueContext *hue = ctx->priv;

    av_expr_free(hue->hue_deg_pexpr);
    av_expr_free(hue->hue_pexpr);
    av_expr_free(hue->saturation_pexpr);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV444P,      AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV420P,      AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV410P,      AV_PIX_FMT_YUV440P,
        AV_PIX_FMT_YUVA444P,     AV_PIX_FMT_YUVA422P,
        AV_PIX_FMT_YUVA420P,
        AV_PIX_FMT_NONE
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));

    return 0;
}

static int config_props(AVFilterLink *inlink)
{
    HueContext *hue = inlink->dst->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    hue->hsub = desc->log2_chroma_w;
    hue->vsub = desc->log2_chroma_h;

    hue->var_values[VAR_N]  = 0;
    hue->var_values[VAR_TB] = av_q2d(inlink->time_base);
    hue->var_values[VAR_R]  = inlink->frame_rate.num == 0 || inlink->frame_rate.den == 0 ?
        NAN : av_q2d(inlink->frame_rate);

    return 0;
}

static void process_chrominance(uint8_t *udst, uint8_t *vdst, const int dst_linesize,
                                uint8_t *usrc, uint8_t *vsrc, const int src_linesize,
                                int w, int h,
                                const int32_t c, const int32_t s)
{
    int32_t u, v, new_u, new_v;
    int i;

    /*
     * If we consider U and V as the components of a 2D vector then its angle
     * is the hue and the norm is the saturation
     */
    while (h--) {
        for (i = 0; i < w; i++) {
            /* Normalize the components from range [16;140] to [-112;112] */
            u = usrc[i] - 128;
            v = vsrc[i] - 128;
            /*
             * Apply the rotation of the vector : (c * u) - (s * v)
             *                                    (s * u) + (c * v)
             * De-normalize the components (without forgetting to scale 128
             * by << 16)
             * Finally scale back the result by >> 16
             */
            new_u = ((c * u) - (s * v) + (1 << 15) + (128 << 16)) >> 16;
            new_v = ((s * u) + (c * v) + (1 << 15) + (128 << 16)) >> 16;

            /* Prevent a potential overflow */
            udst[i] = av_clip_uint8_c(new_u);
            vdst[i] = av_clip_uint8_c(new_v);
        }

        usrc += src_linesize;
        vsrc += src_linesize;
        udst += dst_linesize;
        vdst += dst_linesize;
    }
}

#define TS2D(ts) ((ts) == AV_NOPTS_VALUE ? NAN : (double)(ts))
#define TS2T(ts, tb) ((ts) == AV_NOPTS_VALUE ? NAN : (double)(ts) * av_q2d(tb))

static int filter_frame(AVFilterLink *inlink, AVFrame *inpic)
{
    HueContext *hue = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFrame *outpic;
    int direct = 0;

    if (av_frame_is_writable(inpic)) {
        direct = 1;
        outpic = inpic;
    } else {
        outpic = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!outpic) {
            av_frame_free(&inpic);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(outpic, inpic);
    }

    hue->var_values[VAR_N]   = inlink->frame_count;
    hue->var_values[VAR_T]   = TS2T(inpic->pts, inlink->time_base);
    hue->var_values[VAR_PTS] = TS2D(inpic->pts);

    if (hue->saturation_expr) {
        hue->saturation = av_expr_eval(hue->saturation_pexpr, hue->var_values, NULL);

        if (hue->saturation < SAT_MIN_VAL || hue->saturation > SAT_MAX_VAL) {
            hue->saturation = av_clip(hue->saturation, SAT_MIN_VAL, SAT_MAX_VAL);
            av_log(inlink->dst, AV_LOG_WARNING,
                   "Saturation value not in range [%d,%d]: clipping value to %0.1f\n",
                   SAT_MIN_VAL, SAT_MAX_VAL, hue->saturation);
        }
    }

    if (hue->hue_deg_expr) {
        hue->hue_deg = av_expr_eval(hue->hue_deg_pexpr, hue->var_values, NULL);
        hue->hue = hue->hue_deg * M_PI / 180;
    } else if (hue->hue_expr) {
        hue->hue = av_expr_eval(hue->hue_pexpr, hue->var_values, NULL);
        hue->hue_deg = hue->hue * 180 / M_PI;
    }

    av_log(inlink->dst, AV_LOG_DEBUG,
           "H:%0.1f*PI h:%0.1f s:%0.f t:%0.1f n:%d\n",
           hue->hue/M_PI, hue->hue_deg, hue->saturation,
           hue->var_values[VAR_T], (int)hue->var_values[VAR_N]);

    compute_sin_and_cos(hue);

    if (!direct) {
        av_image_copy_plane(outpic->data[0], outpic->linesize[0],
                            inpic->data[0],  inpic->linesize[0],
                            inlink->w, inlink->h);
        if (inpic->data[3])
            av_image_copy_plane(outpic->data[3], outpic->linesize[3],
                                inpic->data[3],  inpic->linesize[3],
                                inlink->w, inlink->h);
    }

    process_chrominance(outpic->data[1], outpic->data[2], outpic->linesize[1],
                        inpic->data[1],  inpic->data[2],  inpic->linesize[1],
                        inlink->w >> hue->hsub, inlink->h >> hue->vsub,
                        hue->hue_cos, hue->hue_sin);

    if (!direct)
        av_frame_free(&inpic);
    return ff_filter_frame(outlink, outpic);
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    HueContext *hue = ctx->priv;

#define SET_CMD(expr, option) do {                                 \
    if (!strcmp(cmd, option))                                      \
        return set_expr(&hue->expr##_pexpr, args, cmd, ctx);       \
} while (0)
    SET_CMD(hue_deg,    "h");
    SET_CMD(hue,        "H");
    SET_CMD(saturation, "s");

    return AVERROR(ENOSYS);
}

static const AVFilterPad hue_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_props,
    },
    { NULL }
};

static const AVFilterPad hue_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter avfilter_vf_hue = {
    .name        = "hue",
    .description = NULL_IF_CONFIG_SMALL("Adjust the hue and saturation of the input video."),

    .priv_size = sizeof(HueContext),

    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .process_command = process_command,
    .inputs          = hue_inputs,
    .outputs         = hue_outputs,
    .priv_class      = &hue_class,
};
