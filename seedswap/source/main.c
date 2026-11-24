// SPDX-License-Identifier: GPL-2.0-only
// SeedSwap for kaiHEN: turns your own game dumps into games the on-console
// modloader runs in place of Seedsow Lullaby, one save per game.
//
// The Seedsow launcher's B button installs this file from the exploit save
// to SD:/switch/seedswap.nro. Saves are only ever written for the play user;
// the users the loader protects (and the others in users.h) are refused in
// users.c and again in the platform layer.
#include "gfx.h"
#include "platform.h"
#include "ui.h"
#include "util.h"

int main(void) {
    if (!plat_init()) return 1;
    app_log("SeedSwap %s started", SEEDSWAP_VERSION);
    ui_init();
    while (plat_loop()) {
        if (!ui_input(plat_buttons_down())) break;
        Canvas *c = plat_frame_begin();
        gfx_begin(c);
        ui_draw();
        plat_frame_end();
    }
    ui_exit();
    app_log("SeedSwap closed");
    plat_exit();
    return 0;
}
