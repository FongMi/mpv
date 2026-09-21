#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum stream_type { STREAM_VIDEO, STREAM_AUDIO };
enum {
    M_PROPERTY_ERROR = -1, M_PROPERTY_UNKNOWN = -2,
    M_PROPERTY_UNAVAILABLE = -3, M_PROPERTY_NOT_IMPLEMENTED = -4,
    M_PROPERTY_OK = 1,
};
enum { M_PROPERTY_GET, M_PROPERTY_SET, M_PROPERTY_GET_TYPE };
#define M_SETOPT_BACKUP 16
#define OSD_LEVEL_INVISIBLE -1

typedef const char *bstr;
static bstr bstr0(const char *s) { return s; }
struct m_obj_settings { int value; };
struct m_option_action { const char *name; };
struct m_option_type { const struct m_option_action *actions; };
struct m_option { const struct m_option_type *type; bool filter; };
struct m_property_action_arg { const char *key; int action; void *arg; };
union m_option_value { struct m_obj_settings *filters; int number; };
static const union m_option_value m_option_value_default = {0};
struct m_config_option {
    const char *name;
    const struct m_option *opt;
    void *data;
    bool backed_up;
    int backup_value;
};
struct MPOpts {
    struct m_obj_settings *af_settings, *vf_settings;
    int osd_duration;
};
struct m_config {
    struct m_config_option options[3];
    struct MPContext *ctx;
    int raw_sets, last_flags, backups, notifications;
};
typedef struct MPContext {
    struct MPOpts *opts;
    struct m_config *mconfig;
    bool playing, audio_chain, video_chain;
    void *log;
    int audio_reinits, video_reinits, wakeups, osd_messages;
} MPContext;
struct mp_cmd_arg { union { char *s; } v; };
struct mp_cmd_ctx {
    struct MPContext *mpctx;
    struct mp_cmd_arg args[3];
    bool success, msg_osd;
    int on_osd;
};

static const struct m_option_action actions[] = {{"add"}, {"remove"}, {0}};
static const struct m_option_type filter_type = {.actions = actions};
static const struct m_option filter_option = {.type = &filter_type, .filter = true};
static const struct m_option number_option = {0};
static const char *const filter_opt[] = {"vf", "af"};

static void m_option_free(const struct m_option *opt, void *data)
{
    if (opt->filter) {
        free(*(struct m_obj_settings **)data);
        *(struct m_obj_settings **)data = NULL;
    }
}

static void m_option_copy(const struct m_option *opt, void *dst, const void *src)
{
    if (opt->filter) {
        const struct m_obj_settings *value = *(struct m_obj_settings *const *)src;
        struct m_obj_settings *copy = NULL;
        if (value) {
            copy = malloc(sizeof(*copy));
            assert(copy);
            *copy = *value;
        }
        m_option_free(opt, dst);
        *(struct m_obj_settings **)dst = copy;
    } else {
        *(int *)dst = *(const int *)src;
    }
}

static struct m_config_option *m_config_get_co(struct m_config *config, bstr name)
{
    for (int i = 0; i < 3; i++) {
        if (strcmp(config->options[i].name, name) == 0)
            return &config->options[i];
    }
    return NULL;
}

static void m_config_backup_opt(struct m_config *config, const char *name)
{
    struct m_config_option *co = m_config_get_co(config, name);
    assert(co);
    if (!co->backed_up) {
        co->backed_up = true;
        co->backup_value = co->opt->filter ?
            (*(struct m_obj_settings **)co->data)->value : *(int *)co->data;
        config->backups++;
    }
}

static void m_config_notify_change_opt_ptr(struct m_config *config, void *ptr)
{
    assert(ptr == &config->ctx->opts->af_settings ||
           ptr == &config->ctx->opts->vf_settings);
    config->notifications++;
}

static int reinit_audio_filters(struct MPContext *ctx)
{
    ctx->audio_reinits++;
    return ctx->audio_chain && ctx->opts->af_settings &&
           ctx->opts->af_settings->value < 0 ? -1 : 0;
}

static int reinit_video_filters(struct MPContext *ctx)
{
    ctx->video_reinits++;
    return ctx->video_chain && ctx->opts->vf_settings &&
           ctx->opts->vf_settings->value < 0 ? -1 : 0;
}

static void mp_wakeup_core(struct MPContext *ctx) { ctx->wakeups++; }
static int m_config_set_option_raw(struct m_config *config,
                                  struct m_config_option *co, void *data, int flags);
static int mp_property_do(const char *name, int action, void *arg, MPContext *ctx);

static void set_osd_msg(struct MPContext *ctx, int level, int duration,
                        const char *format, ...)
{
    (void)level;
    (void)duration;
    (void)format;
    ctx->osd_messages++;
}

static void show_property_osd(struct MPContext *ctx, const char *name, int flags)
{
    (void)ctx;
    (void)name;
    (void)flags;
}

static char *mp_tprintf(int size, const char *format, ...)
{
    static char text[80];
    assert(size <= (int)sizeof(text));
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

static int m_option_parse(void *log, const struct m_option *opt, bstr name,
                          bstr text, void *dst)
{
    (void)log;
    struct m_obj_settings value = {.value = strcmp(text, "reject") == 0 ? -1 : 2};
    struct m_obj_settings *ptr = strstr(name, "-remove") ? NULL : &value;
    m_option_copy(opt, dst, &ptr);
    return 0;
}

#include "filter_options_functions.h"

// Model the generic option callback: its void result cannot reject the write.
static int m_config_set_option_raw(struct m_config *config,
                                  struct m_config_option *co, void *data, int flags)
{
    config->raw_sets++;
    config->last_flags = flags;
    if (flags & M_SETOPT_BACKUP)
        m_config_backup_opt(config, co->name);
    m_option_copy(co->opt, co->data, data);
    if (co->opt->filter) {
        int type = co->data == &config->ctx->opts->af_settings ?
                   STREAM_AUDIO : STREAM_VIDEO;
        set_filters(config->ctx, type, *(struct m_obj_settings **)co->data);
    }
    return 0;
}

static int mp_property_do(const char *name, int action, void *arg, MPContext *ctx)
{
    const char *local_prefix = "file-local-options/";
    bool local = strncmp(name, local_prefix, strlen(local_prefix)) == 0;
    const char *key = name + strlen(local ? local_prefix : "options/");
    struct m_property_action_arg ka = {.key = key, .action = action, .arg = arg};
    return access_options(&ka, local, ctx);
}

struct fixture {
    MPContext ctx;
    struct MPOpts opts;
    struct m_config config;
    int unrelated;
};

static void init(struct fixture *f)
{
    *f = (struct fixture){0};
    f->ctx = (MPContext){
        .opts = &f->opts, .mconfig = &f->config, .playing = true,
        .audio_chain = true, .video_chain = true,
    };
    f->config.ctx = &f->ctx;
    f->config.options[0] = (struct m_config_option){
        .name = "af", .opt = &filter_option, .data = &f->opts.af_settings};
    f->config.options[1] = (struct m_config_option){
        .name = "vf", .opt = &filter_option, .data = &f->opts.vf_settings};
    f->config.options[2] = (struct m_config_option){
        .name = "unrelated", .opt = &number_option, .data = &f->unrelated};
    struct m_obj_settings value = {.value = 1};
    struct m_obj_settings *ptr = &value;
    m_option_copy(&filter_option, &f->opts.af_settings, &ptr);
    m_option_copy(&filter_option, &f->opts.vf_settings, &ptr);
}

static void finish(struct fixture *f)
{
    m_option_free(&filter_option, &f->opts.af_settings);
    m_option_free(&filter_option, &f->opts.vf_settings);
}

static void change_filter(struct fixture *f, const char *name,
                          const char *operation, const char *value, bool success)
{
    struct mp_cmd_ctx cmd = {
        .mpctx = &f->ctx, .success = true,
        .args = {{{.s = (char *)name}}, {{.s = (char *)operation}},
                 {{.s = (char *)value}}},
    };
    cmd_change_list(&cmd);
    assert(cmd.success == success);
}

static void test_filter_transaction(bool audio, bool local)
{
    struct fixture f;
    init(&f);
    const char *name = audio ? "af" : "vf";
    struct m_config_option *co = m_config_get_co(&f.config, name);
    char property[40];
    snprintf(property, sizeof(property), "%s/%s",
             local ? "file-local-options" : "options", name);

    change_filter(&f, property, "add", "reject", false);
    assert((*(struct m_obj_settings **)co->data)->value == 1);
    assert(f.config.notifications == 0);
    change_filter(&f, property, "add", "accept", true);
    assert((*(struct m_obj_settings **)co->data)->value == 2);
    assert(f.config.notifications == 1);
    change_filter(&f, property, "add", "reject", false);
    assert((*(struct m_obj_settings **)co->data)->value == 2);
    assert(f.config.notifications == 1);
    change_filter(&f, property, "remove", "accept", true);
    assert(!*(struct m_obj_settings **)co->data);
    assert(f.config.notifications == 2 && f.config.raw_sets == 0);
    assert(f.config.backups == (local ? 1 : 0));
    assert(!local || co->backup_value == 1);
    assert(f.ctx.wakeups == 4 && f.ctx.osd_messages == 2);
    assert((audio ? f.ctx.audio_reinits : f.ctx.video_reinits) == 4);
    assert((audio ? f.ctx.video_reinits : f.ctx.audio_reinits) == 0);
    finish(&f);
}

static void test_local_idle(void)
{
    struct fixture f;
    init(&f);
    f.ctx.playing = false;
    change_filter(&f, "file-local-options/af", "add", "accept", false);
    change_filter(&f, "file-local-options/vf", "add", "accept", false);
    assert(f.config.raw_sets == 0 && f.config.backups == 0);
    assert(f.ctx.audio_reinits == 0 && f.ctx.video_reinits == 0);
    assert(f.opts.af_settings->value == 1 && f.opts.vf_settings->value == 1);
    finish(&f);
}

static void test_global_idle(bool audio)
{
    struct fixture f;
    init(&f);
    f.ctx.playing = f.ctx.audio_chain = f.ctx.video_chain = false;
    const char *name = audio ? "options/af" : "options/vf";
    change_filter(&f, name, "add", "reject", true);
    assert((audio ? f.opts.af_settings : f.opts.vf_settings)->value == -1);
    assert(f.config.raw_sets == 0 && f.config.backups == 0);
    assert(f.config.notifications == 1);
    finish(&f);
}

static void test_unrelated_option(bool local)
{
    struct fixture f;
    init(&f);
    int value = 9;
    struct m_property_action_arg ka = {
        .key = "unrelated", .action = M_PROPERTY_SET, .arg = &value};
    assert(access_options(&ka, local, &f.ctx) == M_PROPERTY_OK);
    assert(f.unrelated == 9 && f.config.raw_sets == 1);
    assert(f.config.last_flags == (local ? M_SETOPT_BACKUP : 0));
    assert(f.config.backups == (local ? 1 : 0));
    assert(f.ctx.audio_reinits == 0 && f.ctx.video_reinits == 0);
    finish(&f);
}

int main(void)
{
    test_filter_transaction(true, true);
    test_filter_transaction(false, true);
    test_filter_transaction(true, false);
    test_filter_transaction(false, false);
    test_local_idle();
    test_global_idle(true);
    test_global_idle(false);
    test_unrelated_option(true);
    test_unrelated_option(false);
    puts("filter option transactions: 9 cases passed");
    return 0;
}
