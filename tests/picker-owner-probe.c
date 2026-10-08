/* C6.76 (Windows, run under wine64 on Xvfb): the ROM chooser is a dialog OWNED by the game
 * window, runs in its own thread so the loop of the game keeps running while it is open, and
 * gives the owner back when it closes. A plain top-level window stands in for the game window.
 *
 *   picker-owner-probe
 *
 * Phase 1: begin_child(owner) -> attached == 1; the #32770 dialog appears with GW_OWNER == our
 * window; our window is disabled while it is open; the loop iterates meanwhile; WM_CLOSE on the
 * dialog gives OD_PICK_CANCEL, and the window is enabled again.
 * Phase 2: begin_child(NULL) -> attached == 0 (the front end would minimise); the dialog has no
 * owner. */
#include "opendana/rom_picker.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static HWND g_dialog;

static BOOL CALLBACK find_dialog(HWND w, LPARAM want_owner)
{
    char cls[64];
    GetClassNameA(w, cls, sizeof cls);
    if (strcmp(cls, "#32770") == 0 && IsWindowVisible(w) &&
        GetWindow(w, GW_OWNER) == (HWND)want_owner) {
        g_dialog = w;
        return FALSE;
    }
    return TRUE;
}

static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Run one dialog: returns the iterations of the loop while it was open. */
static long run_phase(HWND game, int with_owner, od_pick_result *result)
{
    od_rom_picker p;
    od_picker_parent parent = { 0, game };
    od_rom_picker_begin_child(&p, NULL, with_owner ? &parent : NULL);
    EXPECT(p.attached == (with_owner ? 1 : 0), "attached is %d", p.attached);
    char path[1024], err[160] = {0};
    long iterations = 0, at_open = -1, closed_at = -1;
    DWORD t0 = GetTickCount();
    g_dialog = NULL;
    *result = OD_PICK_PENDING;
    while (GetTickCount() - t0 < 20000) {
        pump();
        iterations++;
        *result = od_rom_picker_poll(&p, path, sizeof path, err, sizeof err);
        if (*result != OD_PICK_PENDING)
            break;
        if (g_dialog == NULL) {
            EnumWindows(find_dialog, (LPARAM)(with_owner ? game : NULL));
            if (g_dialog != NULL)
                at_open = iterations;
        } else if (iterations - at_open > 60 && closed_at < 0) {
            if (with_owner)
                EXPECT(!IsWindowEnabled(game), "the game window is enabled while its dialog is open");
            PostMessageA(g_dialog, WM_CLOSE, 0, 0);
            closed_at = iterations;
        }
        Sleep(10);
    }
    EXPECT(g_dialog != NULL, "no dialog of class #32770 %s appeared", with_owner ? "owned by the game window" : "");
    EXPECT(at_open >= 0 && closed_at - at_open > 60, "the loop did not run while the dialog was open");
    printf("phase owner=%d: dialog %s, loop iterations while open: %ld, result %d\n", with_owner,
           g_dialog ? "found" : "NOT found", closed_at - at_open, (int)*result);
    return closed_at - at_open;
}

int main(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "OpenDanaProbe";
    RegisterClassA(&wc);
    HWND game = CreateWindowA("OpenDanaProbe", "OpenDana", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              50, 50, 400, 300, NULL, NULL, wc.hInstance, NULL);
    EXPECT(game != NULL, "cannot create the stand-in game window");
    pump();
    od_pick_result r1, r2;
    run_phase(game, 1, &r1);
    EXPECT(r1 == OD_PICK_CANCEL, "closing the owned dialog gave %d, wanted CANCEL", (int)r1);
    EXPECT(IsWindowEnabled(game), "the game window stayed disabled after the dialog closed");
    run_phase(game, 0, &r2);
    EXPECT(r2 == OD_PICK_CANCEL, "closing the dialog without owner gave %d, wanted CANCEL", (int)r2);
    printf(fails ? "picker-owner: %d FAILURE(S)\n"
                 : "picker-owner: the dialog is owned by the game window, the loop runs, CANCEL and re-enabled\n", fails);
    return fails ? 1 : 0;
}
