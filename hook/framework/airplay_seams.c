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

HOOK_EXPORT void *AirPlayCopyServerInfo(void *a0, void *a1, void *a2, void *a3) {
    void *info;
    size_t i;
    pthread_once(&g_resolve_once, resolve);
    if (!g_real_copy_server_info) {
        LOG_ERROR(LOG_MODULE, "AirPlayCopyServerInfo real symbol UNRESOLVED");
        return NULL;
    }
    info = g_real_copy_server_info(a0, a1, a2, a3);
    if (!info || !seams_active()) return info;
    for (i = 0; i < hook_framework_module_count(); ++i) {
        const hook_module_def_t *def = hook_framework_module_at(i);
        if (def && def->airplay.on_server_info) def->airplay.on_server_info(a0, &info);
    }
    return info;
}

HOOK_EXPORT int AirPlayReceiverSessionSetup(void *session, void *request, void **response) {
    hook_setup_ctx_t setup;
    size_t i;
    int active;
    pthread_once(&g_resolve_once, resolve);
    if (!g_real_session_setup) {
        LOG_ERROR(LOG_MODULE, "AirPlayReceiverSessionSetup real symbol UNRESOLVED");
        return -6700;                       /* kUnknownErr */
    }
    active = seams_active();
    if (!active) return g_real_session_setup(session, request, response);
    setup.session = session;
    setup.request = request;
    setup.response = NULL;
    setup.sequence = __sync_add_and_fetch(&g_setup_sequence, 1);
    setup.result = 0;
    for (i = 0; i < hook_framework_module_count(); ++i) {
        const hook_module_def_t *def = hook_framework_module_at(i);
        if (def && def->airplay.on_setup_request) def->airplay.on_setup_request(&setup);
    }
    setup.result = g_real_session_setup(session, setup.request, response);
    if (setup.request != request) {
        /* A module's replacement request (a CF object of its own): released once stock is done. */
        const cflite_t *cf = cflite();
        LOG_WARN(LOG_MODULE, "SETUP %u with a module's request: stock result %d", setup.sequence, setup.result);
        if (cf) cf->release(setup.request);
        setup.request = request;
    }
    setup.response = response ? *response : NULL;
    for (i = 0; i < hook_framework_module_count(); ++i) {
        const hook_module_def_t *def = hook_framework_module_at(i);
        if (def && def->airplay.on_setup_response) def->airplay.on_setup_response(&setup);
    }
    if (response) *response = setup.response;
    return setup.result;
}

HOOK_EXPORT int AirPlayReceiverSessionSetSecurityInfo(void *session, const uint8_t *key, const uint8_t *iv) {
    int result;
    size_t i;
    pthread_once(&g_resolve_once, resolve);
    if (!g_real_set_security_info) {
        LOG_ERROR(LOG_MODULE, "AirPlayReceiverSessionSetSecurityInfo real symbol UNRESOLVED");
        return -6700;                       /* kUnknownErr */
    }
    result = g_real_set_security_info(session, key, iv);
    if (result != 0 || !key || !iv || !seams_active()) return result;
    for (i = 0; i < hook_framework_module_count(); ++i) {
        const hook_module_def_t *def = hook_framework_module_at(i);
        if (def && def->airplay.on_security_info) def->airplay.on_security_info(session, key, iv);
    }
    return result;
}

HOOK_EXPORT void AirPlayReceiverSessionTearDown(void *session, void *request, int32_t reason, uint8_t *outDone) {
    uint8_t done = 0;
    size_t i;
    pthread_once(&g_resolve_once, resolve);
    if (!g_real_session_teardown) {
        LOG_ERROR(LOG_MODULE, "AirPlayReceiverSessionTearDown real symbol UNRESOLVED");
        return;
    }
    /* Stock's own verdict on whether the whole session ended, also when the caller passed no
     * place for it. */
    g_real_session_teardown(session, request, reason, outDone ? outDone : &done);
    if (!session || !seams_active()) return;
    if (outDone) done = *outDone;
    for (i = 0; i < hook_framework_module_count(); ++i) {
        const hook_module_def_t *def = hook_framework_module_at(i);
        if (def && def->airplay.on_session_teardown) def->airplay.on_session_teardown(session, done != 0);
    }
}
