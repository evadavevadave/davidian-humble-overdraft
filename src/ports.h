/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Davidian. See LICENSE and NOTICE.md. */
/* Port indices, shared by the plugin and its GUI. They must match bundle/humble_overdraft.ttl. */
#ifndef PORTS_H
#define PORTS_H

#define HO_URI    "urn:davidian:humble-overdraft"
#define HO_UI_URI "urn:davidian:humble-overdraft#ui"

enum {
    PORT_IN, PORT_OUT,
    /* Clean channel (front panel) */
    PORT_VOLUME, PORT_TREBLE, PORT_MIDDLE, PORT_BASS, PORT_MASTER,
    /* Overdrive channel (front panel) */
    PORT_DRIVE, PORT_OD_TREBLE, PORT_OD_MIDDLE, PORT_OD_BASS, PORT_OD_LEVEL,
    PORT_OD_TRIM,  /* internal trim pot */
    PORT_PRESENCE,
    PORT_OVERDRIVE, /* channel relay: 0 clean, 1 overdrive */
    PORT_CAB,       /* 0 line (speaker terminal), 1 speaker emulation */
    PORT_VOLTS, PORT_OUTPUT, PORT_ENABLED,
    PORT_LATENCY,   /* output */
    PORT_COUNT
};

#endif
