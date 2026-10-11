package de.audi.app.combi.bap.app.audio;

import de.audi.app.combi.bap.app.kombipictures.IPictureProvider;
import de.audi.app.combi.bap.app.kombipictures.IPictureManager;
import java.util.HashMap;
import java.util.Map;
import org.dsi.ifc.global.ResourceLocator;

class AppConnectorTerminalMode$CoverArtProvider implements IPictureProvider {

    private final Map id2pictureURL;
    private final IPictureManager pictureManager;

    AppConnectorTerminalMode$CoverArtProvider(IPictureManager pictureManager) {
        this.pictureManager = pictureManager;
        this.id2pictureURL = new HashMap();
    }

    public void requestPicture(long entryID) {
        this.requestPicture(entryID, 255);
    }

    public void requestPicture(long entryID, int sourceType) {
        Long key = new Long(entryID);
        if (this.id2pictureURL.containsKey(key)) {
            this.pictureManager.responseCoverArt(
                entryID, sourceType, (ResourceLocator) this.id2pictureURL.get(key), false);
        }
    }

    public void addToMapping(long entryID, ResourceLocator rl) {
        this.id2pictureURL.put(new Long(entryID), rl);
    }

    public boolean hasPicture(long entryID) {
        return this.id2pictureURL.containsKey(new Long(entryID));
    }

    public void clearMapping() {
        this.id2pictureURL.clear();
    }
}
