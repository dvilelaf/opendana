/* Synthetic PE for the package inspector's controls. This is NOT the game. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#ifndef OD_PROBE_COMMIT
#define OD_PROBE_COMMIT "unknown"
#endif
int main(void)
{
    SDL_version version;
    SDL_GetVersion(&version);
    puts("probe (commit " OD_PROBE_COMMIT ")");
    return version.major != 2;
}
