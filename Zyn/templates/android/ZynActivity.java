package org.vyx.zyn;

import android.os.Bundle;
import android.util.Log;
import org.libsdl.app.SDLActivity;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/** SDLActivity owns the Surface, JNI callbacks, input and native application thread. */
public class ZynActivity extends SDLActivity {
    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "@@NATIVE@@" };
    }

    @Override protected String[] getArguments() { return new String[] { "--visible" }; }

    private void installAssetAs(String asset, String relativePath) throws IOException {
        File destination = new File(getFilesDir(), relativePath);
        File parent = destination.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("Cannot create " + parent);
        }
        // Framework code reads these files through std.fs. They are application assets,
        // never user state: refresh them on launch after an APK update.
        try (InputStream input = getAssets().open(asset);
             FileOutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[16384];
            int count;
            while ((count = input.read(buffer)) != -1) { output.write(buffer, 0, count); }
        }
    }

    private void installAsset(String asset) throws IOException {
        installAssetAs(asset, asset);
    }

    private void installProjectAsset(String source, String relativePath) throws IOException {
        String[] children = getAssets().list(source);
        if (children != null && children.length != 0) {
            for (String child : children) {
                installProjectAsset(source + "/" + child, relativePath + "/" + child);
            }
            return;
        }
        installAssetAs(source, relativePath);
    }

    private void installProjectAssets() throws IOException {
        String[] children = getAssets().list("zyn/assets");
        if (children == null) { return; }
        for (String child : children) {
            installProjectAsset("zyn/assets/" + child, "assets/" + child);
        }
    }

    @Override protected void onCreate(Bundle state) {
        try {
            installAsset("Zyn.runtime.toml");
            installAsset("nut/solid.slang");
            installProjectAssets();
            File cache = new File(getFilesDir(), "target");
            if (!cache.isDirectory() && !cache.mkdirs()) { throw new IOException("Cannot create " + cache); }
        } catch (IOException error) {
            throw new IllegalStateException("Zyn assets are missing or cannot be installed", error);
        }
        super.onCreate(state);
        Log.i("Zyn", "Activity created");
    }

    @Override protected void onPause() { super.onPause(); Log.i("Zyn", "Activity paused"); }
    @Override protected void onResume() { super.onResume(); Log.i("Zyn", "Activity resumed"); }
    @Override protected void onDestroy() { super.onDestroy(); Log.i("Zyn", "Activity destroyed"); }
}
