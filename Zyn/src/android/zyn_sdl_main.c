/* SDLActivity owns Android's Activity, native window, input and lifecycle.
 * SDL invokes SDL_main on its application thread after installing JNI state.
 * Vyx emits the C main(argc, argv) ABI, including argCount/getArg initialization.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <android/log.h>
#include <unistd.h>

extern int main(int argc, char **argv);

SDL_DECLSPEC int SDLCALL SDL_main(int argc, char **argv)
{
    const char *storage = SDL_GetAndroidInternalStoragePath();
    if (!storage || chdir(storage) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, "Zyn", "Cannot enter app internal storage: %s",
                            storage ? storage : SDL_GetError());
        return 1;
    }
    __android_log_print(ANDROID_LOG_INFO, "Zyn", "Starting Vyx application (SDLActivity)");
    const int result = main(argc, argv);
    __android_log_print(ANDROID_LOG_INFO, "Zyn", "Vyx application returned %d", result);
    return result;
}
