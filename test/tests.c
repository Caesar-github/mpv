#include "options/path.h"
#include "osdep/subprocess.h"
#include "player/core.h"
#include "tests.h"

static const struct unittest *unittests[] = {
    &test_chmap,
    &test_gl_video,
    &test_img_format,
    &test_json,
    &test_linked_list,
    &test_repack_sws,
#if HAVE_ZIMG
    &test_repack_zimg,
#endif
    NULL
};

bool run_tests(struct MPContext *mpctx)
{
    char *sel = mpctx->opts->test_mode;
    assert(sel && sel[0]);

    if (strcmp(sel, "help") == 0) {
        MP_INFO(mpctx, "Available tests:\n");
        for (int n = 0; unittests[n]; n++)
            MP_INFO(mpctx, "   %s\n", unittests[n]->name);
        MP_INFO(mpctx, "   all-simple\n");
        return true;
    }

    struct test_ctx ctx = {
        .global = mpctx->global,
        .log = mpctx->log,
        .ref_path = "test/ref",
        .out_path = "test/out",
    };

    if (!mp_path_isdir(ctx.ref_path)) {
        MP_FATAL(mpctx, "Must be run from git repo root dir.\n");
        abort();
    }
    mp_mkdirp(ctx.out_path);
    assert(mp_path_isdir(ctx.out_path));

    int num_run = 0;

    for (int n = 0; unittests[n]; n++) {
        const struct unittest *t = unittests[n];

        // Exactly 1 entrypoint please.
        assert(MP_IS_POWER_OF_2(
            (t->run        ? (1 << 1) : 0)));

        bool run = false;
        run |= strcmp(sel, "all-simple") == 0 && !t->is_complex;
        run |= strcmp(sel, t->name) == 0;

        if (run) {
            if (t->run)
                t->run(&ctx);
            num_run++;
        }
    }

    MP_INFO(mpctx, "%d unittests successfully run.\n", num_run);

    return num_run > 0; // still error if none
}

#ifdef NDEBUG
static_assert(false, "don't define NDEBUG for tests");
#endif

void assert_int_equal_impl(const char *file, int line, int64_t a, int64_t b)
{
    if (a != b) {
        printf("%s:%d: %"PRId64" != %"PRId64"\n", file, line, a, b);
        abort();
    }
}

void assert_string_equal_impl(const char *file, int line,
                              const char *a, const char *b)
{
    if (strcmp(a, b) != 0) {
        printf("%s:%d: '%s' != '%s'\n", file, line, a, b);
        abort();
    }
}

void assert_float_equal_impl(const char *file, int line,
                              double a, double b, double tolerance)
{
    if (fabs(a - b) > tolerance) {
        printf("%s:%d: %f != %f\n", file, line, a, b);
        abort();
    }
}

FILE *test_open_out(struct test_ctx *ctx, const char *name)
{
    char *path = mp_tprintf(4096, "%s/%s", ctx->out_path, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        MP_FATAL(ctx, "Could not open '%s' for writing.\n", path);
        abort();
    }
    return f;
}

void assert_text_files_equal_impl(const char *file, int line,
                                  struct test_ctx *ctx, const char *ref,
                                  const char *new, const char *err)
{
    char *path_ref = mp_tprintf(4096, "%s/%s", ctx->ref_path, ref);
    char *path_new = mp_tprintf(4096, "%s/%s", ctx->out_path, new);

    char *errstr = NULL;
    int res = mp_subprocess((char*[]){"diff", "-u", "--", path_ref, path_new, 0},
                            NULL, NULL, NULL, NULL, &errstr);

    if (res) {
        if (res == 1)
            MP_WARN(ctx, "Note: %s\n", err);
        MP_FATAL(ctx, "Giving up.\n");
        abort();
    }
}
