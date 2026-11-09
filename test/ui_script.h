/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Davidian. See LICENSE and NOTICE.md. */
/* The GUI checks shared by ui_check.c (Win32) and ui_check_mac.m (Cocoa). The including
 * file defines check(), the last_port/last_value/writes globals filled by its write
 * callback, and these mouse helpers in panel coordinates (top-left origin):
 *   click(w, x, y), drag(w, x, y, dy), dblclick(w, x, y), wheel_up(w, x, y). */
#ifndef UI_SCRIPT_H
#define UI_SCRIPT_H

#include <math.h>
#include <stdio.h>

#include "../src/ports.h"

/* Port values as a host sends them on opening: the defaults in humble_overdraft.ttl. */
static const float UI_DEFAULTS[PORT_COUNT] = {
    0, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.6f, 0.5f, 0.5f, 0.5f, 0.35f, 0.5f, 0.3f, 0, 1, 1.0f, 0, 1, 0
};

static void ui_script(WIDGET_T w)
{
    char label[160];

    /* Knobs move 1/180 of their travel per pixel. */
    drag(w, 76, 165, 36);
    snprintf(label, sizeof label, "dragging Clean Volume down 36 px sends port %d = %.3f", last_port, last_value);
    check(label, last_port == PORT_VOLUME && fabsf(last_value - 0.3f) < 0.01f);

    drag(w, 556, 395, -400);
    snprintf(label, sizeof label, "dragging OD Level far up stops at %.3f", last_value);
    check(label, last_port == PORT_OD_LEVEL && last_value == 1.0f);

    drag(w, 76, 395, 1);
    dblclick(w, 76, 395);
    snprintf(label, sizeof label, "double-click resets Drive to %.2f", last_value);
    check(label, last_port == PORT_DRIVE && fabsf(last_value - 0.6f) < 1e-4f);

    click(w, 800, 165 - 18);
    check("top of the speaker switch selects Line", last_port == PORT_CAB && last_value == 0.0f);
    click(w, 800, 165 + 18);
    check("bottom of the speaker switch selects Cab", last_port == PORT_CAB && last_value == 1.0f);
    click(w, 830, 425);
    check("overdrive footswitch selects the overdrive channel", last_port == PORT_OVERDRIVE && last_value == 1.0f);
    click(w, 965, 425);
    check("amp on footswitch bypasses", last_port == PORT_ENABLED && last_value == 0.0f);

    drag(w, 1000, 158, 18);
    snprintf(label, sizeof label, "dragging Output trim down sends %.2f dB", last_value);
    check(label, last_port == PORT_OUTPUT && last_value < -3.0f && last_value > -4.2f);
    drag(w, 696, 412, -18);
    snprintf(label, sizeof label, "dragging OD Trim up sends %.3f", last_value);
    check(label, last_port == PORT_OD_TRIM && fabsf(last_value - 0.6f) < 0.01f);

    writes = 0;
    wheel_up(w, 196, 165);
    snprintf(label, sizeof label, "mouse wheel on Clean Treble sends %.3f", last_value);
    check(label, writes == 1 && last_port == PORT_TREBLE && fabsf(last_value - 0.525f) < 1e-4f);
    click(w, 5, 5);
    check("clicking empty panel sends nothing new", writes == 1);
}

#endif
