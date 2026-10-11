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
    }
}
