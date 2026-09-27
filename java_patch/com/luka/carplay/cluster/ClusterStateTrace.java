/*
 * ClusterStateTrace - one bounded WARN line per CarPlay cluster edge.
 *
 * The September captures could not show what the cluster was left in at disconnect.  This
 * records, only at connect/disconnect, RGI start/stop, takeover release and gate reopen:
 * the platform fingerprint, the physical terminal-1 context, ScreenModule ownership, the
 * native RG gate, and the stock ClusterService/ClusterViewMode state (view mode, gfx,
 * data rate, rgActive, RGI validity).  WARN so it reaches /tmp/carplay_java.log without the
 * verbose marker.  Every read is side-effect free and every failure is swallowed.
 */
package com.luka.carplay.cluster;

import com.luka.carplay.framework.Log;

import de.audi.atip.base.IFrameworkAccess;
import de.audi.tghu.navi.app.Navigation;
import de.audi.tghu.navi.app.cluster.ClusterService;

public final class ClusterStateTrace {
    private static final String TAG = "ClusterState";
    private static final int TERMINAL_CLUSTER = com.luka.carplay.core.ScreenModule.TERMINAL_CLUSTER;

    private ClusterStateTrace() { }

    public static void dump(String edge) {
        try {
            IFrameworkAccess fw = com.luka.carplay.core.CarPlayApp.framework();
            StringBuffer line = new StringBuffer(384);
            line.append(edge).append(": ").append(ClusterPlatform.describe(fw));
            line.append(" ctx1=").append(currentContext(fw));
            line.append(" connected=").append(com.luka.carplay.core.ScreenModule.isConnected());
            line.append(" ownsCtx=").append(com.luka.carplay.core.ScreenModule.ownsClusterContext());
            line.append(" navActive=").append(com.luka.carplay.core.ScreenModule.isNavActive());
            line.append(' ').append(com.luka.carplay.core.ScreenNavStatusGate.describe());
            line.append(' ').append(MostPresentation.describe());
            line.append(' ').append(clusterService());
            Log.w(TAG, line.toString());
        } catch (Throwable t) {
            Log.w(TAG, edge + ": trace failed: " + t);
        }
    }

    private static String currentContext(IFrameworkAccess fw) {
        try {
            return String.valueOf(fw.getHMIService().getDisplayManager().getCurrentContextID(TERMINAL_CLUSTER));
        } catch (Throwable t) {
            return "?";
        }
    }

    private static String clusterService() {
        try {
            Navigation navigation = Navigation.getInstance();
            ClusterService cs = navigation != null ? navigation.getClusterService() : null;
            return cs != null ? cs.describeClusterStateForCarPlay() : "clusterService=none";
        } catch (Throwable t) {
            return "clusterService=? (" + t + ")";
        }
    }
}
