package de.audi.tghu.navi.app.cluster;

import com.luka.carplay.core.AltScreenModule;

import de.audi.atip.log.LogChannel;
import de.audi.atip.mmicombi.IViewSizeManager;
import de.audi.tghu.command.ICommandListFactory;
import de.audi.tghu.navi.app.NavigationEnv;
import de.audi.tghu.navi.app.OperationManager;
import de.audi.tghu.navi.app.SpeechManager;
import de.audi.tghu.navi.app.audio.AudioStateMachine;
import de.audi.tghu.navi.app.map.MapManager;

/**
 * Stock BAP boundary for CarPlay KDK composition and the steering-wheel roller.
 *
 * The instrument cluster sends wheel rotation as Navigation BAP MapScale.steps.  Stock
 * CombiBAPListener.setMapScale() applies it to mapKombi via activeContext.increment(400476,
 * steps).  While altScreen owns a Virtual Cockpit we translate the signed step to CarPlay's
 * virtual knob and acknowledge the BAP request with the unchanged native scale.  While a
 * MOST cluster's MAP view shows the phone's map, the steps zoom that map instead
 * (ClusterVideo.zoom), again with stock's unchanged scale as the answer.  Otherwise the stock
 * method runs unchanged.  The cluster's map colour (Light / Dark / Automatic) also sets the
 * phone's MOST cluster map theme (ClusterVideo).
 */
public final class AltScreenCombiBAPListener extends CombiBAPListener {
    private final MapManager maps;

    public AltScreenCombiBAPListener(
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

    public void setMapPresentation(boolean largeMapView, boolean leftMenu, boolean rightMenu) {
        com.luka.carplay.cluster.ClusterLayerController.onVcPresentation(largeMapView);
        super.setMapPresentation(largeMapView, leftMenu, rightMenu);
    }

    public void setMapScale(int steps) {
        if (AltScreenModule.isAltScreenCluster()) {
            AltScreenModule.consumeSteeringWheelScale(steps);
            /* Complete the BAP SetGet with the unchanged native-map status. */
            this.updateMapScale();
            return;
        }
        if (com.luka.carplay.cluster.ClusterVideo.getInstance().zoom(steps)) steps = 0;
        super.setMapScale(steps);
    }

    public void setMapScaleSetting(int setting) {
        if (AltScreenModule.isAltScreenCluster()) {
            /* This is autoZoom, not the steering-wheel OK key.  Do not mutate
             * the hidden Audi map; setMapScale() sends the one Status ACK. */
            return;
        }
        super.setMapScaleSetting(setting);
    }
}
