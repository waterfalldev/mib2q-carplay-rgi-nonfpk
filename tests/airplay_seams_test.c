/* The AirPlayReceiverSessionTearDown seam (hook/framework/airplay_seams.c) against a fake of
 * stock's (tests/fake_airplay_teardown.c, a shared library reached through RTLD_NEXT as
 * libairplay.so is on the car).  Checks: every call reaches stock unchanged; modules hear
 * stock's own verdict on whether the whole session ended, also when the caller passed no
 * outDone (the seam then gives stock one); the caller's outDone keeps stock's answer; nothing
 * is reported for a NULL session, outside dio_manager or before the framework has modules. */
#include <stdint.h>
#include <stdio.h>

#include "framework/hook_framework.h"
#include "framework/cflite.h"
#include "most/check.h"

extern void *g_fake_teardown_session;
extern void *g_fake_teardown_request;
extern int32_t g_fake_teardown_reason;
extern int g_fake_teardown_calls, g_fake_teardown_had_done;

/* The seam's own definition (airplay_seams.c), the one stock's PLT reaches on the car. */
void AirPlayReceiverSessionTearDown(void *session, void *request, int32_t reason, uint8_t *outDone);

/* ---------------------------------------------------------------- fake framework */

static int g_reported, g_whole;
static void *g_reported_session;
static int g_dio = 1;
static size_t g_modules = 1;

static void on_teardown(void *session, int whole) {
    ++g_reported;
    g_reported_session = session;
    g_whole = whole;
}

static const hook_module_def_t g_module = { .name = "test", .airplay = { .on_session_teardown = on_teardown } };

size_t hook_framework_module_count(void) { return g_modules; }
const hook_module_def_t *hook_framework_module_at(size_t index) { return index == 0 ? &g_module : NULL; }
int hook_process_is_dio_manager(void) { return g_dio; }
const cflite_t *cflite(void) { return NULL; }

/* ---------------------------------------------------------------- checks */

int main(void) {
    int session, request;
    uint8_t done = 7;

    AirPlayReceiverSessionTearDown(&session, NULL, -6753, NULL);
    check(g_fake_teardown_calls == 1 && g_fake_teardown_session == &session && !g_fake_teardown_request
          && g_fake_teardown_reason == -6753, "a teardown reaches stock with its arguments");
    check(g_fake_teardown_had_done, "stock is given a place for its verdict when the caller passed none");
    check(g_reported == 1 && g_reported_session == &session && g_whole == 1,
          "modules hear that the whole session ended");

    AirPlayReceiverSessionTearDown(&session, &request, 0, &done);
    check(g_fake_teardown_calls == 2 && g_fake_teardown_request == &request && done == 0,
          "a teardown of named streams: the caller gets stock's answer (%d)", done);
    check(g_reported == 2 && g_whole == 0, "modules hear it was not the whole session");

    done = 7;
    AirPlayReceiverSessionTearDown(&session, NULL, 0, &done);
    check(done == 1 && g_reported == 3 && g_whole == 1, "the whole session with the caller's outDone: %d", done);

    AirPlayReceiverSessionTearDown(NULL, NULL, 0, NULL);
    check(g_fake_teardown_calls == 4 && g_reported == 3, "a NULL session reaches stock, nothing is reported");

    g_dio = 0;
    AirPlayReceiverSessionTearDown(&session, NULL, 0, NULL);
    g_dio = 1;
    g_modules = 0;
    AirPlayReceiverSessionTearDown(&session, NULL, 0, NULL);
    g_modules = 1;
    check(g_fake_teardown_calls == 6 && g_reported == 3,
          "outside dio_manager or before the framework has modules: a plain pass-through");

    printf("airplay_seams_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
