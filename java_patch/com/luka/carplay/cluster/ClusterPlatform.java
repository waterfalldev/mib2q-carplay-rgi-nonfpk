/*
 * ClusterPlatform - which instrument-cluster presentation the head unit is coded for.
 *
 * The CarPlay cluster context (dc[80] = {98,101,102,33}), the KDK layer
 * controller and the terminal-1 context pin were built for the Virtual Cockpit (FPK), where
 * stock sysConst KOMBI_MAP_MODE (541) == 2 and the KDK is composed from displayables 101/102.
 * Stock MHI2Q has two other cluster variants that reach the same Java:
 *
 *   541 == 1  map/KDK streamed to the cluster over MOST (Util.isClusterMapMOST); the stock
 *             ClusterViewMode/KOMOService state machine owns what terminal 1 shows
 *   541 == 0  RGI only: the cluster draws its own arrows from BAP (Util.isClusterRGI)
 *
 * On those variants configureDM() never creates 101/102 and CombiMapController never runs
 * handleKdk(), so the VC context/opacity machinery has no presentation path, and taking
 * terminal 1 away from stock only breaks OEM restoration.  A MOST cluster gets its own path
 * instead (MostPresentation: CarPlay's maneuver in the stock arrows view; CarPlayKOMOService:
 * guidance text).  This class answers which applies, fail-safe: when unknown, leave stock alone.
 */
package com.luka.carplay.cluster;

import de.audi.atip.base.IFrameworkAccess;

public final class ClusterPlatform {
    public static final int SYSCONST_KOMBI_MAP_MODE = 541;   /* ICoreSysConfig.KOMBI_MAP_MODE */
    public static final int SYSCONST_KOMBI_MAP = 4383;       /* ICoreSysConfig.KOMBI_MAP */
    public static final int SYSCONST_KOMBI_KDK = 4388;       /* ICoreSysConfig.KOMBI_KDK */
    public static final int KOMBI_MAP_MODE_FPK = 2;          /* Util.isClusterMapFPK */
    public static final int KOMBI_MAP_MODE_MOST = 1;         /* Util.isClusterMapMOST */
    private static final int KOMBI_TYPE_G24 = 4;

    private static final int MODE_UNKNOWN = -1;              /* G24, no framework, or unreadable */
    private static final int MODE_UNRESOLVED = Integer.MIN_VALUE;

    private static volatile IFrameworkAccess framework;
    /* The coding cannot change without a reboot: fixed once a session start has read it. */
    private static volatile int resolvedMode = MODE_UNRESOLVED;

    private ClusterPlatform() { }

    /** DisplayManagerMIB2High records the framework it was built with, before any CarPlay. */
    public static void bind(IFrameworkAccess fw) {
        if (fw != null) {
            framework = fw;
            resolvedMode = MODE_UNRESOLVED;
        }
    }

    /** ScreenModule.start (framework ready): fix the coding for the hot no-argument queries.
     *  Not done in bind(), which runs before the navigation sysConsts are applied. */
    public static void resolve(IFrameworkAccess fw) {
        int mode = kombiMapMode(fw);
        if (mode != MODE_UNKNOWN) resolvedMode = mode;
    }

    /** sysConst 541 (KOMBI_MAP_MODE), or MODE_UNKNOWN on G24 / without a readable framework. */
    private static int kombiMapMode(IFrameworkAccess fw) {
        if (fw == null) return MODE_UNKNOWN;
        try {
            if (fw.getKombiType() == KOMBI_TYPE_G24) return MODE_UNKNOWN;
            return fw.getSysConst(SYSCONST_KOMBI_MAP_MODE);
        } catch (Throwable t) {
            return MODE_UNKNOWN;
        }
    }

    private static int mode() {
        int mode = resolvedMode;
        return mode != MODE_UNRESOLVED ? mode : kombiMapMode(framework);
    }

    /** True only for the validated Virtual Cockpit composition (not G24, sysConst 541 == 2). */
    public static boolean ownsContexts(IFrameworkAccess fw) {
        return kombiMapMode(fw) == KOMBI_MAP_MODE_FPK;
    }

    public static boolean ownsContexts() {
        return mode() == KOMBI_MAP_MODE_FPK;
    }

    /** Map and KDK streamed to the cluster over MOST (stock Util.isClusterMapMOST, 541 == 1).
     *  Its arrows view is the head unit's KDK image in display context 73 - see MostPresentation. */
    public static boolean isMost(IFrameworkAccess fw) {
        return kombiMapMode(fw) == KOMBI_MAP_MODE_MOST;
    }

    public static boolean isMost() {
        return mode() == KOMBI_MAP_MODE_MOST;
    }

    /** One-line fingerprint for the bounded diagnostics. */
    public static String describe(IFrameworkAccess fw) {
        if (fw == null) fw = framework;
        if (fw == null) return "platform=unknown";
        return "kombiType=" + value(fw, -1)
            + " sysConst(541)=" + value(fw, SYSCONST_KOMBI_MAP_MODE)
            + " sysConst(4383)=" + value(fw, SYSCONST_KOMBI_MAP)
            + " sysConst(4388)=" + value(fw, SYSCONST_KOMBI_KDK)
            + " ownsContexts=" + ownsContexts(fw);
    }

    private static String value(IFrameworkAccess fw, int sysConst) {
        try {
            return String.valueOf(sysConst < 0 ? fw.getKombiType() : fw.getSysConst(sysConst));
        } catch (Throwable t) {
            return "?";
        }
    }
}
