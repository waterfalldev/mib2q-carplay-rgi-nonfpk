package de.audi.tghu.navi.app.cluster;

import de.audi.atip.log.LogChannel;
import de.audi.atip.mmicombi.IViewSizeManager;
import de.audi.tghu.command.ICommandListFactory;
import de.audi.tghu.navi.app.NavigationEnv;
import de.audi.tghu.navi.app.OperationManager;
import de.audi.tghu.navi.app.SpeechManager;
import de.audi.tghu.navi.app.audio.AudioStateMachine;
import de.audi.tghu.navi.app.map.MapManager;

/**
 * Stock BAP boundary for CarPlay KDK composition.
 *
 * VC's Fct44 (KDK visibility) and Fct54 (map presentation/stage) are forwarded to
 * the layer controller before stock acknowledges them.  The steering-wheel roller
 * (setMapScale) zooms stock's native map, except while a MOST cluster's MAP view
 * shows the phone's map: then its steps zoom that map (ClusterVideo.zoom), and stock
 * still answers the request, with its own map's unchanged scale.  The cluster's map colour
 * (Light / Dark / Automatic) also sets the phone's cluster map theme (ClusterVideo).
 */
public final class ScreenCombiBAPListener extends CombiBAPListener {
    private final MapManager maps;

    public ScreenCombiBAPListener(
        ClusterService service,
        LogChannel logChannel,
        NavigationEnv env,
        SpeechManager speechManager,
        OperationManager operationManager,
        AudioStateMachine audioStateMachine,
        MapManager mapManager,
        ICommandListFactory commandListFactory,
        IViewSizeManager viewSizeManager
    ) {
        super(
            service,
            logChannel,
            env,
            speechManager,
            operationManager,
            audioStateMachine,
            mapManager,
            commandListFactory,
            viewSizeManager
        );
        this.maps = mapManager;
    }

    /** Start-up: stock seeds the cluster from the head unit's saved cluster map setup. */
    public void initFromSetup() {
        super.initFromSetup();
        try {
            com.luka.carplay.cluster.ClusterVideo.getInstance().setClusterColour(this.maps.getSetupKombi().getDayNightView());
        } catch (Throwable t) { /* stock's start-up must never fail on ours */ }
    }

    /** The driver changed the cluster's map colour (BAP MapColorAndType: 0 day, 1 night, 2 auto). */
    public void setMapColor(int colour) {
        super.setMapColor(colour);
        com.luka.carplay.cluster.ClusterVideo.getInstance().setClusterColour(colour);
    }

    /** Apply the accepted stock state before its Status acknowledgement. This boundary
     * also covers internal supplementary visibility changes and initial Status replay,
     * which bypass the two-argument BAP request setter. */
    protected void updateMapVisibility() {
        com.luka.carplay.cluster.ClusterLayerController.onVcVisibility(this.supplementaryMapViewVisible);
        super.updateMapVisibility();
    }

    public void setMapScale(int steps) {
        if (com.luka.carplay.cluster.ClusterVideo.getInstance().zoom(steps)) steps = 0;
        super.setMapScale(steps);
    }

    public void setMapPresentation(boolean largeMapView, boolean leftMenu, boolean rightMenu) {
        com.luka.carplay.cluster.ClusterLayerController.onVcPresentation(largeMapView);
        super.setMapPresentation(largeMapView, leftMenu, rightMenu);
    }
}
