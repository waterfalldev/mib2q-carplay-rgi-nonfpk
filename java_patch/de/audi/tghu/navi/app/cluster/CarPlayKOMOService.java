package de.audi.tghu.navi.app.cluster;

import de.audi.tghu.navi.app.NavigationEnv;

/**
 * stock KOMOService with one owner at a time for the KOMO guidance text.
 *
 * On a MOST cluster the text in the arrows and map views - distance to the next maneuver,
 * turn-to street, current street, arrival time, remaining time, distance to destination -
 * is what the head unit sends through DSIKOMONavInfo (KOMOCaller).  Stock ClusterService writes
 * those fields directly and is not part of the BAP route-guidance gate.
 *
 * While CarPlay route guidance owns the fields, every stock write is recorded but not sent, and
 * CarPlay's values go out instead.  Releasing replays stock's latest value for each field, or
 * stock's own "invalid" value where stock wrote nothing, so no CarPlay text outlives the
 * session.  Everything else (views, fades, data rate, map scale, listener callbacks from the
 * cluster) is untouched stock behaviour, with one scoped exception: the cluster's KOMO view
 * visibility, below.
 */
public class CarPlayKOMOService extends KOMOService {
    private static final int INVALID_DISTANCE_UNIT = 255;    /* stock ClusterService, maneuver */
    private static final int INVALID_DESTINATION_UNIT = -1;  /* stock ClusterService, destination */

    private final Object fieldLock = new Object();
    private boolean carPlayOwned;

    /* Stock's latest write per field. */
    private boolean stockDistanceSet;
    private long stockDistanceValue;
    private int stockDistanceUnit;
    private boolean stockDistanceValid;
    private boolean stockTurnToSet;
    private String stockTurnTo;
    private String stockTurnToSecondary;
    private boolean stockCurrentStreetSet;
    private String stockCurrentStreet;
    private boolean stockEtaSet;
    private int stockEtaFormat;
    private short stockEtaDay;
    private short stockEtaHour;
    private short stockEtaMinute;
    private boolean stockEtaValid;
    private boolean stockEtaFlag;
    private boolean stockRttSet;
    private short stockRttHour;
    private short stockRttMinute;
    private boolean stockRttValid;
    private boolean stockDestinationSet;
    private long stockDestinationValue;
    private int stockDestinationUnit;
    private boolean stockDestinationValid;

    /* CarPlay's latest write per field, to avoid resending unchanged values on every delta. */
    private boolean carPlayDistanceSent;
    private long carPlayDistanceValue;
    private int carPlayDistanceUnit;
    private boolean carPlayDistanceValid;
    private String carPlayTurnTo;
    private String carPlayCurrentStreet;
    private boolean carPlayEtaSent;
    private int carPlayEtaFormat;
    private short carPlayEtaDay;
    private short carPlayEtaHour;
    private short carPlayEtaMinute;
    private boolean carPlayEtaValid;
    private boolean carPlayRttSent;
    private short carPlayRttHour;
    private short carPlayRttMinute;
    private boolean carPlayRttValid;
    private boolean carPlayDestinationSent;
    private long carPlayDestinationValue;
    private int carPlayDestinationUnit;
    private boolean carPlayDestinationValid;

    public CarPlayKOMOService(NavigationEnv env, ClusterService service, ClusterKDKHandler kdkHandler) {
        super(env, service, kdkHandler);
    }

    /* ---- stock writers: record, forward only while stock owns the fields ---- */

    public void setDistanceToNextManeuver(long value, int unit, boolean valid) {
        synchronized (fieldLock) {
            stockDistanceSet = true;
            stockDistanceValue = value;
            stockDistanceUnit = unit;
            stockDistanceValid = valid;
            if (!carPlayOwned) super.setDistanceToNextManeuver(value, unit, valid);
        }
    }

    public void setTurnToStreet(String street, String secondary) {
        synchronized (fieldLock) {
            stockTurnToSet = true;
            stockTurnTo = street;
            stockTurnToSecondary = secondary;
            if (!carPlayOwned) super.setTurnToStreet(street, secondary);
        }
    }

    public void setCurrentStreet(String street) {
        synchronized (fieldLock) {
            stockCurrentStreetSet = true;
            stockCurrentStreet = street;
            if (!carPlayOwned) super.setCurrentStreet(street);
        }
    }

    public void setETA(int format, short day, short hour, short minute, boolean valid, boolean flag) {
        synchronized (fieldLock) {
            stockEtaSet = true;
            stockEtaFormat = format;
            stockEtaDay = day;
            stockEtaHour = hour;
            stockEtaMinute = minute;
            stockEtaValid = valid;
            stockEtaFlag = flag;
            if (!carPlayOwned) super.setETA(format, day, hour, minute, valid, flag);
        }
    }

    public void setRTT(short hour, short minute, boolean valid) {
        synchronized (fieldLock) {
            stockRttSet = true;
            stockRttHour = hour;
            stockRttMinute = minute;
            stockRttValid = valid;
            if (!carPlayOwned) super.setRTT(hour, minute, valid);
        }
    }

    public void setDistanceToDestination(long value, int unit, boolean valid) {
        synchronized (fieldLock) {
            stockDestinationSet = true;
            stockDestinationValue = value;
            stockDestinationUnit = unit;
            stockDestinationValid = valid;
            if (!carPlayOwned) super.setDistanceToDestination(value, unit, valid);
        }
    }

    /* ---- ownership ---- */

    public boolean isCarPlayOwned() {
        synchronized (fieldLock) { return carPlayOwned; }
    }

    /** Hand the guidance fields to CarPlay, or back to stock (replaying stock's latest values). */
    public void setCarPlayOwned(boolean owned) {
        synchronized (fieldLock) {
            if (carPlayOwned == owned) return;
            setCarPlayOwnedLocked(owned);
        }
        holdVisibility(owned);
    }

    /** fieldLock held. */
    private void setCarPlayOwnedLocked(boolean owned) {
        carPlayOwned = owned;
        carPlayDistanceSent = false;
        carPlayTurnTo = null;
        carPlayCurrentStreet = null;
        carPlayEtaSent = false;
        carPlayRttSent = false;
        carPlayDestinationSent = false;
        if (owned) return;
        if (stockDistanceSet) super.setDistanceToNextManeuver(stockDistanceValue, stockDistanceUnit, stockDistanceValid);
        else super.setDistanceToNextManeuver(-1L, INVALID_DISTANCE_UNIT, false);
        if (stockTurnToSet) super.setTurnToStreet(stockTurnTo, stockTurnToSecondary);
        else super.setTurnToStreet("", "");
        if (stockCurrentStreetSet) super.setCurrentStreet(stockCurrentStreet);
        else super.setCurrentStreet("");
        if (stockEtaSet) super.setETA(stockEtaFormat, stockEtaDay, stockEtaHour, stockEtaMinute, stockEtaValid, stockEtaFlag);
        else super.setETA(0, (short) 0, (short) 0, (short) 0, false, false);
        if (stockRttSet) super.setRTT(stockRttHour, stockRttMinute, stockRttValid);
        else super.setRTT((short) 0, (short) 0, false);
        if (stockDestinationSet) super.setDistanceToDestination(stockDestinationValue, stockDestinationUnit, stockDestinationValid);
        else super.setDistanceToDestination(-1L, INVALID_DESTINATION_UNIT, false);
    }

    /* ---- the cluster's KOMO view reports (DSIKOMONavInfo/DSIKOMOView listener) ----
     *
     * Stock offers the arrows view only while the cluster reports its KOMO view visible
     * (ClusterViewMode.isKDKReady), and on a MOST cluster that view is shown only once the head
     * unit has selected it.  Starting from compass that never resolves: on 25 September 2026 the
     * cluster reported visible=false at every edge and stock stayed in COMPASS.  While CarPlay owns
     * the guidance fields (RGI on a MOST cluster), stock therefore sees the view as visible.  The
     * cluster's own reports are still recorded throughout, and on release its latest report is
     * handed to stock, so nothing CarPlay-made outlives the route. */
    private final Object visibilityLock = new Object();
    private boolean clusterVisible;
    private boolean clusterVisibleReported;
    private boolean visibilityHeld;
    private int lastKomoViewEnabled = -1;
    private int lastGfxState = -1;
    private int lastDataRate = -1;

    public void updateVisibility(boolean visible, int validity) {
        synchronized (visibilityLock) {
            if (validity == 1 && (!clusterVisibleReported || visible != clusterVisible)) {
                com.luka.carplay.framework.Log.w(ClusterService.VIEW_TAG, "cluster KOMO view visible=" + visible
                    + (visibilityHeld ? " (stock keeps true while CarPlay guides)" : ""));
            }
            if (validity == 1) {
                clusterVisible = visible;
                clusterVisibleReported = true;
            }
            super.updateVisibility(visibilityHeld && validity == 1 ? true : visible, validity);
        }
    }

    public void updateKomoViewEnabled(boolean enabled, int validity) {
        if (validity == 1) lastKomoViewEnabled = logChange("cluster KOMO view enabled", lastKomoViewEnabled, enabled ? 1 : 0);
        super.updateKomoViewEnabled(enabled, validity);
    }

    public void updateGfxState(int state, int validity) {
        if (validity == 1) lastGfxState = logChange("cluster KOMO gfx state", lastGfxState, state);
        super.updateGfxState(state, validity);
    }

    public void updateDataRate(int rate, int validity) {
        if (validity == 1) lastDataRate = logChange("cluster KOMO data rate", lastDataRate, rate);
        super.updateDataRate(rate, validity);
    }

    private static int logChange(String what, int last, int value) {
        if (value != last) com.luka.carplay.framework.Log.w(ClusterService.VIEW_TAG, what + "=" + value);
        return value;
    }

    /** CarPlay guidance edge: hold stock's view visibility at true, or hand back the cluster's own. */
    private void holdVisibility(boolean hold) {
        synchronized (visibilityLock) {
            if (visibilityHeld == hold) return;
            visibilityHeld = hold;
            com.luka.carplay.framework.Log.w(ClusterService.VIEW_TAG, hold
                ? "KOMO view visibility held for CarPlay (cluster reports " + clusterVisible + ")"
                : "KOMO view visibility returned to the cluster's report (" + clusterVisible + ")");
            super.updateVisibility(hold || clusterVisible, 1);
        }
    }

    /* ---- CarPlay writers (ClusterService.publishCarPlayKomoGuidance) ---- */

    void carPlayDistanceToNextManeuver(long value, int unit, boolean valid) {
        synchronized (fieldLock) {
            if (!carPlayOwned) return;
            if (carPlayDistanceSent && value == carPlayDistanceValue && unit == carPlayDistanceUnit
                    && valid == carPlayDistanceValid) return;
            carPlayDistanceSent = true;
            carPlayDistanceValue = value;
            carPlayDistanceUnit = unit;
            carPlayDistanceValid = valid;
            super.setDistanceToNextManeuver(value, unit, valid);
        }
    }

    void carPlayTurnToStreet(String street) {
        synchronized (fieldLock) {
            if (!carPlayOwned || street.equals(carPlayTurnTo)) return;
            carPlayTurnTo = street;
            super.setTurnToStreet(street, "");
        }
    }

    void carPlayCurrentStreet(String street) {
        synchronized (fieldLock) {
            if (!carPlayOwned || street.equals(carPlayCurrentStreet)) return;
            carPlayCurrentStreet = street;
            super.setCurrentStreet(street);
        }
    }

    void carPlayEta(int format, short day, short hour, short minute, boolean valid) {
        synchronized (fieldLock) {
            if (!carPlayOwned) return;
            if (carPlayEtaSent && format == carPlayEtaFormat && day == carPlayEtaDay && hour == carPlayEtaHour
                    && minute == carPlayEtaMinute && valid == carPlayEtaValid) return;
            carPlayEtaSent = true;
            carPlayEtaFormat = format;
            carPlayEtaDay = day;
            carPlayEtaHour = hour;
            carPlayEtaMinute = minute;
            carPlayEtaValid = valid;
            super.setETA(format, day, hour, minute, valid, false);
        }
    }

    void carPlayRtt(short hour, short minute, boolean valid) {
        synchronized (fieldLock) {
            if (!carPlayOwned) return;
            if (carPlayRttSent && hour == carPlayRttHour && minute == carPlayRttMinute
                    && valid == carPlayRttValid) return;
            carPlayRttSent = true;
            carPlayRttHour = hour;
            carPlayRttMinute = minute;
            carPlayRttValid = valid;
            super.setRTT(hour, minute, valid);
        }
    }

    void carPlayDistanceToDestination(long value, int unit, boolean valid) {
        synchronized (fieldLock) {
            if (!carPlayOwned) return;
            if (carPlayDestinationSent && value == carPlayDestinationValue && unit == carPlayDestinationUnit
                    && valid == carPlayDestinationValid) return;
            carPlayDestinationSent = true;
            carPlayDestinationValue = value;
            carPlayDestinationUnit = unit;
            carPlayDestinationValid = valid;
            super.setDistanceToDestination(value, unit, valid);
        }
    }
}
