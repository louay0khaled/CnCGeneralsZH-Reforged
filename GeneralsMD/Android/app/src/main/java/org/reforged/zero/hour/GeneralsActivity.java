package org.reforged.zero.hour;

import org.libsdl.app.SDLActivity;

/**
 * Actual game activity. It is started only after SetupActivity has validated and persisted
 * the player's Zero Hour installation path.
 */
public final class GeneralsActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
