/*
 * ClusterVideo - the phone's cluster stream reaching the renderer, as the hook reports it:
 * EVT_CLUSTER_VIDEO (sticky) says a stream connection is "live" once its frames decrypt to
 * H.264 and the renderer can read them from the frame ring, and when that stream ends.  It
 * passes that on to MostPresentation.setMapLive, which takes the MAP view while CarPlay
 * guidance also holds.  MostPresentation composes the view only once the renderer reports a
 * window of the measured size with a decoded picture posted in it, and hands it back to stock
 * when guidance or the stream ends, or CarPlay disconnects.
 */
package com.luka.carplay.cluster;

import com.luka.carplay.bus.CarplayBus;
import com.luka.carplay.framework.Log;

public final class ClusterVideo implements CarplayBus.Listener {

    private static final String TAG = "ClusterVideo";
    private static final ClusterVideo INSTANCE = new ClusterVideo();

    /* Guarded by this; the MAP request is changed only under it, so stop() is never overtaken. */
    private boolean running;
    private boolean live;
    private long stream;

    private ClusterVideo() { }

    public static ClusterVideo getInstance() { return INSTANCE; }

    /** ScreenModule.start on a MOST cluster: a new session, with no stream yet. */
    public void start() {
        synchronized (this) {
            live = false;
            stream = 0;
            MostPresentation.setMapLive(false);
            if (running) return;
            running = true;
        }
        CarplayBus bus = CarplayBus.getInstance();
        bus.on(CarplayBus.EVT_CLUSTER_VIDEO, this);
        bus.start();   /* idempotent */
    }

    /** ScreenModule.stop: the session's stream is gone with it. */
    public void stop() {
        synchronized (this) {
            if (!running) return;
            running = false;
            live = false;
            MostPresentation.setMapLive(false);
        }
        CarplayBus.getInstance().off(CarplayBus.EVT_CLUSTER_VIDEO);
    }

    /* The MMI's night mode (1 night, 0 day), -1 until CarPlay starts; guarded by this. */
    private int night = -1;

    /** The MMI's night mode at CarPlay start and on every change (CarplayDSILifecycleController).
     *  Stock tells the phone for the whole session, and the cluster map does not follow
     *  (map23): the hook sets the cluster display's own (setNightMode with its uuid), now and
     *  each time its stream goes live. */
    public void setNightMode(boolean on) {
        synchronized (this) {
            night = on ? 1 : 0;
        }
        sendNightMode("MMI");
    }

    private void sendNightMode(String why) {
        int n;
        synchronized (this) {
            n = night;
        }
        if (n < 0) return;
        boolean sent = CarplayBus.getInstance().sendBinary(CarplayBus.CMD_ALT_APPEARANCE, new byte[] { (byte) n });
        Log.w(TAG, (n == 1 ? "night" : "day") + " mode for the cluster display (" + why + ")"
            + (sent ? "" : ": not sent, no hook connection"));
    }

    /* changeMapZoomLevel per wheel report, at most (the hook caps at the same). */
    private static final int ZOOM_MAX_STEPS = 8;

    /** The steering-wheel roller (ScreenCombiBAPListener.setMapScale, MapScale steps, positive
     *  zooms out): while the MAP view shows the phone's map, the steps zoom that map - the hook
     *  sends the phone one changeMapZoomLevel per step.  False leaves them to stock's map. */
    public boolean zoom(int steps) {
        synchronized (this) {
            if (!running || !live) return false;
        }
        if (steps == 0 || !MostPresentation.isMapActive()) return false;
        int capped = steps > ZOOM_MAX_STEPS ? ZOOM_MAX_STEPS : steps < -ZOOM_MAX_STEPS ? -ZOOM_MAX_STEPS : steps;
        boolean sent = CarplayBus.getInstance().sendBinary(CarplayBus.CMD_ALT_ZOOM, new byte[] { (byte) capped });
        Log.w(TAG, "wheel zoom " + steps + (sent ? " sent to the phone's map" : " not sent: no hook connection"));
        return sent;
    }

    public void onFrame(int type, int flags, byte[] payload, int len) {
        if (type != CarplayBus.EVT_CLUSTER_VIDEO) return;
        CarplayBus.Data d = CarplayBus.parseText(payload, len);
        boolean on = d.bool("live", false);
        long s = d.num64("stream", 0);
        synchronized (this) {
            if (!running || (on == live && s == stream)) return;
            live = on;
            stream = s;
            MostPresentation.setMapLive(live);
        }
        Log.i(TAG, "cluster stream " + s + (on ? " live" : " ended"));
        if (on) sendNightMode("stream live");
    }
}
