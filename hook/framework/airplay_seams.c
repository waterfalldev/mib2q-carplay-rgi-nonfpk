/*
 * AirPlay seams (hook_framework.h hook_airplay_seams_t): stock libairplay entry points stock
 * reaches through its PLT (docs/hook/integration-seam.md), interposed once for every module.
 * Each forwards to stock unchanged and runs module callbacks around it; outside dio_manager,
 * or before the framework has its modules, they are plain pass-throughs.
 *   AirPlayCopyServerInfo                  the /info dictionary (the caller releases it)
 *   AirPlayReceiverSessionSetup            (session, request, CFDictionary *outResponse)
 *   AirPlayReceiverSessionSetSecurityInfo  (session, key[16], iv[16]): the session's master key
 *   AirPlayReceiverSessionTearDown         (session, request, reason, Boolean *outDone): the
 *       whole session when the request names no streams - stock then sets *outDone (map14)
 */
#include "hook_framework.h"
#include "cflite.h"

#include <dlfcn.h>
#include <pthread.h>

#define LOG_MODULE "airplay"

typedef void *(*copy_server_info_fn)(void *, void *, void *, void *);
typedef int (*session_setup_fn)(void *, void *, void **);
typedef int (*set_security_info_fn)(void *, const uint8_t *, const uint8_t *);
typedef void (*session_teardown_fn)(void *, void *, int32_t, uint8_t *);

static copy_server_info_fn g_real_copy_server_info;
static session_setup_fn g_real_session_setup;
static set_security_info_fn g_real_set_security_info;
static session_teardown_fn g_real_session_teardown;
static pthread_once_t g_resolve_once = PTHREAD_ONCE_INIT;
static uint32_t g_setup_sequence;

static void resolve(void) {
    *(void **)&g_real_copy_server_info = dlsym(RTLD_NEXT, "AirPlayCopyServerInfo");
    *(void **)&g_real_session_setup = dlsym(RTLD_NEXT, "AirPlayReceiverSessionSetup");
    *(void **)&g_real_set_security_info = dlsym(RTLD_NEXT, "AirPlayReceiverSessionSetSecurityInfo");
    *(void **)&g_real_session_teardown = dlsym(RTLD_NEXT, "AirPlayReceiverSessionTearDown");
}

/* The modules exist once the framework is up (its first Cinemo boundary, before any AirPlay
 * session); the seams never start it early. */
static int seams_active(void) {
    return hook_framework_module_count() > 0 && hook_process_is_dio_manager();
}

/* Every seam first resolves stock's entry points, and gives up when its own is missing. */
#define REQUIRE_REAL(fn, name, ret) \
    pthread_once(&g_resolve_once, resolve); \
    if (!(fn)) { LOG_ERROR(LOG_MODULE, name " real symbol UNRESOLVED"); return ret; }

/* Run `cb` of every module that declares it, in table order. */
#define SEAM_EACH(cb, args) do { \
    const hook_module_def_t *d_; \
    size_t i_; \
    for (i_ = 0; (d_ = hook_framework_module_at(i_)); ++i_) \
        if (d_->airplay.cb) d_->airplay.cb args; \
} while (0)

HOOK_EXPORT void *AirPlayCopyServerInfo(void *a0, void *a1, void *a2, void *a3) {
    void *info;
    REQUIRE_REAL(g_real_copy_server_info, "AirPlayCopyServerInfo", NULL)
    info = g_real_copy_server_info(a0, a1, a2, a3);
    if (!info || !seams_active()) return info;
    SEAM_EACH(on_server_info, (a0, &info));
    return info;
}

HOOK_EXPORT int AirPlayReceiverSessionSetup(void *session, void *request, void **response) {
    hook_setup_ctx_t setup;
    REQUIRE_REAL(g_real_session_setup, "AirPlayReceiverSessionSetup", -6700 /* kUnknownErr */)
    if (!seams_active()) return g_real_session_setup(session, request, response);
    setup.session = session;
    setup.request = request;
    setup.response = NULL;
    setup.sequence = __sync_add_and_fetch(&g_setup_sequence, 1);
    setup.result = 0;
    SEAM_EACH(on_setup_request, (&setup));
    setup.result = g_real_session_setup(session, setup.request, response);
    if (setup.request != request) {
        /* A module's replacement request (a CF object of its own): released once stock is done. */
        const cflite_t *cf = cflite();
        LOG_WARN(LOG_MODULE, "SETUP %u with a module's request: stock result %d", setup.sequence, setup.result);
        if (cf) cf->release(setup.request);
        setup.request = request;
    }
    setup.response = response ? *response : NULL;
    SEAM_EACH(on_setup_response, (&setup));
    if (response) *response = setup.response;
    return setup.result;
}

HOOK_EXPORT int AirPlayReceiverSessionSetSecurityInfo(void *session, const uint8_t *key, const uint8_t *iv) {
    int result;
    REQUIRE_REAL(g_real_set_security_info, "AirPlayReceiverSessionSetSecurityInfo", -6700 /* kUnknownErr */)
    result = g_real_set_security_info(session, key, iv);
    if (result != 0 || !key || !iv || !seams_active()) return result;
    SEAM_EACH(on_security_info, (session, key, iv));
    return result;
}

HOOK_EXPORT void AirPlayReceiverSessionTearDown(void *session, void *request, int32_t reason, uint8_t *outDone) {
    uint8_t done = 0;
    REQUIRE_REAL(g_real_session_teardown, "AirPlayReceiverSessionTearDown", )
    /* Stock's own verdict on whether the whole session ended, also when the caller passed no
     * place for it. */
    g_real_session_teardown(session, request, reason, outDone ? outDone : &done);
    if (!session || !seams_active()) return;
    if (outDone) done = *outDone;
    SEAM_EACH(on_session_teardown, (session, done != 0));
}
