#include <stdbool.h>
#include <string.h>

#include <mpv/client.h>

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4)
        return 2;

    mpv_handle *ctx = mpv_create();
    if (!ctx)
        return 2;

    int result = 2;
    const char *options[][2] = {
        {"config", "no"},
        {"load-scripts", "no"},
        {"ytdl", "no"},
        {"terminal", "no"},
        {"vo", "null"},
        {"ao", "null"},
        {"curl-enabled", "yes"},
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        if (mpv_set_option_string(ctx, options[i][0], options[i][1]) < 0)
            goto done;
    }
    if (argc == 4 &&
        mpv_set_option_string(ctx, "http-header-fields", argv[3]) < 0)
        goto done;
    if (mpv_initialize(ctx) < 0)
        goto done;

    const char *command[] = {"loadfile", argv[1], NULL};
    result = 1;
    if (mpv_command(ctx, command) < 0)
        goto done;

    int64_t deadline = mpv_get_time_us(ctx) + 10000000;
    bool expect_success = strcmp(argv[2], "success") == 0;
    while (mpv_get_time_us(ctx) < deadline) {
        mpv_event *event = mpv_wait_event(ctx, 0.1);
        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            result = expect_success ? 0 : 1;
            break;
        }
        if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *end = event->data;
            result = !expect_success && end->error < 0 ? 0 : 1;
            break;
        }
    }
done:
    mpv_terminate_destroy(ctx);
    return result;
}
