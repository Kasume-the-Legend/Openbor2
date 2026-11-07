package org.openbor.engine;

import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Iterator;

/**
 * Turtle Mania story updater. Downloads fresh AI-written dialogue pools from the tmnt-story
 * Cloudflare Worker and drops them next to the game data. OpenBOR reads loose files before the
 * .pak, and the engine's working directory is getExternalFilesDir(null), so no engine change is
 * needed. Any failure (offline, server down) is ignored: the game keeps its built-in story.
 */
public class StoryUpdater {
    private static final String TAG = "StoryUpdater";
    public static final String URL_BASE = "https://tmnt-story.acoffey412.workers.dev";

    /** Runs the download on a worker thread and waits at most maxWaitMs, so startup is never blocked for long. */
    public static void updateBlocking(final Context ctx, long maxWaitMs) {
        Thread t = new Thread(new Runnable() {
            @Override public void run() { try { update(ctx); } catch (Throwable e) { Log.w(TAG, "update failed: " + e); } }
        });
        t.start();
        try { t.join(maxWaitMs); } catch (InterruptedException ignored) { }
    }

    static void update(Context ctx) throws Exception {
        File root = ctx.getExternalFilesDir(null);
        if (root == null) return;
        File vfile = new File(root, "tm_story_version.txt");
        long since = 0;
        try {
            if (vfile.exists()) {
                byte[] b = readAll(new java.io.FileInputStream(vfile));
                since = Long.parseLong(new String(b, "UTF-8").trim());
            }
        } catch (Exception ignored) { }

        HttpURLConnection c = (HttpURLConnection) new URL(URL_BASE + "/pack?since=" + since).openConnection();
        c.setConnectTimeout(4000);
        c.setReadTimeout(5000);
        c.setRequestProperty("User-Agent", "tmnt-last-ronin-android");
        if (c.getResponseCode() != 200) return;
        JSONObject pack = new JSONObject(new String(readAll(c.getInputStream()), "UTF-8"));
        JSONObject files = pack.getJSONObject("files");
        int n = 0;
        Iterator<String> it = files.keys();
        while (it.hasNext()) {
            String rel = it.next();
            if (!rel.matches("data/cutscenes/tm/pools/[A-Za-z0-9_]+\\.txt")) continue;
            if (!validPool(files.getString(rel))) { Log.w(TAG, "rejected malformed pool " + rel); continue; }
            File dst = new File(root, rel);
            File dir = dst.getParentFile();
            if (dir != null && !dir.exists() && !dir.mkdirs()) continue;
            FileOutputStream out = new FileOutputStream(dst);
            try { out.write(files.getString(rel).getBytes("UTF-8")); } finally { out.close(); }
            n++;
        }
        FileOutputStream vo = new FileOutputStream(vfile);
        try { vo.write(String.valueOf(pack.optLong("version", since)).getBytes("UTF-8")); } finally { vo.close(); }
        Log.d(TAG, "Turtle Mania story updated: " + n + " pools");
    }

    /** A pool is "<count>\n" then exactly that many lines "say <face> <NAME> <row1> <row2> <row3>", rows of at most 29 characters. */
    static boolean validPool(String body) {
        if (body == null) return false;
        String[] ln = body.split("\n");
        int n;
        try { n = Integer.parseInt(ln[0].trim()); } catch (Exception e) { return false; }
        if (n < 1) return false;
        int seen = 0;
        for (int i = 1; i < ln.length; i++) {
            String l = ln[i].replace("\r", "");
            if (l.trim().length() == 0) continue;
            String[] t = l.split(" ", -1);
            if (t.length != 6 || !t[0].equals("say") || t[1].length() == 0 || t[2].length() == 0) return false;
            for (int k = 3; k < 6; k++) if (t[k].length() == 0 || t[k].length() > 29) return false;
            seen++;
        }
        return seen == n;
    }

    private static byte[] readAll(InputStream in) throws Exception {
        try {
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int r;
            while ((r = in.read(buf)) > 0) bo.write(buf, 0, r);
            return bo.toByteArray();
        } finally { in.close(); }
    }
}
