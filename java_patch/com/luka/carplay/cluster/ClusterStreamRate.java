/*
 * ClusterStreamRate -- logs the rate the cluster video stream is set to.
 *
 * On a MOST cluster the arrows reach the display as video: videoencoderservice captures
 * terminal 1 at its update rate, encodes it and sends it over MOST.  That rate caps how many
 * distinct frames the cluster can show, whatever the renderer draws.  Stock sets it from the
 * cluster's KOMO data rate (10 fps at data rate 2, 1 fps at 1; measured in v20), and
 * MostPresentation raises stock's 10 to 30 while CarPlay's arrows view is composed.
 *
 * This class asks DSIDisplayManagement for the cluster display's rate (getUpdateRate on
 * internal display 4, which DisplayManager maps from terminal 1) at CarPlay connect, at each
 * arrows-view edge and whenever the cluster reports a new KOMO data rate, and logs every
 * answer at WARN.  DisplayManagerMIB2High logs the rates requested through the HMI.
 * ScreenModule starts it on a MOST cluster at CarPlay connect and stops it at disconnect.
 *
 * Read-only and best-effort: getUpdateRate changes nothing, and every stock
 * DSIDisplayManagementListener in the MU1329 lsd.jar (fwhmi DSIDisplayListener,
 * PowerDisplayHandler, media DSIDisplayManagerListener, DSIDisplayManagerControllerImpl)
 * ignores getUpdateRateResult and setUpdateRateResult, so the extra reply changes no stock
 * state.  A missing service or failed registration is logged once and changes nothing else.
 *
 * Java 1.4 / Foundation 1.1 (no generics/autoboxing).
 */
package com.luka.carplay.cluster;

import com.luka.carplay.core.FrameworkRef;
import com.luka.carplay.framework.Log;

import org.dsi.ifc.displaymanagement.DSIDisplayManagement;
import org.dsi.ifc.displaymanagement.DSIDisplayManagementListener;
import org.osgi.framework.ServiceRegistration;

public final class ClusterStreamRate {
    private static final String TAG = "StreamRate";
    private static final int DSI_INSTANCE = 0;
    /** DisplayManager.getInternalDisplayID(terminal 1): the cluster. */
    static final int CLUSTER_DISPLAY = 4;

    private static final Object LOCK = new Object();
    private static DSIDisplayManagement dsi;          /* non-null while started */
    private static String pendingReason = "";
    private static String lastAnswer = "unknown";

    private FrameworkRef.ServiceHandle handle;
    private ServiceRegistration registration;
    private boolean missingLogged;

    public synchronized void start(FrameworkRef fw) {
        if (registration != null) return;
        if (fw == null || !fw.isReady() || fw.serviceManager() == null) return;
        FrameworkRef.ServiceHandle h = fw.getServiceHandle(DSIDisplayManagement.class);
        if (h == null || !(h.service() instanceof DSIDisplayManagement)) {
            if (h != null) h.release();
            if (!missingLogged) {
                missingLogged = true;
                Log.w(TAG, "DSIDisplayManagement not available; cluster stream rate not logged");
            }
            return;
        }
        try {
            registration = fw.serviceManager().registerDSIListener(
                DSI_INSTANCE, DSIDisplayManagementListener.class.getName(), new RateListener());
            handle = h;
            attach((DSIDisplayManagement) h.service());
            query("CarPlay connect");
        } catch (Throwable t) {
            registration = null;
            h.release();
            Log.w(TAG, "listener registration failed; cluster stream rate not logged: " + t);
        }
    }

    public synchronized void stop() {
        detach();
        ServiceRegistration r = registration;
        FrameworkRef.ServiceHandle h = handle;
        registration = null;
        handle = null;
        if (r != null) {
            try { r.unregister(); }
            catch (Throwable t) { Log.w(TAG, "listener unregistration failed: " + t); }
        }
        if (h != null) h.release();
    }

    static void attach(DSIDisplayManagement d) {
        synchronized (LOCK) { dsi = d; }
    }

    static void detach() {
        synchronized (LOCK) { dsi = null; pendingReason = ""; }
    }

    /** Ask for the cluster stream rate; the answer is logged when it arrives.  Never throws. */
    public static void query(String reason) {
        DSIDisplayManagement d;
        synchronized (LOCK) {
            d = dsi;
            if (d == null) return;
            pendingReason = reason;
        }
        try {
            d.getUpdateRate(CLUSTER_DISPLAY);
        } catch (Throwable t) {
            Log.w(TAG, "getUpdateRate(" + CLUSTER_DISPLAY + ") after " + reason + " failed: " + t);
        }
    }

    /** For the cluster state trace: the last rate requested through the HMI and the last DSI answer. */
    public static String describe() {
        String answer;
        synchronized (LOCK) { answer = lastAnswer; }
        return "streamRate{requested=" + requestedClusterRate() + " dsi=" + answer + "}";
    }

    private static String requestedClusterRate() {
        try {
            int r = de.audi.tghu.fwhmi.DisplayManagerMIB2High.requestedUpdateRate(
                com.luka.carplay.core.ScreenModule.TERMINAL_CLUSTER);
            return r < 0 ? "none" : String.valueOf(r);
        } catch (Throwable t) {
            return "?";
        }
    }

    static void onUpdateRate(int display, int rate) {
        String reason;
        synchronized (LOCK) {
            reason = pendingReason;
            lastAnswer = "(" + display + ", " + rate + ")";
        }
        /* Argument order is not documented; both are logged as delivered. */
        Log.w(TAG, "cluster stream: getUpdateRateResult(" + display + ", " + rate + ") for display "
            + CLUSTER_DISPLAY + " after " + reason + "; requested via HMI " + requestedClusterRate());
    }

    static void onSetResult(int display, int result) {
        Log.w(TAG, "setUpdateRateResult(" + display + ", " + result + ")");
    }

    /** Only the two rate replies are used; every other display-management reply is ignored. */
    static final class RateListener implements DSIDisplayManagementListener {
        public void getUpdateRateResult(int a, int b) { onUpdateRate(a, b); }
        public void setUpdateRateResult(int a, int b) { onSetResult(a, b); }
        public void asyncException(int error, String message, int request) {
            if (request == DSIDisplayManagement.RT_GETUPDATERATE || request == DSIDisplayManagement.RT_SETUPDATERATE) {
                Log.w(TAG, "update rate request " + request + " failed: " + error + " " + message);
            }
        }
        public void getExtents(int a, int b, int c) { }
        public void activeContext(int a, int b, int c) { }
        public void fadeStarted(int a, int b) { }
        public void fadeComplete(int a, int b) { }
        public void getDisplayPower(int a, int b) { }
        public void getDisplayBrightness(int a, int b) { }
        public void getBrightness(int a, int b) { }
        public void getContrast(int a, int b) { }
        public void getColor(int a, int b) { }
        public void getTint(int a, int b) { }
        public void lockDisplayResult(int a) { }
        public void unlockDisplayResult(int a) { }
        public void setCroppingResult(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j, int k) { }
        public void getDisplayableInfo(int a, int b, int c) { }
        public void takeScreenshotOnExternalStorageResult(int a, int b, String c) { }
        public void setDisplayTypeResult(int a, int b) { }
        public void getDisplayTypeResult(int a, int b) { }
        public void startComponentResult(int a, int b, int c, int d) { }
        public void stopComponentResult(int a, int b, int c, int d) { }
        public void setAnnotationDataResponse(int a, int b) { }
        public void initAnnotationsResponse(int a, int b) { }
        public void destroyImageDisplayableResponse(int a, int b) { }
        public void requestUpdateImageDisplayableResponse(int a, int b) { }
        public void createImageDisplayableResponse(int a, int b) { }
    }
}
