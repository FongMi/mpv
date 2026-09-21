#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVERROR(e) (-(e))
#define AVERROR_OPTION_NOT_FOUND (-1414549496)
#define AVERROR_INVALIDDATA (-1094995529)
#define AV_OPT_SEARCH_CHILDREN 1
#define MP_WARN(ctx, ...) ((void)(ctx))

typedef struct AVBitStreamFilter { int unused; } AVBitStreamFilter;
typedef struct AVCodecParameters { int unused; } AVCodecParameters;
typedef struct AVBSFContext {
    AVCodecParameters *par_in;
    int time_base_in;
} AVBSFContext;
typedef struct AVPacket AVPacket;
struct mp_codec_params { bool dv_el_present; };
struct sh_stream { struct mp_codec_params *codec; };
struct demux_opts { int dovi_profile7_mode; };
struct demuxer { struct demux_opts *opts; };

static const AVBitStreamFilter *av_bsf_get_by_name(const char *name);
static int av_bsf_alloc(const AVBitStreamFilter *filter, AVBSFContext **out);
static void av_bsf_free(AVBSFContext **context);
static AVCodecParameters *mp_codec_params_to_av(struct mp_codec_params *codec);
static int avcodec_parameters_copy(AVCodecParameters *to,
                                    const AVCodecParameters *from);
static void avcodec_parameters_free(AVCodecParameters **parameters);
static int mp_get_codec_timebase(struct mp_codec_params *codec);
static int av_opt_set(void *context, const char *option, const char *value,
                       int flags);
static int av_bsf_init(AVBSFContext *context);

#include "dovi_bsf_functions.h"

enum fail_stage {
    FILTER, ALLOC, PARAMETERS, COPY, OPTION, COMPRESSION, INIT, NUM_STAGES,
};

static int failures[3][NUM_STAGES];
static int attempts[3], live_contexts, live_parameters;
static int checks;
static struct mp_dovi_split *active;

static int failure(enum fail_stage stage)
{
    return failures[active->mode][stage];
}

static const AVBitStreamFilter *av_bsf_get_by_name(const char *name)
{
    static AVBitStreamFilter filter;
    assert(!strcmp(name, active->mode == DOVI_FILTER_P81
                          ? "dovi_rpu" : "dovi_split"));
    attempts[active->mode]++;
    return failure(FILTER) ? NULL : &filter;
}

static int av_bsf_alloc(const AVBitStreamFilter *filter, AVBSFContext **out)
{
    assert(filter);
    if (failure(ALLOC))
        return failure(ALLOC);
    *out = calloc(1, sizeof(**out));
    assert(*out);
    (*out)->par_in = calloc(1, sizeof(AVCodecParameters));
    assert((*out)->par_in);
    live_contexts++;
    return 0;
}

static void av_bsf_free(AVBSFContext **context)
{
    if (!*context)
        return;
    free((*context)->par_in);
    free(*context);
    *context = NULL;
    live_contexts--;
}

static AVCodecParameters *mp_codec_params_to_av(struct mp_codec_params *codec)
{
    assert(codec == active->bl->codec);
    if (failure(PARAMETERS))
        return NULL;
    AVCodecParameters *parameters = calloc(1, sizeof(*parameters));
    assert(parameters);
    live_parameters++;
    return parameters;
}

static int avcodec_parameters_copy(AVCodecParameters *to,
                                    const AVCodecParameters *from)
{
    assert(to && from);
    return failure(COPY);
}

static void avcodec_parameters_free(AVCodecParameters **parameters)
{
    assert(*parameters);
    free(*parameters);
    *parameters = NULL;
    live_parameters--;
}

static int mp_get_codec_timebase(struct mp_codec_params *codec)
{
    assert(codec == active->bl->codec);
    return 42;
}

static int av_opt_set(void *context, const char *option, const char *value,
                       int flags)
{
    assert(context && flags == AV_OPT_SEARCH_CHILDREN);
    if (!strcmp(option, "compression")) {
        assert(active->mode == DOVI_FILTER_P81 && !strcmp(value, "none"));
        return failure(COMPRESSION);
    }
    assert(!strcmp(option, active->mode == DOVI_FILTER_P81
                            ? "convert" : "mode"));
    const char *expected = active->mode == DOVI_FILTER_P81 ? "p81"
                         : active->mode == DOVI_FILTER_HDR10 ? "bl" : "el_rpu";
    assert(!strcmp(value, expected));
    return failure(OPTION);
}

static int av_bsf_init(AVBSFContext *context)
{
    assert(context->time_base_in == 42);
    return failure(INIT);
}

static void check(int requested, bool auto_mode, bool hdr10, bool enhancement,
                  int expected_ret, int expected_mode,
                  int p81_attempts, int hdr10_attempts, int split_attempts)
{
    struct demux_opts opts = {
        .dovi_profile7_mode = auto_mode ? DEMUX_DOVI_PROFILE7_AUTO : requested,
    };
    struct demuxer demuxer = {.opts = &opts};
    struct mp_codec_params codec = {.dv_el_present = enhancement};
    struct sh_stream stream = {.codec = &codec};
    struct mp_dovi_split split = {.demuxer = &demuxer, .bl = &stream};
    active = &split;
    memset(attempts, 0, sizeof(attempts));

    int ret = init_profile7_bsf(&split, requested, hdr10);
    assert(ret == expected_ret);
    assert(attempts[DOVI_FILTER_P81] == p81_attempts);
    assert(attempts[DOVI_FILTER_HDR10] == hdr10_attempts);
    assert(attempts[DOVI_FILTER_SPLIT] == split_attempts);
    assert(codec.dv_el_present == enhancement);
    if (ret >= 0) {
        assert(split.bsf && split.mode == expected_mode);
        av_bsf_free(&split.bsf);
    } else {
        assert(!split.bsf);
    }
    assert(live_contexts == 0 && live_parameters == 0);
    memset(failures, 0, sizeof(failures));
    checks++;
}

int main(void)
{
    check(DEMUX_DOVI_PROFILE7_PRESERVE, false, false, true,
          0, DOVI_FILTER_SPLIT, 0, 0, 1);
    check(DEMUX_DOVI_PROFILE7_HDR10, false, true, true,
          0, DOVI_FILTER_HDR10, 0, 1, 0);
    check(DEMUX_DOVI_PROFILE7_P81, false, true, true,
          0, DOVI_FILTER_P81, 1, 0, 0);

    // Missing conversion support retains the supported custom output paths.
    failures[DOVI_FILTER_P81][FILTER] = AVERROR(ENOENT);
    check(DEMUX_DOVI_PROFILE7_P81, false, false, true,
          0, DOVI_FILTER_HDR10, 1, 1, 0);
    failures[DOVI_FILTER_P81][OPTION] = AVERROR_OPTION_NOT_FOUND;
    check(DEMUX_DOVI_PROFILE7_P81, true, true, true,
          0, DOVI_FILTER_HDR10, 1, 1, 0);
    failures[DOVI_FILTER_P81][COMPRESSION] = AVERROR_OPTION_NOT_FOUND;
    check(DEMUX_DOVI_PROFILE7_P81, true, false, true,
          0, DOVI_FILTER_SPLIT, 1, 0, 1);
    failures[DOVI_FILTER_P81][FILTER] = AVERROR(ENOENT);
    failures[DOVI_FILTER_HDR10][OPTION] = AVERROR_OPTION_NOT_FOUND;
    check(DEMUX_DOVI_PROFILE7_P81, true, true, true,
          0, DOVI_FILTER_SPLIT, 1, 1, 1);
    failures[DOVI_FILTER_P81][FILTER] = AVERROR(ENOENT);
    check(DEMUX_DOVI_PROFILE7_P81, true, false, false,
          AVERROR(ENOENT), 0, 1, 0, 0);
    failures[DOVI_FILTER_P81][FILTER] = AVERROR(ENOENT);
    failures[DOVI_FILTER_HDR10][OPTION] = AVERROR_OPTION_NOT_FOUND;
    check(DEMUX_DOVI_PROFILE7_P81, false, true, true,
          AVERROR_OPTION_NOT_FOUND, 0, 1, 1, 0);

    // Initialization failures must stop instead of retrying a different mode.
    for (int stage = ALLOC; stage < NUM_STAGES; stage++) {
        failures[DOVI_FILTER_P81][stage] = AVERROR(ENOMEM);
        check(DEMUX_DOVI_PROFILE7_P81, false, true, true,
              AVERROR(ENOMEM), 0, 1, 0, 0);
        failures[DOVI_FILTER_P81][stage] = AVERROR(ENOMEM);
        check(DEMUX_DOVI_PROFILE7_P81, true, true, true,
              AVERROR(ENOMEM), 0, 1, 0, 0);
    }
    for (int stage = COPY; stage < NUM_STAGES; stage++) {
        failures[DOVI_FILTER_P81][stage] = AVERROR_INVALIDDATA;
        check(DEMUX_DOVI_PROFILE7_P81, true, true, true,
              AVERROR_INVALIDDATA, 0, 1, 0, 0);
    }
    failures[DOVI_FILTER_P81][FILTER] = AVERROR(ENOENT);
    failures[DOVI_FILTER_HDR10][COPY] = AVERROR(ENOMEM);
    check(DEMUX_DOVI_PROFILE7_P81, true, true, true,
          AVERROR(ENOMEM), 0, 1, 1, 0);
    failures[DOVI_FILTER_P81][FILTER] = AVERROR(ENOENT);
    failures[DOVI_FILTER_HDR10][INIT] = AVERROR(EINVAL);
    check(DEMUX_DOVI_PROFILE7_P81, true, true, true,
          AVERROR(EINVAL), 0, 1, 1, 0);
    failures[DOVI_FILTER_P81][INIT] = AVERROR(EINVAL);
    check(DEMUX_DOVI_PROFILE7_P81, true, true, true,
          AVERROR(EINVAL), 0, 1, 0, 0);
    failures[DOVI_FILTER_HDR10][ALLOC] = AVERROR(ENOMEM);
    check(DEMUX_DOVI_PROFILE7_HDR10, false, true, true,
          AVERROR(ENOMEM), 0, 0, 1, 0);
    failures[DOVI_FILTER_SPLIT][INIT] = AVERROR_INVALIDDATA;
    check(DEMUX_DOVI_PROFILE7_PRESERVE, false, false, true,
          AVERROR_INVALIDDATA, 0, 0, 0, 1);
    printf("Dolby Vision BSF initialization and mode selection: "
           "%d cases passed\n",
           checks);
    return 0;
}
