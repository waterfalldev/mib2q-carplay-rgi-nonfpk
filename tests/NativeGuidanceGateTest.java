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
 * Guidance restoration, core seams.  Host-only; runs the shipping carplay_hook.jar classes
 * against the supplied stock JAR.  Every scenario names the on-car failure it guards against.
 */
public final class NativeGuidanceGateTest {

    /** A5-class framework coded for the given KOMBI_MAP_MODE; counts HMI-service lookups. */
    static final class Sink implements InvocationHandler {
        final List<String> calls = new ArrayList<String>();
        public Object invoke(Object p, Method m, Object[] a) {
            calls.add(m.getName());
            if (m.getReturnType() == Integer.TYPE) return Integer.valueOf(0);
            if (m.getReturnType() == Boolean.TYPE) return Boolean.FALSE;
            return null;
        }
        CombiBAPServiceNavi proxy() {
            return (CombiBAPServiceNavi) Proxy.newProxyInstance(NativeGuidanceGateTest.class.getClassLoader(),
                new Class<?>[]{CombiBAPServiceNavi.class}, this);
        }
    }

    /** The gate reopened only via a NavigationJobs pass; without one it stayed shut for the
     * rest of the ignition cycle and stock RG never reached the cluster again. */
    static void gateOpensImmediatelyWithoutDispatcher() throws Exception {
        HostNavigation.install(null);   /* no NavigationJobs dispatcher at all */
        Sink real = new Sink();
        GatedCombiService gate = new GatedCombiService(real.proxy());
        gate.setRouteGuidanceBlocked(true);
        real.calls.clear();   /* blocking hides the stock scale and altitude */
        setStatic(AltScreenNavStatusGate.class, "gate", gate);
        setStatic(AltScreenNavStatusGate.class, "desiredRouteBlocked", Boolean.TRUE);
        setStatic(AltScreenNavStatusGate.class, "appliedRouteBlocked", Boolean.TRUE);
        setStatic(AltScreenNavStatusGate.class, "installScheduled", Boolean.FALSE);

        gate.updateRGStatus(1);
        check(real.calls.isEmpty(), "blocked gate drops stock RG (precondition)");
        AltScreenNavStatusGate.setRouteGuidanceBlocked(false);
        gate.updateRGStatus(1);
        gate.updateManeuverDescriptor(null);
        check(real.calls.size() == 2 && "updateRGStatus".equals(real.calls.get(0)),
            "released gate passes stock RG through at once: " + real.calls);
    }

    /** Stock Navigation cannot be reflected on the host (a field type lives outside lsd.jar),
     * so reach its protected singleton and cluster service the way a subclass may. */
    static final class HostNavigation extends Navigation {
        HostNavigation() { super(null); }
        static void install(ClusterService cs) {
            HostNavigation navigation = null;
            if (cs != null) {
                navigation = (HostNavigation) alloc(HostNavigation.class);
                navigation.clusterService = cs;
            }
            Navigation.instance = navigation;
        }
    }

    static final class RecordingListener extends CombiBAPListener {
        int setCombiServiceCalls;
        CombiBAPServiceNavi lastService;
        RecordingListener() { super(null, null, null, null, null, null, null, null, null); }
        public void setCombiService(CombiBAPServiceNavi service) {
            setCombiServiceCalls++;
            lastService = service;
            this.combiservice = service;
        }
    }

    /** Stock sends BAP only on change, so everything it produced while gated was lost. */
    static void reopenReplaysStockCacheOnNavigationJobs() throws Exception {
        Sink real = new Sink();
        GatedCombiService gate = new GatedCombiService(real.proxy());
        RecordingListener listener = (RecordingListener) alloc(RecordingListener.class);
        set(listener, "combiservice", gate);
        ClusterService cs = (ClusterService) alloc(ClusterService.class);
        set(cs, "combiBAPListener", listener);
        HostNavigation.install(cs);

        setStatic(AltScreenNavStatusGate.class, "clusterService", cs);
        setStatic(AltScreenNavStatusGate.class, "gate", gate);
        setStatic(AltScreenNavStatusGate.class, "desiredRouteBlocked", Boolean.FALSE);
        setStatic(AltScreenNavStatusGate.class, "appliedRouteBlocked", Boolean.TRUE);
        Method installNow = AltScreenNavStatusGate.class.getDeclaredMethod("installNow");
        installNow.setAccessible(true);

        installNow.invoke(null);   /* the NavigationJobs pass scheduled by the release */
        check(listener.setCombiServiceCalls == 1 && listener.lastService == gate,
            "reopen replays the stock CombiBAPListener cache once through the same gate");
        check(!((Boolean) getStatic(AltScreenNavStatusGate.class, "appliedRouteBlocked")).booleanValue(),
            "applied state follows the release");
        installNow.invoke(null);
        check(listener.setCombiServiceCalls == 1, "an already-open gate does not replay again");

        setStatic(AltScreenNavStatusGate.class, "desiredRouteBlocked", Boolean.TRUE);
        installNow.invoke(null);
        check(listener.setCombiServiceCalls == 1, "blocking never replays");
        HostNavigation.install(null);
        setStatic(AltScreenNavStatusGate.class, "clusterService", null);
        setStatic(AltScreenNavStatusGate.class, "gate", null);
        setStatic(AltScreenNavStatusGate.class, "desiredRouteBlocked", Boolean.FALSE);
        setStatic(AltScreenNavStatusGate.class, "appliedRouteBlocked", Boolean.FALSE);
    }

    public static void main(String[] args) throws Exception {
        Log.setLevel(-1);
        gateOpensImmediatelyWithoutDispatcher();
        reopenReplaysStockCacheOnNavigationJobs();
        System.out.println("NativeGuidanceGateTest: immediate release and cached-state replay PASS ("+checks+" checks)");
    }
}
