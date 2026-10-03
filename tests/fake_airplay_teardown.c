/* Stock's AirPlayReceiverSessionTearDown, as the car's libairplay.so behaves (map14 RE): the
 * whole session when the request is NULL, else only the streams it names; *outDone set only when
 * the caller gave a place for it.  Built as a shared library so the seam reaches it through
 * RTLD_NEXT, as it reaches libairplay.so on the car. */
#include <stddef.h>
#include <stdint.h>

void *g_fake_teardown_session;
void *g_fake_teardown_request;
int32_t g_fake_teardown_reason;
int g_fake_teardown_calls, g_fake_teardown_had_done;

void AirPlayReceiverSessionTearDown(void *session, void *request, int32_t reason, uint8_t *outDone) {
    ++g_fake_teardown_calls;
    g_fake_teardown_session = session;
    g_fake_teardown_request = request;
    g_fake_teardown_reason = reason;
    g_fake_teardown_had_done = outDone != NULL;
    if (outDone) *outDone = request == NULL;
}
