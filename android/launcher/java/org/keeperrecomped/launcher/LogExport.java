package org.keeperrecomped.launcher;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Advanced > Maintenance > Export log (Endless Keeper, 2026-11-23): one zip in Download with what a bug report needs,
 * so players can send it when something goes wrong: the game's logs (files/kfx/*.log, Play Original's in
 * files/original), the newest crash report if the game left one (crashes/), the launcher's own log, and a note of
 * the app, device and Android versions. The game starts a fresh keeperfx.log at every start, so this is the log of
 * the game played last (or the one still running). The PC launcher's background game once deleted keeperfx.log;
 * Android has no background game, so nothing else touches it here.
 * Written through MediaStore like Export KeeperFX data: no storage permission, and the file stays the player's.
 */
final class LogExport {
    private LogExport() {}

    /** The zip in Download: its content uri (to share) and the path to show the player. */
    static final class Result {
        final Uri uri;
        final String shown;

        Result(Uri uri, String shown) {
            this.uri = uri;
            this.shown = shown;
        }
    }

    private static final class Item {
        final File file;
        final String name; // inside the zip

        Item(File file, String name) {
            this.file = file;
            this.name = name;
        }
    }

    /** @return true when the game has written a log (the launcher's own log alone is not worth a zip) */
    static boolean hasGameLogs(Context c) {
        List<Item> items = new ArrayList<>();
        gameLogs(c, items);
        return !items.isEmpty();
    }

    static Result write(Context c) throws IOException {
        List<Item> items = new ArrayList<>();
        gameLogs(c, items);
        File logs = new File(c.getFilesDir(), "logs");
        addIfFile(items, new File(logs, "launcher.log"), "launcher/launcher.log");
        addIfFile(items, new File(logs, "launcher.log.1"), "launcher/launcher.log.1");

        String label = c.getApplicationInfo().loadLabel(c.getPackageManager()).toString();
        String name = label + " log " + new SimpleDateFormat("yyyy-MM-dd HH-mm-ss", Locale.ROOT).format(new Date()) + ".zip";
        ContentResolver resolver = c.getContentResolver();
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
        v.put(MediaStore.MediaColumns.MIME_TYPE, "application/zip");
        v.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
        if (uri == null)
            throw new IOException("Cannot create " + name + " in Download");
        try (OutputStream o = resolver.openOutputStream(uri)) {
            if (o == null)
                throw new IOException("Cannot write " + name + " in Download");
            try (ZipOutputStream zip = new ZipOutputStream(o)) {
                zip.putNextEntry(new ZipEntry("about.txt"));
                zip.write(about(c, items).getBytes(StandardCharsets.UTF_8));
                zip.closeEntry();
                byte[] buf = new byte[1 << 16];
                for (Item it : items) {
                    ZipEntry e = new ZipEntry(it.name);
                    e.setTime(it.file.lastModified());
                    zip.putNextEntry(e);
                    // A game still running keeps writing: take what is there now.
                    try (InputStream in = new FileInputStream(it.file)) {
                        int r;
                        while ((r = in.read(buf)) > 0)
                            zip.write(buf, 0, r);
                    }
                    zip.closeEntry();
                }
            }
        } catch (IOException e) {
            resolver.delete(uri, null, null);
            throw e;
        }
        ContentValues ready = new ContentValues();
        ready.put(MediaStore.MediaColumns.IS_PENDING, 0);
        resolver.update(uri, ready, null, null);
        AppLog.i("log exported: Download/" + name + " (" + items.size() + " files)");
        return new Result(uri, "Download/" + name);
    }

    /** The game folders' *.log files and the newest crash report (all files of its name: dump and log copy). */
    private static void gameLogs(Context c, List<Item> out) {
        topLogs(Paths.kfx(c), "", out);
        topLogs(Paths.original(c), "original/", out);
        newestCrash(new File(Paths.kfx(c), "crashes"), "crashes/", out);
        newestCrash(new File(Paths.original(c), "crashes"), "original/crashes/", out);
    }

    private static void topLogs(File dir, String prefix, List<Item> out) {
        File[] list = dir.listFiles();
        if (list == null)
            return;
        for (File f : list)
            if (f.getName().endsWith(".log"))
                addIfFile(out, f, prefix + f.getName());
    }

    private static void newestCrash(File dir, String prefix, List<Item> out) {
        // Play Original's folder links KeeperFX's folders (ClassicRoot): a linked crashes/ is the same reports again.
        if (java.nio.file.Files.isSymbolicLink(dir.toPath()))
            return;
        File[] list = dir.listFiles();
        if (list == null)
            return;
        File newest = null;
        for (File f : list)
            if (f.isFile() && (newest == null || f.lastModified() > newest.lastModified()))
                newest = f;
        if (newest == null)
            return;
        String stem = stem(newest.getName());
        for (File f : list)
            if (stem(f.getName()).equals(stem))
                addIfFile(out, f, prefix + f.getName());
    }

    private static String stem(String name) {
        int dot = name.indexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    private static void addIfFile(List<Item> out, File f, String name) {
        if (f.isFile() && f.length() > 0)
            out.add(new Item(f, name));
    }

    private static String about(Context c, List<Item> items) {
        StringBuilder s = new StringBuilder();
        s.append(c.getApplicationInfo().loadLabel(c.getPackageManager()));
        try {
            PackageInfo p = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            s.append(' ').append(p.versionName).append(" (").append(p.getLongVersionCode()).append(')');
        } catch (Exception ignored) {
        }
        s.append('\n').append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(", Android ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        s.append("Exported ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.ROOT).format(new Date())).append("\n\n");
        SimpleDateFormat when = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT);
        for (Item it : items)
            s.append(it.name).append("  ").append(it.file.length()).append(" bytes, last written ")
                    .append(when.format(new Date(it.file.lastModified()))).append('\n');
        return s.toString();
    }
}
