/*
 * Copyright (c) 2024 Josse De Oliveira
 *
 * This file is part of a patch of FFmpeg.
 *
 * This patch is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * The vsrc_mandelbrot filter from Michael Niedermayer was used as template to create
 * this
 */

/**
 * @file
 * WMS renderer
 */
#include <math.h>
#include "libavutil/eval.h"
#include "libavutil/thread.h"

#include "avfilter.h"
#include "internal.h"

#include "lavfutils.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"

#define SQR(a) ((a)*(a))

typedef struct WMSContext {
    const AVClass *class;
    int w, h;
    char *xref_expr, *yref_expr;
    char *x1_expr,*x2_expr,*y1_expr,*y2_expr;
    AVRational frame_rate;
	uint64_t pts;
    double end_pts;
} WMSContext;

typedef struct {
    double x1, y1, x2, y2;
} MapReadContext;

#define OFFSET(x) offsetof(WMSContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption wms_options[] = {
    {"size",        "set frame size",                           OFFSET(w),       AV_OPT_TYPE_IMAGE_SIZE, {.str="640x480"},  0, 0, FLAGS },
    {"s",           "set frame size",                           OFFSET(w),       AV_OPT_TYPE_IMAGE_SIZE, {.str="640x480"},  0, 0, FLAGS },
    {"rate",        "set frame rate",                           OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, {.str="25"},  0, INT_MAX, FLAGS },
    {"r",           "set frame rate",                           OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, {.str="25"},  0, INT_MAX, FLAGS },
    {"end_pts",     "set the terminal pts value",               OFFSET(end_pts), AV_OPT_TYPE_DOUBLE,     {.dbl=400},  0, INT64_MAX, FLAGS },
    {"xref",        "set a x coord you can use as reference",   OFFSET(xref_expr), AV_OPT_TYPE_STRING,     {.str="0"},  0, 0, FLAGS },
    {"yref",        "set a y coord you can use as reference",   OFFSET(yref_expr), AV_OPT_TYPE_STRING,     {.str="0"},  0, 0, FLAGS },
    {"x1",          "set bbox west coords",                     OFFSET(x1_expr), AV_OPT_TYPE_STRING,     {.str="-180"},  0, 0, FLAGS },
    {"x2",          "set bbox east coords",                     OFFSET(x2_expr), AV_OPT_TYPE_STRING,     {.str="180"},  0, 0, FLAGS },
    {"y1",          "set bbox north coords",                    OFFSET(y1_expr), AV_OPT_TYPE_STRING,     {.str="-90"},  0, 0, FLAGS },
    {"y2",          "set bbox south coords",                      OFFSET(y2_expr), AV_OPT_TYPE_STRING,     {.str="90"},  0, 0, FLAGS },
{NULL},
};

AVFILTER_DEFINE_CLASS(wms);

static av_cold int init(AVFilterContext *ctx)
{
    //WMSContext *s = ctx->priv;
	//do nothing
	return 0;
}

static av_cold void uninit(AVFilterContext *ctx){
    //WMSContext *s = ctx->priv;
	//do nothing
}




static const char *const var_names[] = {
    "xref", "yref", //reference
    "x1","x2","y1","y2", //bbox
    "t", //time
    NULL
};

enum var_name {
    VAR_XREF,
    VAR_YREF,
    VAR_X1,
    VAR_X2,
    VAR_Y1,
    VAR_Y2,
    VAR_T,
    VARS_NB
};

static int parse_expressions(MapReadContext *mapctx, AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    WMSContext *s = ctx->priv;
    int ret;

    double var_values[VARS_NB], res;
    char *expr;

    var_values[VAR_XREF] = NAN;
    var_values[VAR_YREF] = NAN;
    var_values[VAR_X1] = NAN;
    var_values[VAR_X2] = NAN;
    var_values[VAR_Y1] = NAN;
    var_values[VAR_Y2] = NAN;
    var_values[VAR_T] = s->pts * av_q2d(outlink->time_base);


    if ((ret = av_expr_parse_and_eval(&res, (expr = s->xref_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    var_values[VAR_XREF] = res;

    if ((ret = av_expr_parse_and_eval(&res, (expr = s->yref_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    var_values[VAR_YREF] = res;


    if ((ret = av_expr_parse_and_eval(&res, (expr = s->x1_expr),
                                              var_names, var_values,
                                              NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    mapctx->x1 = var_values[VAR_X1] = res;

    if ((ret = av_expr_parse_and_eval(&res, (expr = s->x2_expr),
                                              var_names, var_values,
                                              NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    mapctx->x2 = var_values[VAR_X2] = res;


    if ((ret = av_expr_parse_and_eval(&res, (expr = s->y1_expr),
                                              var_names, var_values,
                                              NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    mapctx->y1 = var_values[VAR_Y1] = res;

    if ((ret = av_expr_parse_and_eval(&res, (expr = s->y2_expr),
                                              var_names, var_values,
                                              NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    mapctx->y2 = var_values[VAR_Y2] = res;

    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'.\n",
           expr);
    return ret;
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    WMSContext *s = ctx->priv;

    if (av_image_check_size(s->w, s->h, 0, ctx) < 0)
        return AVERROR(EINVAL);

    outlink->w = s->w;
    outlink->h = s->h;
    outlink->time_base = av_inv_q(s->frame_rate);
    outlink->frame_rate = s->frame_rate;

    return 0;
}


static int get_frame(AVFrame *dst, AVFilterContext *ctx, const char* url) {

    int ret;

    if ((ret = ff_load_image(dst->data, dst->linesize,
                            &dst->width, &dst->height,
                            &dst->format, url, ctx)) < 0)
        return ret;
    return 0;
}


#define SERVICE_URL "https://ows.terrestris.de/osm/service?service=WMS&version=1.1.1&request=GetMap&layers=OSM-WMS&styles=&format=image%%2Fpng&bbox=%lf%%2C%lf%%2C%lf%%2C%lf&width=%d&height=%d&srs=EPSG%%3A4326&transparent=true"



static AVMutex pts_mutex = AV_MUTEX_INITIALIZER;

static int request_frame(AVFilterLink *link)
{
    int ret;
    char *url;
    AVFrame *picref;
    AVFilterContext *ctx = link->dst;
    WMSContext *s = link->src->priv;
    MapReadContext mctx = {0,0,0,0};

    if((ret =parse_expressions(&mctx, link) < 0))
        return ret;


    picref = av_frame_alloc();
    if (!picref)
        return AVERROR(ENOMEM);

    url = av_asprintf(SERVICE_URL,
        mctx.x1, mctx.y1, mctx.x2, mctx.y2,
        s->w, s->h);

    if ((ret = get_frame(picref, ctx, url)))
        return ret;

    picref->duration = 1;
    ff_mutex_lock(&pts_mutex);
    picref->pts = s->pts++;
    av_log(s, AV_LOG_INFO, "Draw from pts: %ld [(%lf %lf), (%lf %lf)]\r\n", s->pts, mctx.x1, mctx.y1, mctx.x2, mctx.y2);
    av_log(s, AV_LOG_INFO, "Used url: %s\r\n", url);
    ff_mutex_unlock(&pts_mutex);
    av_free(url);

    return ff_filter_frame(link, picref);
}

static const AVFilterPad wms_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = request_frame,
        .config_props  = config_props,
    },
};

const AVFilter ff_vsrc_wms = {
    .name          = "wms",
    .description   = NULL_IF_CONFIG_SMALL("Render a basemap from a wms."),
    .priv_size     = sizeof(WMSContext),
    .priv_class    = &wms_class,
    .init          = init,
    .uninit        = uninit,
    .inputs        = NULL,
    FILTER_OUTPUTS(wms_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_0BGR32),
};
