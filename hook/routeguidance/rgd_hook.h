/*
 * Route Guidance Hook Module
 *
 * Enables CarPlay Route Guidance (TBT navigation) on MU1316:
 * - Patches Identify to advertise RouteGuidanceDisplayComponent
 * - Injects StartRouteGuidanceUpdates (0x5200) after auth
 * - Parses RouteGuidanceUpdate (0x5201), ManeuverUpdate (0x5202),
 *   and LaneGuidanceInformation (0x5204)
 * - Merges each packet into a cache and writes the full snapshot to the bus
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */

#ifndef RGD_HOOK_H
#define RGD_HOOK_H

#include "../framework/hook_framework.h"
#include "rgd_tlv.h"

/* Registered by the framework from hook_module_table (hook/main.c). */
extern const hook_module_def_t rgd_module_def;

/* Shutdown route guidance module (module def's on_shutdown) */
void rgd_shutdown(void);

/* Force send 0x5200 request */
hook_result_t rgd_request_updates(void);

/* Clear cache and bus state */
void rgd_clear_state(const char* reason);

#endif /* RGD_HOOK_H */
