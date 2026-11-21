#include "options/m_config_frontend.h"
#include "test_utils.h"

struct test_opts {
    char **ignore_options;
    int font_size;
    int volume;
};

#define OPT_BASE_STRUCT struct test_opts
static const struct m_sub_options test_opts = {
    .opts = (const struct m_option[]){
        {"config-ignore-options", OPT_STRINGLIST(ignore_options), .flags = M_OPT_NOCFG},
        {"sub-font-size", OPT_INT(font_size)},
        {"sub-size", OPT_ALIAS("sub-font-size")},
        {"volume", OPT_INT(volume)},
        {0},
    },
    .size = sizeof(struct test_opts),
    .defaults = &(const struct test_opts){.font_size = 38, .volume = 100},
};

int main(void)
{
    struct m_config *config = m_config_new(NULL, mp_null_log, &test_opts);
    struct test_opts *opts = config->optstruct;
    bstr guard = bstr0("config-ignore-options");
    bstr font_size = bstr0("sub-font-size"), volume = bstr0("volume");

    assert_int_equal(m_config_set_option_cli(config, guard, font_size,
                                            M_SETOPT_FROM_CMDLINE), 0);
    assert_int_equal(m_config_set_option_cli(config, font_size, bstr0("112"),
                                            M_SETOPT_FROM_CONFIG_FILE), 0);
    assert_int_equal(m_config_set_option_cli(config, volume, bstr0("61"),
                                            M_SETOPT_FROM_CONFIG_FILE), 0);
    assert_int_equal(opts->font_size, 38);
    assert_int_equal(opts->volume, 61);

    struct m_profile *profile = m_config_add_profile(config, "test");
    assert_int_equal(m_config_set_profile_option(config, profile, bstr0("sub-size"),
                                                 bstr0("96")), 0);
    assert_int_equal(m_config_set_profile_option(config, profile, volume, bstr0("73")), 1);
    assert_int_equal(m_config_set_profile(config, "test", 0), 0);
    assert_int_equal(opts->font_size, 38);
    assert_int_equal(opts->volume, 73);

    assert_int_equal(m_config_set_option_cli(config, font_size, bstr0("40"),
                                            M_SETOPT_FROM_CMDLINE), 0);
    assert_int_equal(opts->font_size, 40);
    talloc_free(config);
    return 0;
}
