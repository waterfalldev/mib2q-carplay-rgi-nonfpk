package com.luka.carplay.core;

import com.luka.carplay.framework.Log;
import com.luka.carplay.rgd.GatedCombiService;
import com.luka.carplay.rgd.RouteGuidance;
import de.audi.app.terminalmode.IContext;
import de.audi.atip.base.IFrameworkAccess;
import de.audi.atip.interapp.combi.bap.navi.CombiBAPServiceNavi;
import de.audi.tghu.navi.app.Navigation;
import de.audi.tghu.navi.app.cluster.ClusterService;
import de.audi.tghu.navi.app.cluster.CombiBAPListener;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import sun.misc.Unsafe;
import static testsupport.TestSupport.*;

/**
 * core seams.  Host-only; runs the shipping carplay_hook.jar classes
 * against the supplied stock JAR.  Every scenario names the on-car failure it guards against.
 */
public final class ClusterCoreTest {

    /** Framework coded for the given KOMBI_MAP_MODE; counts HMI-service lookups. */
    static final class Fw implements InvocationHandler {
        final int kombiMapMode;
        int hmiServiceLookups;
        Fw(int kombiMapMode) { this.kombiMapMode = kombiMapMode; }
        public Object invoke(Object proxy, Method m, Object[] a) {
            if (m.getName().equals("getSysConst")) {
                return Integer.valueOf(((Integer) a[0]).intValue() == 541 ? kombiMapMode : 0);
            }
            if (m.getName().equals("getKombiType")) return Integer.valueOf(0);
            if (m.getName().equals("getHMIService")) { hmiServiceLookups++; return null; }
            if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
            if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
            return null;
        }
        IFrameworkAccess proxy() {
            return (IFrameworkAccess) Proxy.newProxyInstance(ClusterCoreTest.class.getClassLoader(),
                new Class<?>[]{IFrameworkAccess.class}, this);
        }
    }

    static FrameworkRef ref(final IFrameworkAccess fw) {
        IContext ctx = (IContext) Proxy.newProxyInstance(ClusterCoreTest.class.getClassLoader(),
            new Class<?>[]{IContext.class}, new InvocationHandler() {
                public Object invoke(Object p, Method m, Object[] a) {
                    if (m.getName().equals("getFramework")) return fw;
                    if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
                    if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
                    return null;
                }
            });
        return new FrameworkRef(ctx);
    }

    static boolean switchWorkerAlive() {
        for (Thread t : Thread.getAllStackTraces().keySet()) {
            if ("carplay-cluster-switch".equals(t.getName()) && t.isAlive()) return true;
        }
        return false;
    }

    /** On a MOST (541=1) or RGI-only (541=0) cluster the session must not take terminal 1. */
    static void screenModuleLeavesNonFpkClusterToStock(int kombiMapMode) throws Exception {
        Fw handler = new Fw(kombiMapMode);
        AltScreenModule screen = new AltScreenModule();
        check(screen.start(ref(handler.proxy())), "non-FPK start must complete, not retry");
        check(handler.hmiServiceLookups == 0, "non-FPK start must not acquire the DisplayManager");
        check(!switchWorkerAlive(), "non-FPK start must not create the context switch worker");
        check(AltScreenModule.isConnected(), "session semantics (isConnected) stay for other modules");
        check(!AltScreenModule.ownsClusterContext(), "541=" + kombiMapMode + " must not own terminal-1 contexts");
        screen.stop();
        check(!AltScreenModule.isConnected(), "stop clears the session");
        check(!AltScreenModule.ownsClusterContext(), "stop leaves no ownership");
    }

    /** Virtual Cockpit keeps the upstream path: it goes on to acquire the DisplayManager. */
    static void screenModuleKeepsFpkPath() throws Exception {
        Fw handler = new Fw(2);
        AltScreenModule screen = new AltScreenModule();
        check(!screen.start(ref(handler.proxy())), "FPK start without a DisplayManager retries (upstream)");
        check(handler.hmiServiceLookups == 1, "FPK start proceeds to the DisplayManager");
        check(((Boolean) getStatic(AltScreenModule.class, "clusterContextsOwned")).booleanValue(),
            "FPK marks the terminal-1 composition as ours");
        setStatic(AltScreenModule.class, "clusterContextsOwned", Boolean.FALSE);
    }

    /** The hook picks its stream-111 receiver from this file: written in place, never renamed
     * (the head unit's /tmp cannot rename), and left alone while the value is unchanged. */
    static void platformMarkerWrittenInPlace() throws Exception {
        java.io.File marker = new java.io.File("/tmp/carplay_cluster_platform");
        marker.delete();
        com.luka.carplay.cluster.ClusterPlatform.resolve(new Fw(1).proxy());
        check("most\n".equals(read(marker)), "MOST coding publishes most");
        check(marker.setLastModified(1000L), "test can stamp the marker");
        com.luka.carplay.cluster.ClusterPlatform.resolve(new Fw(1).proxy());
        check(marker.lastModified() == 1000L, "an unchanged coding does not rewrite the marker");
        com.luka.carplay.cluster.ClusterPlatform.resolve(new Fw(2).proxy());
        check("vc\n".equals(read(marker)), "a changed coding rewrites the marker");
        check(!new java.io.File("/tmp/carplay_cluster_platform.tmp").exists(), "no temporary file");
        marker.delete();
    }

    static String read(java.io.File f) throws Exception {
        java.io.FileInputStream in = new java.io.FileInputStream(f);
        try {
            byte[] b = new byte[16];
            int n = in.read(b);
            return n < 0 ? "" : new String(b, 0, n, "US-ASCII");
        } finally { in.close(); }
    }

    public static void main(String[] args) throws Exception {
        Log.setLevel(-1);
        platformMarkerWrittenInPlace();
        screenModuleLeavesNonFpkClusterToStock(1);
        screenModuleLeavesNonFpkClusterToStock(0);
        screenModuleKeepsFpkPath();
        System.out.println("ClusterCoreTest: platform context ownership PASS ("+checks+" checks)");
    }
}
