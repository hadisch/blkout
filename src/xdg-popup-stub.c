/*
 * xdg-popup-stub.c — Minimaler Stub für xdg_popup_interface
 *
 * Das wlr-layer-shell-Protokoll referenziert xdg_popup im get_popup-Request.
 * blkout verwendet diesen Request nicht, aber der Linker benötigt das Symbol.
 * Dieses Stub-Modul stellt das Symbol bereit, ohne das vollständige
 * xdg-shell-Protokoll einbinden zu müssen.
 */

#include <wayland-client-core.h>

/* Minimale Interface-Beschreibung — Felder method/event werden nicht benötigt,
 * da get_popup von blkout nie aufgerufen wird */
const struct wl_interface xdg_popup_interface = {
    "xdg_popup",  /* Protokollname */
    6,            /* Protokollversion */
    0, NULL,      /* keine Requests definiert (wir rufen sie nicht auf) */
    0, NULL       /* keine Events definiert */
};
