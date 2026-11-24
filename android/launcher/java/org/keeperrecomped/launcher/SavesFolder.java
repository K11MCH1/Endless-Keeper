package org.keeperrecomped.launcher;

import android.content.Context;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;

/**
 * Advanced > Saves folder (Endless Keeper, 2026-11-24): the game saves into a folder the player chooses on the phone's
 * storage or an SD card instead of inside the app, so the saves outlive reinstalling the app and can be backed up or
 * copied to a PC. The game writes there itself (the engine's -savedir), which needs Android's "All files access".
 * Play Original keeps its own saves (ClassicRoot).
 */
final class SavesFolder {
    private SavesFolder() {}

    static final String PREF = "saves_dir";

    /** The chosen folder's path, or "" for the saves inside the app. */
    static String get(Context c) {
        return c.getSharedPreferences("launcher", Context.MODE_PRIVATE).getString(PREF, "");
    }

    /** Android's "All files access" for this app (the game can open files anywhere on the storage by path). */
    static boolean access() {
        return Environment.isExternalStorageManager();
    }

    /** The saves inside the app (KeeperFX's own save folder). */
    static File appSaves(Context c) {
        return new File(Paths.kfx(c), "save");
    }

    /**
     * The folder's path from a tree the system folder picker gave, or null if it is not a folder on the phone's
     * storage or an SD card (a cloud provider's folder has no path the game could open).
     */
    static String pathOf(Uri tree) {
        if (!"com.android.externalstorage.documents".equals(tree.getAuthority()))
            return null;
        String id;
        try {
            id = DocumentsContract.getTreeDocumentId(tree);
        } catch (Exception e) {
            return null;
        }
        int colon = id.indexOf(':');
        if (colon < 0)
            return null;
        String volume = id.substring(0, colon), rel = id.substring(colon + 1);
        String base = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory().getPath() : "/storage/" + volume;
        return rel.isEmpty() ? base : base + "/" + rel;
    }

    /** Copies the files of `from` that `to` does not have yet (a save is never overwritten); returns how many. */
    static int copyMissing(File from, File to) throws IOException {
        File[] files = from.listFiles();
        if (files == null)
            return 0;
        if (!to.isDirectory() && !to.mkdirs())
            throw new IOException("can't make " + to);
        int n = 0;
        for (File f : files) {
            if (!f.isFile())
                continue;
            File dst = new File(to, f.getName());
            if (dst.exists())
                continue;
            Files.copy(f.toPath(), dst.toPath());
            n++;
        }
        return n;
    }
}
