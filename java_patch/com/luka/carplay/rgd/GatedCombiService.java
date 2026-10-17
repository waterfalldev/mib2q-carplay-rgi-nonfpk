/*
 * GatedCombiService - Wrapper around CombiBAPServiceNavi that gates
 * native route-guidance updates during CarPlay route guidance.
 *
 * When blockRouteGuidance=true, native route-guidance related calls from
 * CombiBAPListener are silently dropped so they don't overwrite BAPBridge.
 * This keeps BAPBridge as the single source for FctIDs
 * 17/18/19/20/21/22/23/24/39/46/49/55.
 *
 * Non-route-guidance methods always delegate.
 *
 *
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */
package com.luka.carplay.rgd;

import de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi;
import de.audi.atip.interapp.combi.bap.audio.data.CombiBAPTMCInfoMessage;
import de.audi.atip.interapp.combi.bap.navi.data.CombiBAPDestinationInfo;
import de.audi.atip.interapp.combi.bap.navi.data.CombiBAPDestinationListEntry;
import de.audi.atip.interapp.combi.bap.navi.data.CombiBAPNaviDestination;
import de.audi.atip.interapp.combi.bap.navi.data.CombiBAPNaviLaneGuidanceData;
import de.audi.atip.interapp.combi.bap.navi.data.CombiBAPNaviManeuverDescriptor;
import de.audi.atip.interapp.combi.bap.navi.data.CombiBAPSemiDynamicRouteInfo;
import de.audi.atip.interapp.combi.bap.navi.data.EtcStatus;

public class GatedCombiService implements CombiBAPServiceNavi {
    final CombiBAPServiceNavi real;
    volatile boolean blockRouteGuidance;
    /* FSG_OperationState is global NAVSD state.  Stock Navigation drives it through
     * show/hideInitializingScreen while its own map starts.  That INITIALIZING state
     * also covers the independent CarPlay displayable 99 on the VC, so defer the
     * stock request for the complete altScreen ownership interval. */
    private boolean blockInitializingState;
    private boolean deferredInitializing;
    private boolean haveDeferredInitializing;
    /* FctIDs 19/20/21/22/46 have different ownership from the rest of route guidance:
     * outside CarPlay RGI the stock navigator may keep driving the current/next-road
     * text, travel information and destination menu; maneuver/lane records remain
     * session-gated. */
    volatile boolean blockCurrentPositionInfo;
    private boolean haveMapScale;
    private int mapScaleAutoZoom, mapScaleValue, mapScaleUnit;
    private boolean mapScaleAutoZoomActive, mapScaleIntersectionSupported;
    private boolean haveAltitude;
    private int altitudeValue, altitudeUnit;

    public GatedCombiService(CombiBAPServiceNavi r) { this.real = r; }

    /** Keep NAVSD operational while plane 99 owns the cluster.  The latest stock
     * request is remembered and replayed on release; ClusterService additionally
     * recomputes the authoritative state in case no edge occurred during takeover. */
    public synchronized void setAltScreenTakeover(boolean active) {
        if (blockInitializingState == active) return;

        if (active) {
            haveDeferredInitializing = false;
            deferredInitializing = false;
            blockInitializingState = true;
            /* Clear an INITIALIZING value that may have been published before the
             * wrapper was installed.  This is independent of stock map readiness. */
            try { real.hideInitializingScreen(); } catch (Throwable t) { }
            return;
        }

        blockInitializingState = false;
        if (haveDeferredInitializing) {
            try {
                if (deferredInitializing) real.showInitializingScreen();
                else real.hideInitializingScreen();
            } catch (Throwable t) { }
        }
        haveDeferredInitializing = false;
    }

    /** The scale and altitude readouts are hidden only where CarPlay's own map plane replaces the
     *  stock map (the Virtual Cockpit).  A MOST or RGI-only cluster keeps showing stock's. */
    private static boolean hidesScaleAndAltitude() {
        return !com.luka.carplay.cluster.ClusterPlatform.leavesContextsToStock();
    }

    /** Route-data ownership. BAPBridge is the only writer while CarPlay RGI is active.
     * Keep stock compass and NAVSD state untouched; on the Virtual Cockpit hide scale and altitude. */
    public synchronized void setRouteGuidanceBlocked(boolean active) {
        if (blockRouteGuidance == active) return;
        blockRouteGuidance = active;
        if (!hidesScaleAndAltitude()) return;
        if (active) {
            try { sendHiddenMapScale(); } catch (Throwable t) { }
            try { real.updateAltitude(0x7FFF, 2); } catch (Throwable t) { }
        } else {
            try {
                if (haveMapScale) real.updateMapScale(
                    mapScaleAutoZoom, mapScaleAutoZoomActive, mapScaleValue,
                    mapScaleUnit, mapScaleIntersectionSupported);
            } catch (Throwable t) { }
            try {
                if (haveAltitude) real.updateAltitude(altitudeValue, altitudeUnit);
            } catch (Throwable t) { }
        }
    }

    private void sendHiddenMapScale() {
        real.updateMapScale(15, false, 0xFFFF, 0xFF, false);
    }

    public void setCurrentPositionInfoBlocked(boolean blocked) {
        blockCurrentPositionInfo = blocked;
    }

    /* GATED ROUTE-GUIDANCE METHODS */
    public void updateRGStatus(int a) {
        boolean pass = !blockRouteGuidance;
        lastRgStatus = trace("RGStatus", lastRgStatus, a, pass);
        if (pass) real.updateRGStatus(a);
    }

    /** on a MOST cluster FctID 39 is stock's answer to the cluster's own view choice
     *  (COMPASS 2, arrows 1, map 3; ClusterViewMode via CombiBAPListener.setViewMode).  Left
     *  unanswered, the cluster falls back to compass; CarPlay's Virtual Cockpit value (RGI, 0) is
     *  a view a MOST coding never offers.  So on MOST stock answers it throughout, and CarPlay
     *  sends the same value (BAPBridge.activeRGType). */
    public void updateActiveRGType(int a) {
        boolean pass = !blockRouteGuidance || com.luka.carplay.cluster.ClusterPlatform.isMost();
        lastActiveRgType = trace("ActiveRGType", lastActiveRgType, a, pass);
        if (pass) real.updateActiveRGType(a);
    }

    /* the cluster view handshake as stock sends it, one WARN line per change. */
    private int lastRgStatus = UNSET;
    private int lastActiveRgType = UNSET;
    private int lastInfoStates = UNSET;
    private static final int UNSET = Integer.MIN_VALUE;

    private static int trace(String what, int last, int value, boolean passed) {
        int key = passed ? value : -1000 - value;
        if (key != last && com.luka.carplay.cluster.ClusterPlatform.leavesContextsToStock()) {
            com.luka.carplay.framework.Log.w("ClusterView", "stock " + what + "=" + value + (passed ? "" : " (gated)"));
        }
        return key;
    }

    public void updateDistanceToNextManeuver(int a, int b, boolean c, int d) {
        if (!blockRouteGuidance) real.updateDistanceToNextManeuver(a, b, c, d);
    }

    public void updateCurrentPositionInfo(String s) {
        if (!blockCurrentPositionInfo) real.updateCurrentPositionInfo(s);
    }

    public void updateManeuverDescriptor(CombiBAPNaviManeuverDescriptor[] a) {
        if (!blockRouteGuidance) real.updateManeuverDescriptor(a);
    }

    public void updateLaneGuidance(boolean a, CombiBAPNaviLaneGuidanceData[] b) {
        if (!blockRouteGuidance) real.updateLaneGuidance(a, b);
    }

    public void updateExitView(int a, int b) {
        if (!blockRouteGuidance) real.updateExitView(a, b);
    }

    public void updateManeuverState(int a) {
        if (!blockRouteGuidance) real.updateManeuverState(a);
    }

    /* All other methods: pure delegation, except the stock NAVSD initialization
     * state which must not mask an active altScreen plane. */
    public synchronized void showInitializingScreen() {
        if (blockInitializingState) {
            deferredInitializing = true;
            haveDeferredInitializing = true;
            return;
        }
        real.showInitializingScreen();
    }
    public synchronized void hideInitializingScreen() {
        if (blockInitializingState) {
            deferredInitializing = false;
            haveDeferredInitializing = true;
            /* Reassert READY in case this callback crossed the takeover edge in
             * the underlying BAP initialization manager. */
            real.hideInitializingScreen();
            return;
        }
        real.hideInitializingScreen();
    }
    public void updateCompassInfo(int a, int b) { real.updateCompassInfo(a, b); }
    public void updateTurnToInfo(String a, String b) {
        if (!blockCurrentPositionInfo) real.updateTurnToInfo(a, b); }
    public void updateDistanceToDestination(int a, int b, boolean c) {
        if (!blockCurrentPositionInfo) real.updateDistanceToDestination(a, b, c); }
    public void updateTimeToDestination(int a, int b, long c) {
        if (!blockCurrentPositionInfo) real.updateTimeToDestination(a, b, c); }
    public void updateTMCInfoMessages(CombiBAPTMCInfoMessage[] a) {
        real.updateTMCInfoMessages(a); }
    public void updateLastDestinationsList(CombiBAPDestinationListEntry[] a) {
        real.updateLastDestinationsList(a); }
    public void updateFavoriteDestinationsList(CombiBAPDestinationListEntry[] a) {
        real.updateFavoriteDestinationsList(a); }
    public void updateHomeAddress(CombiBAPNaviDestination a) { real.updateHomeAddress(a); }
    public void routeGuidanceActDeactResult(int a) { real.routeGuidanceActDeactResult(a); }
    public void repeatLastNavAnnouncementResult(int a) {
        real.repeatLastNavAnnouncementResult(a); }
    public void updateVoiceGuidanceState(int a) { real.updateVoiceGuidanceState(a); }
    public void updateInfoStates(int a) {
        lastInfoStates = trace("InfoStates", lastInfoStates, a, true);
        real.updateInfoStates(a); }
    public void updateTrafficBlockIndication(int a) { real.updateTrafficBlockIndication(a); }
    public void updateMapColor(int a) { real.updateMapColor(a); }
    public void updateMapType(int a, int b) { real.updateMapType(a, b); }
    public void updateSupportedMapTypes(boolean a, int b) { real.updateSupportedMapTypes(a, b); }
    public void updateMapView(int a, int b) { real.updateMapView(a, b); }
    public void updateSupportedMapViews(int a, int b) { real.updateSupportedMapViews(a, b); }
    public void updateMapVisibility(boolean a, boolean b) { real.updateMapVisibility(a, b); }
    public void updateMapOrientation(int a) { real.updateMapOrientation(a); }
    public synchronized void updateMapScale(int a, boolean b, int c, int d, boolean e) {
        mapScaleAutoZoom = a;
        mapScaleAutoZoomActive = b;
        mapScaleValue = c;
        mapScaleUnit = d;
        mapScaleIntersectionSupported = e;
        haveMapScale = true;
        /* A steering-wheel MapScale SetGet must always receive a Status.  The
         * BAP framework bypasses value deduplication while setGetPending=true,
         * so resending the hidden value is both the ACK and the visibility rule. */
        if (blockRouteGuidance && hidesScaleAndAltitude()) sendHiddenMapScale();
        else real.updateMapScale(a, b, c, d, e);
    }
    public void updateDestinationInfo(CombiBAPDestinationInfo a) {
        if (!blockCurrentPositionInfo) real.updateDestinationInfo(a); }
    public synchronized void updateAltitude(int a, int b) {
        altitudeValue = a;
        altitudeUnit = b;
        haveAltitude = true;
        if (!(blockRouteGuidance && hidesScaleAndAltitude())) real.updateAltitude(a, b);
    }
    public void updateOnlineNavigationState(int a, int b, int c) {
        real.updateOnlineNavigationState(a, b, c); }
    public void updateSemidynamicRouteGuidance(CombiBAPSemiDynamicRouteInfo a) {
        real.updateSemidynamicRouteGuidance(a); }
    public void poiSearchResult(int a, int b) { real.poiSearchResult(a, b); }
    public void updatePOIListSize(int a) { real.updatePOIListSize(a); }
    public void updateFSGSetup(int a, boolean b) { real.updateFSGSetup(a, b); }
    public void updateMapPresentation(boolean a, boolean b, boolean c) {
        real.updateMapPresentation(a, b, c); }
    public void updateEtcStatus(EtcStatus a) { real.updateEtcStatus(a); }
}
