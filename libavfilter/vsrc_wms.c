/*
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

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xmlschemastypes.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlstring.h>
#include <libxml/globals.h>

#include "avfilter.h"
#include "internal.h"
#include "lavfutils.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/bprint.h"
#include "libavutil/eval.h"
#include "libavutil/thread.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"

#define SQR(a) ((a)*(a))


enum WMSVersion { WMS_V1_0_0, WMS_V1_1_0, WMS_V1_1_1, WMS_V1_3_0 };

typedef struct WMSContext {
    const AVClass *class;
    int w, h;
    char *xref_expr, *yref_expr;
    char *x1_expr,*x2_expr,*y1_expr,*y2_expr;
    AVRational frame_rate;
	uint64_t pts;
    double end_pts;
    char *capabilities_url;
    char *url;
    char *layers;
    char *version;
    char *service;
    char *fmt_url;
    enum WMSVersion wms_version;
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
    {"y2",          "set bbox south coords",                    OFFSET(y2_expr), AV_OPT_TYPE_STRING,     {.str="90"},  0, 0, FLAGS },
    {"y2",          "set bbox south coords",                    OFFSET(y2_expr), AV_OPT_TYPE_STRING,     {.str="90"},  0, 0, FLAGS },
    {"url",         "set service URL without parameters",       OFFSET(capabilities_url), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS},
    {"layers",      "set layers parameter for WMS",             OFFSET(layers), AV_OPT_TYPE_STRING, {.str=""}, 0, 0, FLAGS},
    {NULL},
};

AVFILTER_DEFINE_CLASS(wms);

static xmlNodePtr find_child_xml(xmlNodePtr node,const char* name) {
    xmlNodePtr search;
    if(node == NULL) return NULL;
    for(search = node->children;
        search && xmlStrcasecmp(search->name, (xmlChar *)name) != 0;
        search = search->next) {
    }
    return search;
}

static int parse_xml(const xmlDocPtr doc, AVFilterContext *ctx) {
    WMSContext *s = ctx->priv;
    xmlNodePtr nodeptr, serviceptr, getmapptr;
    xmlChar *version, *url;

    nodeptr = xmlDocGetRootElement(doc);
    if (nodeptr == NULL) {
        av_log(ctx, AV_LOG_ERROR, "Error reading XML root\n");
        return AVERROR(EIO);
    }

    version = xmlGetProp(nodeptr, (const xmlChar *)"version");
    if(version == NULL) {
        av_log(ctx, AV_LOG_ERROR, "Could not read version\n");
        return AVERROR(EINVAL);
    }
    s->version = (char *)xmlStrdup(version);
    xmlFree(version);

    serviceptr = find_child_xml(nodeptr, "Service");
    if(serviceptr == NULL) {
        av_log(ctx, AV_LOG_ERROR, "Could not find Service node in GetCapabilities XML\n");
        return AVERROR(EINVAL);
    }

    nodeptr = find_child_xml(nodeptr, "Capability");
    nodeptr = find_child_xml(nodeptr, "Request");
    getmapptr = nodeptr = find_child_xml(nodeptr, "GetMap");
    if(getmapptr == NULL) {
        av_log(ctx, AV_LOG_ERROR, "Could not find GetMap node in GetCapabilities XML\n");
        return AVERROR(EINVAL);
    }

    nodeptr = find_child_xml(serviceptr, "Name");
    if(nodeptr == NULL || nodeptr->children == NULL || nodeptr->children->content == NULL) {
        av_log(ctx, AV_LOG_WARNING, "Could not read service name, using 'WMS'\n");
        s->service = strdup("WMS");
    }else {
        s->service = (char *)xmlStrdup(serviceptr->children->content);
    }

    nodeptr = find_child_xml(getmapptr, "DCPType");
    nodeptr = find_child_xml(nodeptr, "HTTP");
    nodeptr = find_child_xml(nodeptr, "Get");
    nodeptr = find_child_xml(nodeptr, "OnlineResource");
    if(nodeptr == NULL) {
        av_log(ctx, AV_LOG_ERROR, "Could not read OnlineResource node\n");
        return AVERROR(EINVAL);
    }
    url = xmlGetNsProp(nodeptr, "href", "xlink");
    if(url == NULL) {
        av_log(ctx, AV_LOG_WARNING, "Could not read URL property for GetMap, using the same as GetCapabilities\n");
        s->url = strdup(s->capabilities_url);
    }else {
        s->url = (char *)xmlStrdup(url);
        xmlFree(url);
    }
    return 0;
}

static char* prepare_capabilities_url(char *opt_capurl) {
    char tmp, *lastchr, *url;
    //Clean URL: remove # and ?
    #define IS_CLEANURL(ch) ch!=0 & ch!='?' && ch!='#'
    for(lastchr= opt_capurl; IS_CLEANURL(*lastchr); lastchr++){}
    tmp = *lastchr;
    *lastchr = 0;
    url = av_asprintf("%s?request=GetCapabilities", opt_capurl);
    *lastchr = tmp;
    return url;
}

static int read_xml(AVFilterContext *ctx) {
    WMSContext *s = ctx->priv;
    AVIOContext *io_ctx = NULL;
    int ret;
    struct AVBPrint buf;
    xmlDocPtr doc = NULL;
    char *url = prepare_capabilities_url(s->capabilities_url);
    ret = avio_open2(&io_ctx, url, AVIO_FLAG_READ, NULL, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error opening GetCapabilities URL: %s\n", av_err2str(ret));
        goto end;
    }

    // Must read all the XML to parse it
    av_bprint_init(&buf, 0, INT_MAX);
    avio_read_to_bprint(io_ctx, &buf, INT_MAX);


    doc = xmlReadMemory(buf.str, buf.len, url, NULL, 0);
    if (doc == NULL) {
        av_log(ctx, AV_LOG_ERROR, "Error reading XML file\n");
        ret = EIO;
    }

    ret = parse_xml(doc, ctx);
end:
    avio_close(io_ctx);
    av_bprint_finalize(&buf, NULL);
    av_free(url);
    xmlFreeDoc(doc);
    xmlCleanupParser();
    return ret;
}

static int parse_getcapabilities(AVFilterContext *ctx) {
    return read_xml(ctx);
}

#define WMS_REQARG_SERVICE "service=%s"
#define WMS_REQARG_VERSION "version=%s"
#define WMS_REQARG_REQUEST "request=%s"
#define WMS_REQARG_LAYERS "layers=%s"
#define WMS_REQARG_STYLES "styles=%s"
#define WMS_REQARG_FORMAT "format=%s"
#define WMS_REQARG_BBOX "bbox=%%lf,%%lf,%%lf,%%lf"
#define WMS_REQARG_WIDTH "width=%d"
#define WMS_REQARG_HEIGHT "height=%d"
#define WMS_REQARG_SRS "srs=%s"
#define WMS_REQARG_CRS "crs=%s"

#define WMS_1_1_X_REQARGS "%s?" \
    WMS_REQARG_SERVICE "&" WMS_REQARG_VERSION "&" WMS_REQARG_REQUEST "&" \
    WMS_REQARG_LAYERS "&" WMS_REQARG_STYLES "&" WMS_REQARG_FORMAT "&" \
    WMS_REQARG_BBOX "&" WMS_REQARG_WIDTH "&" WMS_REQARG_HEIGHT "&" \
    WMS_REQARG_SRS

#define WMS_1_3_0_REQARGS "%s?" \
    WMS_REQARG_SERVICE "&" WMS_REQARG_VERSION "&" WMS_REQARG_REQUEST "&" \
    WMS_REQARG_LAYERS "&" WMS_REQARG_STYLES "&" WMS_REQARG_FORMAT "&" \
    WMS_REQARG_BBOX "&" WMS_REQARG_WIDTH "&" WMS_REQARG_HEIGHT "&" \
    WMS_REQARG_CRS

static int init_version(AVFilterContext *ctx) {
    WMSContext *s = ctx->priv;
    if(strcmp(s->version, "1.3.0") == 0) {
        s->wms_version = WMS_V1_3_0;
        return 0;
    }
    if(strcmp(s->version, "1.1.1") == 0) {
        s->wms_version = WMS_V1_1_1;
        return 0;
    }
    if(strcmp(s->version, "1.1.0") == 0) {
        s->wms_version = WMS_V1_1_0;
        return 0;
    }

    av_log(ctx, AV_LOG_ERROR,
           "WMS version '%s' not implemented. Available versions are '1.0.0' , '1.1.0', '1.1.1' and '1.3.0'\n",
           s->version);

    return AVERROR(EINVAL);

}



static char* format_url_arg(char* raw_arg) {
    #define NONEED_ESCAPE(ch) \
        (ch >='a' && ch <= 'z') || (ch >='A' && ch <= 'Z') || \
        (ch >='-' && ch <= '9' && ch != '/') ||(ch == '~' || ch == '_')

    int dst_i = 0;
    int ret;
    int result_size = 1;
    char* dst;
    char c;
    for (int i=0; (c=raw_arg[i]) != 0; i++) {
        if (NONEED_ESCAPE(c)) {
            result_size++;
        }else {
            result_size += 4;
        }
    }

    dst = av_malloc(result_size);
    dst[0] = 0;

    for (int i=0; (c=raw_arg[i]) != 0; i++) {
        if (NONEED_ESCAPE(c)) {
            dst[dst_i] = c;
            dst_i++;
            dst[dst_i] = '\0';
        }else {
            // adds %%xx instead of special char
            ret = snprintf(&dst[dst_i], 5, "%%%%%02X", (uint8_t)c);
            if (ret < 0) return NULL;
            if(ret < 4){ dst_i += ret; }else{ dst_i += 4; }
        }
    }
    return dst;
}

#define WMS_REQVAL_REQUEST "GetMap"
#define WMS_REQVAL_STYLES ""
#define WMS_REQVAL_FORMAT "image/png"
#define WMS_REQVAL_PROJ "EPSG:4326"

static int init_format(AVFilterContext *ctx) {
    WMSContext *s = ctx->priv;

    int ret;
    //Needs escaping: service , layers
    char *service = format_url_arg(s->service);
    char *layers = format_url_arg(s->layers);

    if (service == NULL) {
        av_log(ctx, AV_LOG_ERROR,
       "Could not escape 'service' URL param\n");
        ret = EINVAL;
        goto fail;
    }

    if (layers == NULL) {
        av_log(ctx, AV_LOG_ERROR,
    "Could not escape 'layers' arg for URL\n");
        ret = EINVAL;
        goto fail;
    }

    switch (s->wms_version) {
        case WMS_V1_3_0:
            s->fmt_url = av_asprintf(WMS_1_3_0_REQARGS, s->url,
                service, s->version, WMS_REQVAL_REQUEST,
                layers, WMS_REQVAL_STYLES, WMS_REQVAL_FORMAT,
                s->w, s->h, WMS_REQVAL_PROJ
                );
            break;
        default:
            s->fmt_url = av_asprintf(WMS_1_1_X_REQARGS, s->url,
                service, s->version, WMS_REQVAL_REQUEST,
                layers, WMS_REQVAL_STYLES, WMS_REQVAL_FORMAT,
                s->w, s->h, WMS_REQVAL_PROJ
                );
            break;
    }

    av_log(ctx, AV_LOG_DEBUG,"WMS URL format: %s", s->fmt_url);
    av_free(service);
    av_free(layers);

    if (strlen(s->fmt_url) <= 0) {
        av_log(ctx, AV_LOG_ERROR,
       "Could not build URL\n");
        ret = AVERROR(EIO);
        goto fail;
    };
    return 0;
fail:
    av_free(service);
    av_free(layers);
    return ret;
}

static av_cold int init(AVFilterContext *ctx)
{
    int ret;

    if ((ret=parse_getcapabilities(ctx))<0)
        return ret;
    if((ret = init_version(ctx)) < 0)
        return ret;
    if((ret = init_format(ctx)) < 0)
        return ret;

    av_log(ctx, AV_LOG_DEBUG, "Successfully initialized WMS Context\n");
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx){
    WMSContext *s = ctx->priv;
    free(s->url);
	free(s->service);
	free(s->version);
	av_free(s->fmt_url);
    av_log(ctx, AV_LOG_DEBUG, "Successfully uninitialized WMS Context\n");
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

    if ((ret = av_expr_parse_and_eval(&res, (expr = s->xref_expr),var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    var_values[VAR_XREF] = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = s->yref_expr),var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    var_values[VAR_YREF] = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = s->x1_expr),
                                              var_names, var_values,
                                              NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    mapctx->x1 = var_values[VAR_X1] = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = s->x2_expr),var_names, var_values,
                                              NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    mapctx->x2 = var_values[VAR_X2] = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = s->y1_expr),var_names, var_values,
                                              NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0)
        goto fail;
    mapctx->y1 = var_values[VAR_Y1] = res;
    if ((ret = av_expr_parse_and_eval(&res, (expr = s->y2_expr),var_names, var_values,
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

    url = av_asprintf(s->fmt_url,
        mctx.x1, mctx.y1, mctx.x2, mctx.y2);

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
