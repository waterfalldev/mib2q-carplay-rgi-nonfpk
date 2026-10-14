/*
 * CarPlay Route Guidance Module
 *
 * Subscribes to EVT_RGD_UPDATE on CarplayBus, parses route state,
 * sends to BAP cluster.
 *
 * Payload is text in key:type:value format carried over into the bus
 * for minimal consumer churn:
 *   @routeguidance
 *   route_state:n:<0-6>
 *   dist_maneuver_m:n:<meters>
 *   m0_type:n:<maneuver type>
 *   ...
 *
 * Copyright (c) 2026 LuKa (@LuKa_dev)
 */
package com.luka.carplay.rgd;

import com.luka.carplay.bus.CarplayBus;
import com.luka.carplay.framework.Log;

public class RouteGuidance implements CarplayBus.Listener {

    private static final String TAG = "RouteGuidance";
    private static final int MAX_MANEUVERS = 32;
    /*
     * RouteGuidanceState values verified from MHI3 dio_manager:
     * 0=NO_ROUTE_SET, 1=ROUTE_SET, 2=ARRIVED, 3=LOADING, 4=LOCATING, 5=REROUTING, 6=PROCEED_TO_ROUTE
     */
    private static final int ROUTE_STATE_NO_ROUTE_SET = 0;
    private static final int ROUTE_STATE_ROUTE_SET = 1;

    /* State */
    private BAPBridge bap;
    private volatile boolean running;
    private volatile boolean rgActive = false;
    /* Separate from rgActive: route intent/BAP ownership may already be active while stock
     * ctx 74 is still held.  FRAME_READY gates the route-text scroll and cached-state replay. */
    private boolean presentationConfirmed = false;
    /* One WARN per pending episode; the retry runs on every RG update. */
    private boolean bapStartPendingLogged = false;
    private boolean hasRouteUpdate = false;
    private boolean routeWantsActive = false;

    /* Renderer readiness is independent of iOS RGI deltas. A tiny dedicated worker advances
     * the cached presentation state on READY/FRAME_READY/death and on a bounded retry tick. */
    private final Object presentationLock = new Object();
    private Thread presentationThread;
    private boolean presentationWake;
    private boolean presentationDrivePending;
    private boolean infoPresentationRefreshPending;
    private boolean rendererViewportRefreshPending;
    /* Guarded by presentationLock. View always writes phase 0; the steering-
     * wheel encoder press toggles it. The worker is the sole BAP publisher. */
    private int desiredInfoPhase;
    private long infoReturnDeadline;
    private int presentationGeneration;
    private static final long PRESENTATION_RETRY_MS = 500L;
    private static final long INFO_TIME_HOLD_MS = 20000L;

    private final com.luka.carplay.core.ScreenModule.ViewAreaModeListener viewAreaModeListener =
        new com.luka.carplay.core.ScreenModule.ViewAreaModeListener() {
            public void onViewAreaModeChanged(int mode) {
                requestViewAreaRefresh(mode);
            }
        };

    private final com.luka.carplay.core.ScreenModule.InfoModeListener infoModeListener =
        new com.luka.carplay.core.ScreenModule.InfoModeListener() {
            public void onInfoModeToggle() {
                requestInfoModeToggle();
            }
        };

    private final com.luka.carplay.cluster.ClusterLayerController.ViewportListener viewportListener =
        new com.luka.carplay.cluster.ClusterLayerController.ViewportListener() {
            public void onManeuverViewportChanged() {
                synchronized (presentationLock) {
                    if (!running) return;
                    rendererViewportRefreshPending = true;
                    presentationWake = true;
                    presentationLock.notifyAll();
                }
            }
        };

    private final BAPBridge.PresentationListener bapPresentationListener =
        new BAPBridge.PresentationListener() {
            public void onPresentationStateChanged() {
                requestPresentationCheck();
            }
        };

    /* Parsed state (reused to avoid allocations) */
    private State state = new State();

    /* ============================================================
     * State Container
     * ============================================================ */

    public static class State {
        /* Dirty flags (top-level) */
        public static final int DIRTY_ROUTE_STATE      = 1 << 0;
        public static final int DIRTY_MANEUVER_STATE   = 1 << 1;
        public static final int DIRTY_MANEUVER_COUNT   = 1 << 2;
        public static final int DIRTY_MANEUVER_LIST    = 1 << 3;
        public static final int DIRTY_DIST_DEST        = 1 << 4;
        public static final int DIRTY_DIST_MAN         = 1 << 7;
        public static final int DIRTY_ETA              = 1 << 10;
        public static final int DIRTY_TIME_REMAINING   = 1 << 11;
        public static final int DIRTY_CURRENT_ROAD     = 1 << 12;
        public static final int DIRTY_DESTINATION      = 1 << 13;
        public static final int DIRTY_DISCONNECT       = 1 << 14;
        public static final int DIRTY_MANEUVER_ICON    = 1 << 15;
        public static final int DIRTY_MANEUVER_TEXT    = 1 << 16;
        public static final int DIRTY_VISIBLE_IN_APP   = 1 << 17;
        public static final int DIRTY_SOURCE_SUPPORTS_RG = 1 << 20;
        public static final int DIRTY_LANE_GUIDANCE    = 1 << 21;

        public int dirtyMask = 0;

        /* Route */
        public int routeState = -1;
        public long routeGeneration = -1L;
        public int maneuverState = -1;
        public int maneuverCount = 0;
        public int[] maneuverOrder = null;
        public int visibleInApp = -1;
        public int sourceSupportsRg = -1;
        public int laneGuidanceShowing = -1;
        public int laneGuidanceIndex = -1;
        public int laneGuidanceSlot = -1;

        /* Distance to destination */
        public int distDestM = -1;

        /* Distance to maneuver */
        public int distManeuverM = -1;

        /* Time */
        public int etaSeconds = -1;
        public long timeRemainingSeconds = -1;
        public long timeRemainingSampleSeconds = -1L;

        /* Roads */
        public String currentRoad = null;
        public String destination = null;

        /* Disconnect reason */
        public String disconnectReason = null;

        /* Maneuvers */
        public int[] mType = new int[MAX_MANEUVERS];
        public int[] mTurnAngle = new int[MAX_MANEUVERS];
        /* Distinguish a received -1 degree value from the legacy empty-slot -1. */
        public boolean[] mTurnAnglePresent = new boolean[MAX_MANEUVERS];
        public int[] mJunctionType = new int[MAX_MANEUVERS];
        public int[] mDrivingSide = new int[MAX_MANEUVERS];
        public int[] mDistance = new int[MAX_MANEUVERS];
        public String[] mName = new String[MAX_MANEUVERS];
        public String[] mAfterRoad = new String[MAX_MANEUVERS];
        public String[] mExitInfo = new String[MAX_MANEUVERS];
        public int[][] mJunctionAngles = new int[MAX_MANEUVERS][];
        /*
         * Junction element exit angle in degrees.
         * MHI3 dio_manager uses sentinel 1000 when the field is missing.
         */
        public int[] mExitAngle = new int[MAX_MANEUVERS];
        public int[] mLinkedLaneGuidanceIndex = new int[MAX_MANEUVERS];
        public int[] mLinkedLaneGuidanceSlot = new int[MAX_MANEUVERS];
        /* Slot version: bumps in C hook when a different iOS maneuver is assigned to a slot.
         * Detects maneuver transitions even when type/angles are identical. */
        public int[] mVer = new int[MAX_MANEUVERS];

        public int[][] mLanePositions = new int[MAX_MANEUVERS][];
        public int[] mLaneCount = new int[MAX_MANEUVERS];
        public int[][] mLaneDirections = new int[MAX_MANEUVERS][];
        public int[][] mLaneStatus = new int[MAX_MANEUVERS][];
        /*
         * Per-lane angle vectors from 0x5204 lane informations.
         * Format on the bus: lane0 comma-list, lanes separated by '|'
         * Example: "1000|40,20|"
         */
        public int[][][] mLaneAngles = new int[MAX_MANEUVERS][][];

        /*
         * Lane-guidance cache keyed by iOS composedGuidanceEventIndex.
         * These lg* arrays are separate from maneuver slots: CarPlay can send
         * lane event 0 while maneuver slot 0 currently belongs to iOS
         * maneuver 2 after reconnect/reroute.
         */
        public int[] lgIndex = new int[MAX_MANEUVERS];
        public int[][] lgLanePositions = new int[MAX_MANEUVERS][];
        public int[] lgLaneCount = new int[MAX_MANEUVERS];
        public int[] lgLaneComplete = new int[MAX_MANEUVERS];
        public int[][] lgLaneDirections = new int[MAX_MANEUVERS][];
        public int[][] lgLaneStatus = new int[MAX_MANEUVERS][];
        public int[][][] lgLaneAngles = new int[MAX_MANEUVERS][][];

        public State() {
            reset();
        }

        public void reset() {
            routeState = -1;
            routeGeneration = -1L;
            maneuverState = -1;
            maneuverCount = 0;
            maneuverOrder = null;
            distDestM = -1;
            distManeuverM = -1;
            etaSeconds = -1;
            timeRemainingSeconds = -1;
            timeRemainingSampleSeconds = -1L;
            currentRoad = null;
            destination = null;
            disconnectReason = null;
            visibleInApp = -1;
            sourceSupportsRg = -1;
            clearLaneGuidance();
            clearAllManeuverSlots();
            clearDirty();
        }

        /* Reset per-maneuver and per-lane-cache arrays to their initial
         * empty values without touching dirty flags or top-level fields.
         * Shared between full reset() and the source_supports_rg=0
         * hard-clear path in parse(). */
        public void clearAllManeuverSlots() {
            for (int i = 0; i < MAX_MANEUVERS; i++) {
                clearManeuverSlot(i);
                clearLaneSlot(i);
            }
        }

        /** Empty one lane-guidance cache slot; true if it held anything. */
        public boolean clearLaneSlot(int i) {
            boolean had = lgIndex[i] != -1 || lgLaneCount[i] != -1 || lgLaneComplete[i] != -1
                || lgLanePositions[i] != null || lgLaneDirections[i] != null
                || lgLaneStatus[i] != null || lgLaneAngles[i] != null;
            lgIndex[i] = -1;
            lgLanePositions[i] = null;
            lgLaneCount[i] = -1;
            lgLaneComplete[i] = -1;
            lgLaneDirections[i] = null;
            lgLaneStatus[i] = null;
            lgLaneAngles[i] = null;
            return had;
        }

        /** Reset a reassigned maneuver without touching independent lane events. */
        public void clearManeuverSlot(int i) {
            mType[i] = -1;
            mTurnAngle[i] = -1;
            mTurnAnglePresent[i] = false;
            mJunctionType[i] = -1;
            mDrivingSide[i] = -1;
            mDistance[i] = -1;
            mName[i] = null;
            mAfterRoad[i] = null;
            mExitInfo[i] = null;
            mJunctionAngles[i] = null;
            mExitAngle[i] = 1000;
            mVer[i] = -1;
            mLinkedLaneGuidanceIndex[i] = -1;
            mLinkedLaneGuidanceSlot[i] = -1;
            mLanePositions[i] = null;
            mLaneCount[i] = -1;
            mLaneDirections[i] = null;
            mLaneStatus[i] = null;
            mLaneAngles[i] = null;
        }

        public void clearLaneGuidance() {
            laneGuidanceShowing = -1;
            laneGuidanceIndex = -1;
            laneGuidanceSlot = -1;
        }

        public void clearDirty() {
            dirtyMask = 0;
        }

        public void markDirty(int flag) {
            dirtyMask |= flag;
        }

        /** Re-publish the cached full snapshot after a late renderer connection/restart. */
        public void markAllDirtyForReplay() {
            dirtyMask |= DIRTY_ROUTE_STATE | DIRTY_MANEUVER_STATE | DIRTY_MANEUVER_COUNT
                | DIRTY_MANEUVER_LIST | DIRTY_DIST_DEST | DIRTY_DIST_MAN | DIRTY_ETA
                | DIRTY_TIME_REMAINING | DIRTY_CURRENT_ROAD | DIRTY_DESTINATION
                | DIRTY_MANEUVER_ICON | DIRTY_MANEUVER_TEXT | DIRTY_VISIBLE_IN_APP
                | DIRTY_SOURCE_SUPPORTS_RG | DIRTY_LANE_GUIDANCE;
        }

    }

    /* ============================================================
     * Lifecycle
     * ============================================================ */

    /**
     * Initialize with BAP service.
     */
    public boolean init(Object naviService) {
        bap = new BAPBridge();
        if (!bap.init(naviService)) {
            Log.e(TAG, "BAPBridge init failed");
            bap = null;
            return false;
        }

        Log.i(TAG, "Initialized");
        return true;
    }

    /**
     * Start route guidance.
     */
    public void start() {
        if (running) return;
        synchronized (this) {
            running = true;
            rgActive = false;
            presentationConfirmed = false;
            hasRouteUpdate = false;
            routeWantsActive = false;
            state.reset();
        }
        /* stop() clears the static listeners so a restarted module never retains a
         * stale RouteGuidance instance; start() installs the session inputs. */
        com.luka.carplay.core.ScreenModule.setViewAreaModeListener(viewAreaModeListener);
        com.luka.carplay.core.ScreenModule.setInfoModeListener(infoModeListener);
        com.luka.carplay.cluster.ClusterLayerController.setViewportListener(viewportListener);
        if (bap != null) bap.setPresentationListener(bapPresentationListener);
        final int generation;
        synchronized (presentationLock) {
            generation = ++presentationGeneration;
            presentationWake = false;
            presentationDrivePending = false;
            infoPresentationRefreshPending = false;
            rendererViewportRefreshPending = false;
            desiredInfoPhase = 0;
            infoReturnDeadline = 0L;
            presentationThread = new Thread(new Runnable() {
                public void run() { presentationLoop(generation); }
            }, "carplay-rgi-presentation");
            presentationThread.setDaemon(true);
            presentationThread.start();
        }
        com.luka.carplay.core.ScreenModule.setNavActive(false);

        CarplayBus bus = CarplayBus.getInstance();
        bus.on(CarplayBus.EVT_RGD_UPDATE, this);
        bus.start();   /* idempotent */

        /* Cold-boot catch-up: when the phone is already connected, the hook
         * may have sent its automatic sticky snapshot before this listener
         * existed and CarplayBus correctly dropped it.  Request the same
         * snapshot again now that EVT_RGD_UPDATE has a consumer.  If the hook
         * is not connected yet this send simply fails; its normal connect-time
         * replay will arrive later, after the listener is already installed. */
        boolean syncRequested = bus.send(CarplayBus.CMD_SYNC_REQ, 0, null, 0);

        Log.i(TAG, "Started (subscribed to EVT_RGD_UPDATE, sync=" + syncRequested + ")");
    }

    /**
     * Stop route guidance.
     */
    public void stop() {
        if (!running) return;
        CarplayBus.getInstance().off(CarplayBus.EVT_RGD_UPDATE);
        Thread worker;
        synchronized (presentationLock) {
            running = false;
            presentationGeneration++;
            presentationWake = true;
            presentationDrivePending = false;
            infoPresentationRefreshPending = false;
            rendererViewportRefreshPending = false;
            desiredInfoPhase = 0;
            infoReturnDeadline = 0L;
            presentationLock.notifyAll();
            worker = presentationThread;
            presentationThread = null;
        }
        synchronized (this) {
            rgActive = false;
            presentationConfirmed = false;
            hasRouteUpdate = false;
            routeWantsActive = false;
            com.luka.carplay.core.ScreenModule.setNavActive(false);
            if (bap != null) {
                bap.onStop();
                bap.onShutdown();
                bap.setPresentationListener(null);
            }
        }
        com.luka.carplay.core.ScreenModule.clearViewAreaModeListener(viewAreaModeListener);
        com.luka.carplay.core.ScreenModule.clearInfoModeListener(infoModeListener);
        com.luka.carplay.cluster.ClusterLayerController.clearViewportListener(viewportListener);
        if (worker != null && worker != Thread.currentThread()) {
            try { worker.join(1000L); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        }

        Log.i(TAG, "Stopped");
    }

    public boolean isRunning() {
        return running;
    }

    /** REPLACE mode: cancel stock nav + shut RG gate for the whole CarPlay session
     *  (called on connect, not on CarPlay-nav start).  No-op if not initialized. */
    /** @return true once the RG gate is actually shut (else caller retries). */
    public boolean engageTakeover()    { return bap != null && bap.engageTakeover(); }
    public void    disengageTakeover() { if (bap != null) bap.disengageTakeover(); }

    /* ============================================================
     * CarplayBus Listener
     * ============================================================ */

    public synchronized void onFrame(int type, int flags, byte[] payload, int len) {
        if (!running) return;
        if (type != CarplayBus.EVT_RGD_UPDATE) return;
        /* Parse into state (delta) */
        parse(CarplayBus.parseText(payload, len));
        if (state.dirtyMask == 0) {
            if (rgActive && !presentationConfirmed) requestPresentationCheck();
            return;
        }

        /* Check for disconnect */
        if (state.disconnectReason != null) {
            resetInfoPresentation();
            if (bap != null) { bap.onStop(); bap.onShutdown(); }
            rgActive = false;
            presentationConfirmed = false;
            routeWantsActive = false;
            com.luka.carplay.core.ScreenModule.setNavActive(false);  /* hide layers (stock ctx 74) */
            hasRouteUpdate = false;
            state.reset();
            return;
        }

        /*
         * Start/stop gating:
         * - Prefer explicit active authority from visible_in_app (native-like TBT_Active semantics).
         * - Fall back to route/maneuver heuristics only when visible_in_app is unknown.
         * - iOS can emit metadata-only bus updates (component_ids/current_road/etc) even when not navigating.
         *
         * Only change RG active state when we received an activation-relevant delta.
         */
        int actMask = State.DIRTY_ROUTE_STATE
            | State.DIRTY_MANEUVER_STATE
            | State.DIRTY_MANEUVER_COUNT
            | State.DIRTY_MANEUVER_LIST
            | State.DIRTY_MANEUVER_ICON
            | State.DIRTY_VISIBLE_IN_APP
            | State.DIRTY_SOURCE_SUPPORTS_RG;
        boolean hasActivationDelta = (state.dirtyMask & actMask) != 0;

        if (hasActivationDelta) {
            boolean explicitClear = ((state.dirtyMask & State.DIRTY_MANEUVER_COUNT) != 0)
                && (state.maneuverCount == 0);

            /*
             * visible_in_app is iAP2 RouteGuidanceUpdate TLV 0x0F.  Verified against Apple's own
             * source (accessoryd 23G71, +[ACCNavigationRouteGuidanceUpdateInfo keyForType:] case 0xF):
             * it is ACCNav_RGUpdate_RouteGuidanceBeingShownInApp -- whether the nav app's guidance UI
             * is currently ON SCREEN, NOT whether a route is active.  iOS sends it 0 for third-party
             * maps and whenever the nav app isn't the foreground CarPlay app, even mid-route, so it
             * must NOT override an otherwise-active route (issue #8: 3rd-party nav on iOS 26+ showed
             * cached info then vanished within a second).  Route activity comes from the separate
             * fields RouteGuidanceState / ManeuverCount / maneuver list.
             */
            boolean routeLooksActive = (state.routeState >= ROUTE_STATE_ROUTE_SET)
                || (state.maneuverCount > 0)
                || (state.maneuverOrder != null && state.maneuverOrder.length > 0);
            boolean hasActiveAuthority = (state.visibleInApp >= 0);
            boolean wantActive;
            if (hasActiveAuthority) {
                /* being-shown-in-app=1 activates; being-shown-in-app=0 only deactivates when the
                 * route itself no longer looks active (real end-of-route is still caught by the
                 * routeState==NO_ROUTE_SET hard override below). */
                wantActive = (state.visibleInApp != 0) || routeLooksActive;
            } else {
                wantActive = routeLooksActive;
                if (explicitClear && state.routeState < ROUTE_STATE_ROUTE_SET) {
                    wantActive = false;
                }
            }
            if (state.sourceSupportsRg == 0) wantActive = false;

            /* route_state=0 (NO_ROUTE_SET) is authoritative -- no route means
             * nothing to show, regardless of stale visible_in_app from C hook. */
            if (state.routeState == ROUTE_STATE_NO_ROUTE_SET) wantActive = false;
            if (!wantActive && routeWantsActive) resetInfoPresentation();
            if (!wantActive) bapStartPendingLogged = false;
            routeWantsActive = wantActive;

            /* Re-check running: a frame captured by the bus dispatcher just before stop()
             * must not resurrect the bridge (bap.onStart re-shuts the gate + relaunches the
             * renderer) after disconnect already tore it down.  running is volatile and stop()
             * clears it before bap teardown. */
            if (!running) { state.clearDirty(); return; }

            if (wantActive && !rgActive) {
                Log.i(TAG, "RG activate: route_state=" + state.routeState
                    + " maneuver_count=" + state.maneuverCount
                    + " visible_in_app=" + state.visibleInApp
                    + " source_supports_rg=" + state.sourceSupportsRg);
                /* ctx 80 (black backing + maneuver plane) and the Maps /map URL follow the BAP
                 * start itself: that is the edge on which the Kombi animates its KDK slot in, so
                 * waiting for FRAME_READY only left the slot empty next to Maps' own turn card
                 * (seen 2026-09-21).  presentationConfirmed tracks renderer/BAP readiness for
                 * the text hold and position scroll only, not the context. */
                presentationConfirmed = false;
                rgActive = bap != null && bap.onStart();
                com.luka.carplay.core.ScreenModule.setNavActive(rgActive);
                if (!rgActive) {
                    if (!bapStartPendingLogged) {
                        Log.w(TAG, "RG activation pending: BAP start did not complete; keeping ctx 74");
                        bapStartPendingLogged = true;
                    }
                    requestPresentationCheck();
                } else {
                    bapStartPendingLogged = false;
                }
            } else if (!wantActive && rgActive) {
                Log.i(TAG, "RG deactivate: route_state=" + state.routeState
                    + " maneuver_count=" + state.maneuverCount
                    + " visible_in_app=" + state.visibleInApp
                    + " source_supports_rg=" + state.sourceSupportsRg);
                /* With C hook debounce, transient route_state=0 never reaches
                 * Java — any deactivation here is genuine (source_supports_rg=0,
                 * visibleInApp=0, or real route end).  End route; preserve pixels until VC withdraws KDK visibility. */
                if (bap != null) { bap.onStop(); bap.onRouteEnd(); }
                rgActive = false;
                presentationConfirmed = false;
                /* RGI off -> hide the CarPlay cluster layers (stock ctx 74). */
                com.luka.carplay.core.ScreenModule.setNavActive(false);
            }
        }

        if (!rgActive) {
            state.clearDirty();
            return;
        }

        /* Send only while RG is active to avoid fighting native BAP when inactive */
        if (bap != null) {
            // Text sent before context 80 was visible must still receive its
            // complete first hold when a live delta confirms the presentation.
            if (!presentationConfirmed || !bap.isPresentationReady()) bap.suspendPositionScroll();
            boolean updatePublished = bap.update(state);
            boolean rendererAndBapReady = bap.isPresentationReady();
            /* First entry to ctx 80 requires THIS cached/current RGI update to have completed.
             * After that, an unrelated recoverable BAP delta error must not flicker the context;
             * renderer link loss still drops readiness immediately on the next update. */
            boolean confirmed = rendererAndBapReady && (presentationConfirmed || updatePublished);
            if (confirmed != presentationConfirmed) {
                presentationConfirmed = confirmed;
                if (confirmed) {
                    Log.i(TAG, "RG presentation CONFIRMED: BAP snapshot published + popup frame ready");
                } else {
                    Log.w(TAG, "RG presentation lost: keeping route state and ctx 80");
                }
            }
            if (updatePublished) state.clearDirty();
            if (bap.takePositionScrollChange() || !confirmed) wakePositionScroll();
            if (!confirmed || !updatePublished) requestPresentationCheck();
        }
    }

    private void requestPresentationCheck() {
        synchronized (presentationLock) {
            if (!running) return;
            presentationDrivePending = true;
            presentationWake = true;
            presentationLock.notifyAll();
        }
    }

    /**
     * View is a hard presentation boundary: reset to phase 0 and immediately
     * replay the cached real position/ETA on the serialized worker.
     */
    private void requestViewAreaRefresh(int mode) {
        synchronized (presentationLock) {
            if (!running) return;
            desiredInfoPhase = 0;
            infoReturnDeadline = 0L;
            infoPresentationRefreshPending = true;
            rendererViewportRefreshPending = true;
            presentationWake = true;
            presentationLock.notifyAll();
        }
        Log.i(TAG, "viewArea info phase reset=0 queued: "
            + (mode == com.luka.carplay.core.ScreenModule.VIEWAREA_SMALLSCREEN ? "smallscreen" : "fullscreen"));
    }

    /** Steering-wheel encoder press: toggle phase without touching the HMI/BAP caller thread.
     *  Not synchronized: it runs on the DSI key callback and must not wait for a BAP update
     *  that holds the route monitor; it reads only volatile flags and queues under presentationLock. */
    private void requestInfoModeToggle() {
        /* Same phase toggle in both views, with all text in FctID 19:
         * smallscreen shows U+25CC + duration, fullscreen shows arrival + duration.
         * FctID 20 stays empty. The old smallscreen branch
         * flipped FctID 22's timeInfoType instead, which the cluster cannot render -
         * it has no remaining-duration widget - so it only blanked the clock. */
        if (!running || !rgActive) {
            Log.i(TAG, "route-info toggle ignored without active RGI");
            return;
        }
        int phase;
        synchronized (presentationLock) {
            if (!running) return;
            desiredInfoPhase ^= 1;
            infoReturnDeadline = 0L;
            phase = desiredInfoPhase;
            infoPresentationRefreshPending = true;
            presentationWake = true;
            presentationLock.notifyAll();
        }
        Log.i(TAG, "route-info phase=" + phase + " queued");
    }

    /** Called with the route monitor held, before ending its BAP session. */
    private void resetInfoPresentation() {
        synchronized (presentationLock) {
            desiredInfoPhase = 0;
            infoReturnDeadline = 0L;
            infoPresentationRefreshPending = false;
        }
    }

    /** Called with presentationLock held. No polling while road text is selected. */
    private long infoReturnWait(long now) {
        if (desiredInfoPhase != 1 || infoReturnDeadline == 0L) return -1L;
        long wait = infoReturnDeadline - now;
        // HU wall-clock correction must not leave Time selected for hours.
        if (wait > INFO_TIME_HOLD_MS) {
            infoReturnDeadline = now + INFO_TIME_HOLD_MS;
            wait = INFO_TIME_HOLD_MS;
        }
        return wait > 0L ? wait : 0L;
    }

    private void wakePositionScroll() {
        synchronized (presentationLock) {
            presentationWake = true;
            presentationLock.notifyAll();
        }
    }

    /** No lock inversion: never take RouteGuidance.this inside presentationLock. */
    private synchronized long drivePositionScroll() {
        if (!running || bap == null) return -1L;
        if (!rgActive || !presentationConfirmed || !bap.isPresentationReady()) {
            bap.suspendPositionScroll();
            return -1L;
        }
        long now = System.currentTimeMillis();
        bap.tickPositionScroll(now);
        return bap.positionScrollWait(now);
    }

    private void presentationLoop(int generation) {
        long retryAt = 0L;
        long textWait = 0L;
        boolean retryPresentation = false, retryViewport = false, retryInfo = false;
        while (true) {
            synchronized (presentationLock) {
                if (!running || generation != presentationGeneration) return;
                long now = System.currentTimeMillis();
                long retryWait = retryAt == 0L ? -1L : retryAt - now;
                // Wall-clock changes must not strand recovery for hours.
                if (retryWait > PRESENTATION_RETRY_MS) retryWait = PRESENTATION_RETRY_MS;
                long wait = textWait;
                long infoWait = infoReturnWait(now);
                if (infoWait >= 0L && (wait < 0L || infoWait < wait)) wait = infoWait;
                if (retryWait >= 0L && (wait < 0L || retryWait < wait)) wait = retryWait;
                if (retryAt != 0L && retryWait <= 0L) wait = 0L;
                if (running && generation == presentationGeneration && !presentationWake && wait != 0L) {
                    try {
                        if (wait < 0L) presentationLock.wait();
                        else presentationLock.wait(wait);
                    } catch (InterruptedException e) { Thread.currentThread().interrupt(); return; }
                }
                if (!running || generation != presentationGeneration) return;
            }

            // Never acquire the route monitor while holding presentationLock.
            // Recheck generation AFTER acquiring it: stop/start may have retired
            // this worker while it waited for an onFrame publication to finish.
            synchronized (this) {
                boolean drivePresentation, refreshViewport;
                int infoPhase = -1;
                synchronized (presentationLock) {
                    if (!running || generation != presentationGeneration) return;
                    presentationWake = false;
                    long now = System.currentTimeMillis();
                    if (infoReturnWait(now) == 0L) {
                        desiredInfoPhase = 0;
                        infoReturnDeadline = 0L;
                        infoPresentationRefreshPending = true;
                    }
                    boolean retryDue = retryAt != 0L && (now >= retryAt || retryAt - now > PRESENTATION_RETRY_MS);
                    drivePresentation = presentationDrivePending || (retryDue && retryPresentation);
                    presentationDrivePending = false;
                    refreshViewport = rendererViewportRefreshPending || (retryDue && retryViewport);
                    rendererViewportRefreshPending = false;
                    if (infoPresentationRefreshPending || (retryDue && retryInfo)) {
                        // Read the latest requested mode, including route-end/View
                        // resets. Never retain a failed old mode in the retry timer.
                        infoPhase = desiredInfoPhase;
                        infoPresentationRefreshPending = false;
                    }
                    if (retryDue) retryAt = 0L;
                }

                if (infoPhase >= 0) retryInfo = !driveInfoPresentation(infoPhase);
                if (refreshViewport) retryViewport = !driveRendererViewport();
                if (drivePresentation) retryPresentation = driveCachedPresentation();
                // Apply View/OK/readiness changes before an already-due text step.
                textWait = drivePositionScroll();
                if (retryPresentation || retryViewport || retryInfo) {
                    if (retryAt == 0L) retryAt = System.currentTimeMillis() + PRESENTATION_RETRY_MS;
                    // A failed replay can notify its own readiness callback. Coalesce
                    // only that replay request; preserve View/OK and viewport events.
                    synchronized (presentationLock) {
                        if (retryPresentation) presentationDrivePending = false;
                        presentationWake = presentationDrivePending || infoPresentationRefreshPending
                            || rendererViewportRefreshPending;
                    }
                } else retryAt = 0L;
            }
        }
    }

    /** Serialized with maneuver updates; retries backpressure without replaying BAP. */
    private synchronized boolean driveRendererViewport() {
        if (!running || bap == null || !rgActive) return true;
        return bap.refreshRendererViewport();
    }

    /** Serialized with onFrame/stop; publishes cached FctID 19/20/22 immediately. */
    private synchronized boolean driveInfoPresentation(int phase) {
        if (!running || bap == null || !rgActive) return true;
        boolean applied = bap.refreshInfoPresentation(state, phase);
        if (applied && phase == 1) {
            synchronized (presentationLock) {
                // Arm once after publication. Deltas/recovery retries do not
                // extend Time; View or a newer OK request supersedes this one.
                if (desiredInfoPhase == 1 && !infoPresentationRefreshPending && infoReturnDeadline == 0L)
                    infoReturnDeadline = System.currentTimeMillis() + INFO_TIME_HOLD_MS;
            }
        }
        return applied;
    }

    /** Serialized with onFrame/stop so cached State and BAP are never mutated concurrently. */
    private synchronized boolean driveCachedPresentation() {
        if (!running || bap == null || !routeWantsActive) return false;

        if (!rgActive) {
            rgActive = bap.onStart();
            if (!rgActive) return true;
            com.luka.carplay.core.ScreenModule.setNavActive(true);
        }

        // FRAME_READY can win the race with the worker's first wake. Rebase
        // hidden text even if the worker never observed an unready renderer.
        if (!presentationConfirmed || !bap.isPresentationReady()) bap.suspendPositionScroll();

        if (!bap.preparePresentation()) {
            if (presentationConfirmed) {
                presentationConfirmed = false;
                Log.w(TAG, "RG presentation lost: renderer not ready; keeping ctx 80");
            }
            return true;
        }

        /* The primer proved the current renderer can swap. Now replay the cached RGI snapshot so
         * displayable 98 contains the real current maneuver before context 80 is exposed. */
        state.markAllDirtyForReplay();
        boolean published = bap.update(state);
        if (published) state.clearDirty();
        boolean confirmed = bap.isPresentationReady() && (presentationConfirmed || published);
        if (confirmed != presentationConfirmed) {
            presentationConfirmed = confirmed;
            if (confirmed) Log.i(TAG, "RG presentation CONFIRMED from cached snapshot");
        }
        return !confirmed || !published;
    }

    /* ============================================================
     * Parsing
     * ============================================================ */

    /* Any of these makes a frame a route update (hasRouteUpdate). */
    private static final String[] ROUTE_UPDATE_KEYS = {
        "route_state", "maneuver_state", "maneuver_count", "maneuver_list", "dist_dest_m",
        "dist_maneuver_m", "eta_seconds", "time_remaining_seconds", "current_road", "destination",
        "visible_in_app", "component_ids", "component_count", "source_supports_rg"
    };

    private void parse(CarplayBus.Data d) {
        // Dirty means pending publication, not just changed in this frame.
        // Keep failed HUD/renderer updates across subsequent distance-only deltas.
        if (d.has("route_generation")) {
            long generation = d.num64("route_generation", -1L);
            if (generation >= 0L && generation != state.routeGeneration) {
                // Native resets can be hidden by debounce and reuse every slot
                // version. Reset content before applying this generation's fields,
                // retaining route authority until the source updates it below.
                state.routeGeneration = generation;
                state.clearAllManeuverSlots();
                state.maneuverState = -1;
                state.maneuverOrder = null;
                state.maneuverCount = 0;
                state.distManeuverM = -1;
                state.distDestM = -1;
                state.etaSeconds = -1;
                state.timeRemainingSeconds = -1;
                state.timeRemainingSampleSeconds = -1L;
                state.currentRoad = null;
                state.destination = null;
                state.clearLaneGuidance();
                state.markAllDirtyForReplay();
            }
        }
        for (int k = 0; k < ROUTE_UPDATE_KEYS.length && !hasRouteUpdate; k++) {
            if (d.has(ROUTE_UPDATE_KEYS[k])) hasRouteUpdate = true;
        }

        if (d.has("disconnect_reason")) {
            String v = d.str("disconnect_reason");
            if (!strEq(state.disconnectReason, v)) {
                state.disconnectReason = v;
                state.markDirty(State.DIRTY_DISCONNECT);
            }
        } else if (state.disconnectReason != null && d.size() > 0) {
            /* One-shot field: clear on any subsequent non-empty update */
            state.disconnectReason = null;
            state.markDirty(State.DIRTY_DISCONNECT);
        }
        if (d.has("source_supports_rg")) {
            int v = d.num("source_supports_rg", -1);
            if (v != state.sourceSupportsRg) {
                state.sourceSupportsRg = v;
                state.markDirty(State.DIRTY_SOURCE_SUPPORTS_RG);
                /*
                 * MHI3 (dio_manager) uses the "source supports route guidance" signal to suppress
                 * route guidance updates. For our VC/HUD bridge, treat support==0 as a hard clear
                 * to prevent stale icons/distances from lingering.
                 */
                if (v == 0) {
                    /* support=0 is the authoritative hard-clear edge even if an earlier transient
                     * route_state=0 already changed routeState.  Never let that transient suppress
                     * clearing cached slots/lane data. */
                    clearRoute();
                    state.clearAllManeuverSlots();
                }
            }
        }
        if (d.has("route_state")) {
            int v = d.num("route_state", -1);
            /*
             * route_state=0 handling.
             *
             * iOS sends transient route_state=0 during route setup and periodically
             * during active navigation (~every 30s).  A full per-slot clear here
             * causes permanent data loss because iOS doesn't resend 0x5202 for
             * already-sent maneuver indices, and it causes visible flicker on the
             * cluster (NO_SYMBOL + distance=-1 for ~1s each cycle).
             *
             * Only do a full per-slot clear when we have positive evidence of a
             * genuine route end: sourceSupportsRg=0 or disconnect_reason set.
             * Transient route_state=0 only resets count and order so BAPBridge
             * can suppress maneuver output without losing cached data.
             */
            if (v == 0) {
                clearRoute();
                /* Per-slot data (mType, mTurnAngle, etc.) is NOT cleared here.
                 * It is preserved so that when count>0 returns, BAPBridge can
                 * immediately display the correct maneuver icons.
                 * Full per-slot clear only happens on sourceSupportsRg=0 or
                 * disconnect_reason (handled elsewhere). */
            } else if (v > 0) {
                if (v != state.routeState) {
                    state.routeState = v;
                    state.markDirty(State.DIRTY_ROUTE_STATE);
                }
            }
        }
        if (d.has("maneuver_state")) {
            int v = d.num("maneuver_state", -1);
            /* MHI3 EManeuverState: 0..3 are valid (CONTINUE/INITIAL/PREPARE/EXECUTE). */
            if (v >= 0 && v <= 3 && v != state.maneuverState) {
                state.maneuverState = v;
                state.markDirty(State.DIRTY_MANEUVER_STATE);
            }
        }
        if (d.has("maneuver_count")) {
            int v = d.num("maneuver_count", 0);
            if (v != state.maneuverCount) {
                state.maneuverCount = v;
                state.markDirty(State.DIRTY_MANEUVER_COUNT);
                /*
                 * Explicit clear: iOS (and injected test frames) can send maneuver_count=0 without
                 * sending maneuver_list. Treat that as a real clear and drop cached order,
                 * otherwise BAPBridge may keep re-sending stale maneuver icons.
                 */
                if (v == 0) {
                    state.maneuverOrder = null;
                    state.markDirty(State.DIRTY_MANEUVER_LIST);
                    /*
                     * Don't clear per-slot data (mType, mTurnAngle, etc.) here.
                     *
                     * The slot data was written by the C hook from real 0x5202 ManeuverUpdate
                     * messages and corresponds to valid maneuver info.  Bus deltas only report
                     * changes, so if we clear Java state, the bus won't re-send unchanged values,
                     * and we lose the data permanently until a new 0x5202 arrives.
                     *
                     * maneuver_count=0 can be transient - iOS briefly clears the count before
                     * re-populating (observed at route start).  By preserving slot data we avoid
                     * the race where maneuver_list advances to slots whose data was cleared.
                     *
                     * Full per-slot clear happens only on:
                     *   - source_supports_rg=0  (hard reset)
                     *   - a new route_generation  (clearAllManeuverSlots)
                     *   - a disconnect reason
                     * route_state=0 only resets count and order (see its handler).
                     */
                }
            }
        }
        if (d.has("maneuver_list")) {
            /*
             * MHI3 behaviour:
             * - maneuver_list missing -> keep previous (null or last value)
             * - maneuver_list present but empty -> treat as an explicit empty list
             */
            String raw = d.str("maneuver_list", null);
            int[] v;
            if (raw == null) {
                v = null;
            } else if (raw.length() == 0) {
                v = new int[0];
            } else {
                v = d.intList("maneuver_list");
            }
            if (!intArrayEq(state.maneuverOrder, v)) {
                state.maneuverOrder = v;
                state.markDirty(State.DIRTY_MANEUVER_LIST);
            }
        }
        if (d.has("visible_in_app")) {
            int raw = d.num("visible_in_app", -1);
            int v = (raw == 0 || raw == 1) ? raw : -1;
            if (v != state.visibleInApp) {
                state.visibleInApp = v;
                state.markDirty(State.DIRTY_VISIBLE_IN_APP);
            }
        }
        int showing = intOr(d, "lane_guidance_showing", state.laneGuidanceShowing);
        int index = intOr(d, "lane_guidance_index", state.laneGuidanceIndex);
        int slot = intOr(d, "lane_guidance_slot", state.laneGuidanceSlot);
        if (showing != state.laneGuidanceShowing
                || index != state.laneGuidanceIndex || slot != state.laneGuidanceSlot) {
            state.laneGuidanceShowing = showing;
            state.laneGuidanceIndex = index;
            state.laneGuidanceSlot = slot;
            state.markDirty(State.DIRTY_LANE_GUIDANCE);
        }

        int distDest = intOr(d, "dist_dest_m", state.distDestM);
        if (distDest != state.distDestM) {
            state.distDestM = distDest;
            state.markDirty(State.DIRTY_DIST_DEST);
        }
        int distManeuver = intOr(d, "dist_maneuver_m", state.distManeuverM);
        if (distManeuver != state.distManeuverM) {
            state.distManeuverM = distManeuver;
            state.markDirty(State.DIRTY_DIST_MAN);
        }
        int eta = intOr(d, "eta_seconds", state.etaSeconds);
        if (eta != state.etaSeconds) {
            state.etaSeconds = eta;
            state.markDirty(State.DIRTY_ETA);
        }
        if (d.has("time_remaining_seconds")) {
            long v = d.num64("time_remaining_seconds", -1);
            if (v != state.timeRemainingSeconds) {
                state.timeRemainingSeconds = v;
                state.timeRemainingSampleSeconds = v >= 0L ? BAPBridge.getUtcMillis() / 1000L : -1L;
                state.markDirty(State.DIRTY_TIME_REMAINING);
            }
        }
        String road = strOr(d, "current_road", state.currentRoad);
        if (!strEq(road, state.currentRoad)) {
            state.currentRoad = road;
            state.markDirty(State.DIRTY_CURRENT_ROAD);
        }
        String destination = strOr(d, "destination", state.destination);
        if (!strEq(destination, state.destination)) {
            state.destination = destination;
            state.markDirty(State.DIRTY_DESTINATION);
        }

        /* The slots this frame carries: lane-guidance cache lg<i>_ and maneuvers m<i>_. */
        boolean[] lgSlots = new boolean[MAX_MANEUVERS], mSlots = new boolean[MAX_MANEUVERS];
        for (int k = 0; k < d.size(); k++) {
            markSlot(d.key(k), "lg", lgSlots);
            markSlot(d.key(k), "m", mSlots);
        }
        /* lane_slots names every lane-guidance slot that holds lanes; the rest are empty.
         * Each snapshot carries it, so a frame lost on the bus cannot strand stale lanes. */
        if (d.has("lane_slots")) {
            long held = d.num64("lane_slots", 0);
            for (int i = 0; i < MAX_MANEUVERS; i++)
                if ((held & (1L << i)) == 0 && state.clearLaneSlot(i))
                    state.markDirty(State.DIRTY_LANE_GUIDANCE);
        }
        for (int i = 0; i < MAX_MANEUVERS; i++) {
            if (!lgSlots[i]) continue;
            String p = "lg" + i + "_";
            boolean lanes = false;
            if (setInt(d, p + "index", state.lgIndex, i)) {
                state.lgLaneCount[i] = -1;
                state.lgLaneComplete[i] = -1;
                state.lgLanePositions[i] = null;
                state.lgLaneDirections[i] = null;
                state.lgLaneStatus[i] = null;
                state.lgLaneAngles[i] = null;
                lanes = true;
            }
            lanes |= setInt(d, p + "lane_count", state.lgLaneCount, i);
            lanes |= setInt(d, p + "lane_complete", state.lgLaneComplete, i);
            lanes |= setList(d, p + "lane_positions", state.lgLanePositions, i);
            lanes |= setList(d, p + "lane_directions", state.lgLaneDirections, i);
            lanes |= setList(d, p + "lane_status", state.lgLaneStatus, i);
            lanes |= setAngles(d, p + "lane_angles", state.lgLaneAngles, i);
            if (lanes) state.markDirty(State.DIRTY_LANE_GUIDANCE);
        }

        /* MHI3 gating: discard all per-slot maneuver data while there is no route update yet, or
         * the maneuver list is explicitly empty.  It gates every key of a slot, new ones included. */
        if (!acceptsManeuverData()) return;
        for (int i = 0; i < MAX_MANEUVERS; i++) {
            if (!mSlots[i]) continue;
            String p = "m" + i + "_";
            if (d.has(p + "ver")) {
                int v = d.num(p + "ver", -1);
                if (v >= 0 && v != state.mVer[i]) {
                    state.clearManeuverSlot(i);
                    state.mVer[i] = v;
                    state.markDirty(State.DIRTY_MANEUVER_ICON | State.DIRTY_MANEUVER_TEXT
                        | State.DIRTY_LANE_GUIDANCE);
                }
            }

            boolean icon = setInt(d, p + "type", state.mType, i);
            icon |= setInt(d, p + "junction_type", state.mJunctionType, i);
            icon |= setInt(d, p + "driving_side", state.mDrivingSide, i);
            icon |= setList(d, p + "junction_angles", state.mJunctionAngles, i);   /* side streets */
            if (d.has(p + "exit_angle")) {
                int v = d.num(p + "exit_angle", 1000);
                if (v != state.mExitAngle[i] || v != state.mTurnAngle[i] || !state.mTurnAnglePresent[i]) {
                    state.mExitAngle[i] = v;
                    state.mTurnAngle[i] = v;
                    state.mTurnAnglePresent[i] = true;
                    icon = true;
                }
            }
            if (icon) state.markDirty(State.DIRTY_MANEUVER_ICON);

            /* MHI3 computes the bargraph in the maneuver-update path (tbt_renderer
             * updateCurrentManeuver) from distanceBetweenManeuver: refresh distance and bargraph. */
            if (setInt(d, p + "distance", state.mDistance, i)) state.markDirty(State.DIRTY_DIST_MAN);

            boolean text = setStr(d, p + "name", state.mName, i);
            text |= setStr(d, p + "after_road", state.mAfterRoad, i);
            text |= setStr(d, p + "exit_info", state.mExitInfo, i);
            if (text) state.markDirty(State.DIRTY_MANEUVER_TEXT);

            boolean lanes = setInt(d, p + "lane_count", state.mLaneCount, i);
            lanes |= setInt(d, p + "linked_lane_guidance_index", state.mLinkedLaneGuidanceIndex, i);
            lanes |= setInt(d, p + "linked_lane_guidance_slot", state.mLinkedLaneGuidanceSlot, i);
            lanes |= setList(d, p + "lane_positions", state.mLanePositions, i);
            lanes |= setList(d, p + "lane_directions", state.mLaneDirections, i);
            lanes |= setList(d, p + "lane_status", state.mLaneStatus, i);
            lanes |= setAngles(d, p + "lane_angles", state.mLaneAngles, i);
            if (lanes) state.markDirty(State.DIRTY_LANE_GUIDANCE);
        }
    }

    /* No route: zero state/count, empty maneuver list and lane guidance.  Per-slot data stays. */
    private void clearRoute() {
        if (state.routeState != ROUTE_STATE_NO_ROUTE_SET) {
            state.routeState = ROUTE_STATE_NO_ROUTE_SET;
            state.markDirty(State.DIRTY_ROUTE_STATE);
        }
        if (state.maneuverCount != 0) {
            state.maneuverCount = 0;
            state.markDirty(State.DIRTY_MANEUVER_COUNT);
        }
        state.maneuverOrder = null;
        state.clearLaneGuidance();
        state.markDirty(State.DIRTY_MANEUVER_LIST | State.DIRTY_MANEUVER_ICON | State.DIRTY_MANEUVER_TEXT
            | State.DIRTY_LANE_GUIDANCE);
    }

    /* A field of the frame, or the value it already has when the frame does not carry it. */
    private static int intOr(CarplayBus.Data d, String key, int current) {
        return d.has(key) ? d.num(key, -1) : current;
    }

    private static String strOr(CarplayBus.Data d, String key, String current) {
        return d.has(key) ? d.str(key) : current;
    }

    /* Slot setters: the frame's value for key into a[i]; true when it changed. */
    private static boolean setInt(CarplayBus.Data d, String key, int[] a, int i) {
        int v = intOr(d, key, a[i]);
        if (v == a[i]) return false;
        a[i] = v;
        return true;
    }

    private static boolean setStr(CarplayBus.Data d, String key, String[] a, int i) {
        String v = strOr(d, key, a[i]);
        if (strEq(v, a[i])) return false;
        a[i] = v;
        return true;
    }

    private static boolean setList(CarplayBus.Data d, String key, int[][] a, int i) {
        if (!d.has(key)) return false;
        int[] v = d.intList(key);
        if (intArrayEq(a[i], v)) return false;
        a[i] = v;
        return true;
    }

    private static boolean setAngles(CarplayBus.Data d, String key, int[][][] a, int i) {
        if (!d.has(key)) return false;
        int[][] v = parseLaneAnglesMatrix(d.str(key, null));
        if (intMatrixEq(a[i], v)) return false;
        a[i] = v;
        return true;
    }

    /* Marks slot i when key is prefix, the decimal i (below slots.length) and '_'. */
    private static void markSlot(String key, String prefix, boolean[] slots) {
        if (!key.startsWith(prefix)) return;
        int i = 0, at = prefix.length();
        while (at < key.length() && i < slots.length && key.charAt(at) >= '0' && key.charAt(at) <= '9')
            i = i * 10 + (key.charAt(at++) - '0');
        if (at > prefix.length() && at < key.length() && key.charAt(at) == '_' && i < slots.length)
            slots[i] = true;
    }

    /**
     * Match dio_manager CRouteGuidanceUpdateProcessorImpl::isRouteGuidanceManeuverIndexGreater.
     * The C hook already gates on the original iOS indexes and remaps them into fixed slots, so
     * only the list matters here: none yet allows, an explicit empty one rejects.
     */
    private boolean acceptsManeuverData() {
        return hasRouteUpdate && (state.maneuverOrder == null || state.maneuverOrder.length > 0);
    }

    private static boolean strEq(String a, String b) {
        if (a == null) return b == null;
        return a.equals(b);
    }

    static boolean intArrayEq(int[] a, int[] b) {
        if (a == b) return true;
        if (a == null || b == null) return false;
        if (a.length != b.length) return false;
        for (int i = 0; i < a.length; i++) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    private static boolean intMatrixEq(int[][] a, int[][] b) {
        if (a == b) return true;
        if (a == null || b == null) return false;
        if (a.length != b.length) return false;
        for (int i = 0; i < a.length; i++) {
            if (!intArrayEq(a[i], b[i])) return false;
        }
        return true;
    }

    private static int[][] parseLaneAnglesMatrix(String raw) {
        if (raw == null || raw.length() == 0) return null;

        int lanes = 1;
        for (int i = 0; i < raw.length(); i++) {
            if (raw.charAt(i) == '|') lanes++;
        }

        int[][] out = new int[lanes][];
        int start = 0;
        int lane = 0;
        for (int i = 0; i <= raw.length() && lane < lanes; i++) {
            if (i == raw.length() || raw.charAt(i) == '|') {
                out[lane++] = parseCsvIntList(raw.substring(start, i));
                start = i + 1;
            }
        }
        return out;
    }

    private static int[] parseCsvIntList(String s) {
        if (s == null) return null;
        s = s.trim();
        if (s.length() == 0) return new int[0];

        int commas = 0;
        for (int i = 0; i < s.length(); i++) {
            if (s.charAt(i) == ',') commas++;
        }

        int[] out = new int[commas + 1];
        int start = 0;
        int idx = 0;
        for (int i = 0; i <= s.length() && idx < out.length; i++) {
            if (i == s.length() || s.charAt(i) == ',') {
                String part = s.substring(start, i).trim();
                if (part.length() == 0) return null;
                try {
                    out[idx++] = Integer.parseInt(part);
                } catch (Exception e) {
                    return null;
                }
                start = i + 1;
            }
        }
        return out;
    }
}
