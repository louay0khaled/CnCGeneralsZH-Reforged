package org.reforged.zero.hour;

import org.libsdl.app.SDLActivity;

/**
 * Thin SDL3 Android shell. Native entry point is libmain.so -> SDL_main().
 */
public class GeneralsActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }
}
