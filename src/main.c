/*
 * blkout — Bildschirm-Abdunklungs-Workaround für Wayland
 *
 *			Version:	1.0.0
 *
 * 			Autoren:	Claude (Antropic)
 *						Hans-Dieter Schlabritz <hadisch@zavb.de>
 *						(C) 2026 Hans-Dieter Schlabritz
 *			Lizenz:		GNU GENERAL PUBLIC LICENSE
 *						Version 2, June 1991
 *
 * Zeigt ein schwarzes Vollbild-Overlay über allen Fenstern an.
 * Wird bei Tastendruck oder Mausbewegung wieder geschlossen.
 *
 * Aufruf: blkout [-s <sekunden>] [-e]
 *   -s <n>  : Overlay erst nach n Sekunden Inaktivität anzeigen
 *   -e      : Programm nach erstem Schließen des Overlays beenden
 *
 * Abhängigkeiten: libwayland-client (Laufzeit)
 *                 wlr-layer-shell-unstable-v1 (Protokoll, compiliert rein)
 *                 ext-idle-notify-v1           (Protokoll, compiliert rein)
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

/* Wayland-Kern-API */
#include <wayland-client.h>

/* Generierte Protocol-Bindings (erzeugt vom Makefile via wayland-scanner) */
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"

/* =========================================================================
 * Anwendungszustand
 * ========================================================================= */

typedef struct {
    /* --- Kommandozeilenparameter --- */
    int  timeout_ms;       /* Wartezeit in Millisekunden (0 = sofort anzeigen) */
    bool exit_on_hide;     /* Programm nach Schließen des Overlays beenden */

    /* --- Wayland-Kernobjekte --- */
    struct wl_display    *display;     /* Verbindung zum Compositor */
    struct wl_registry   *registry;   /* Globale Objekte des Compositors */
    struct wl_compositor *compositor;  /* Erstellt wl_surface-Objekte */
    struct wl_shm        *shm;         /* Shared-Memory für Pixeldaten */
    struct wl_seat       *seat;        /* Eingabegeräte-Gruppe */
    struct wl_keyboard   *keyboard;    /* Tastaturereignisse */
    struct wl_pointer    *pointer;     /* Mausereignisse */

    /* --- Layer-Shell-Objekte (für das Overlay-Fenster) --- */
    struct zwlr_layer_shell_v1   *layer_shell;    /* Erzeugt Layer-Surfaces */
    struct zwlr_layer_surface_v1 *layer_surface;  /* Die eigentliche Overlay-Surface */
    struct wl_surface            *surface;         /* Wayland-Surface des Overlays */

    /* --- Idle-Notification-Objekte (für die Timeout-Erkennung) --- */
    struct ext_idle_notifier_v1    *idle_notifier;    /* Manager-Objekt */
    struct ext_idle_notification_v1 *idle_notification; /* Aktive Benachrichtigung */

    /* --- Shared-Memory-Puffer (schwarzes Pixelbild) --- */
    struct wl_buffer *buffer;     /* Wayland-Puffer-Objekt */
    void             *shm_data;   /* Zeiger auf den gemappten Speicher */
    int               shm_fd;     /* Dateideskriptor des Shared-Memory */
    size_t            shm_size;   /* Größe des Puffers in Bytes */
    int               width;      /* Breite der Surface in Pixeln */
    int               height;     /* Höhe der Surface in Pixeln */

    /* --- Programmzustand --- */
    bool overlay_visible;   /* true = Overlay wird gerade angezeigt */
    bool configured;        /* true = configure-Event empfangen, Größe bekannt */
    bool running;           /* false = Hauptschleife verlassen */
} App;

/* =========================================================================
 * Vorwärtsdeklarationen
 * ========================================================================= */

static void show_overlay(App *app);
static void hide_overlay(App *app);

/* =========================================================================
 * Shared-Memory-Hilfsfunktion
 * =========================================================================
 * Erstellt eine anonyme temporäre Datei als Speicherpuffer.
 * Unter Linux wird memfd_create verwendet, das keinen Dateisystemeintrag
 * hinterlässt und automatisch geschlossen wird.
 */
static int create_shm_file(size_t size)
{
    /* Anonyme In-Memory-Datei erstellen (Linux-spezifisch) */
    int fd = memfd_create("blkout-shm", MFD_CLOEXEC);
    if (fd < 0) {
        perror("memfd_create");
        return -1;
    }

    /* Datei auf die gewünschte Größe bringen */
    if (ftruncate(fd, (off_t)size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }

    return fd;
}

/* =========================================================================
 * Puffer erstellen
 * =========================================================================
 * Allociert einen Shared-Memory-Puffer mit den gegebenen Abmessungen und
 * füllt ihn komplett schwarz. Gibt true zurück bei Erfolg.
 */
static bool create_buffer(App *app)
{
    /* Größe berechnen: 4 Bytes pro Pixel (Format XRGB8888) */
    app->shm_size = (size_t)(app->width * app->height * 4);

    /* Shared-Memory-Dateideskriptor erzeugen */
    app->shm_fd = create_shm_file(app->shm_size);
    if (app->shm_fd < 0)
        return false;

    /* Speicher in den Prozessadressraum einblenden */
    app->shm_data = mmap(NULL, app->shm_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED, app->shm_fd, 0);
    if (app->shm_data == MAP_FAILED) {
        perror("mmap");
        close(app->shm_fd);
        app->shm_fd = -1;
        return false;
    }

    /* Alle Pixel auf Schwarz setzen (0x00000000 im Format XRGB8888 = schwarz) */
    memset(app->shm_data, 0, app->shm_size);

    /* Wayland-SHM-Pool aus dem Dateideskriptor erstellen */
    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, app->shm_fd,
                                                   (int32_t)app->shm_size);
    if (!pool) {
        fprintf(stderr, "wl_shm_create_pool fehlgeschlagen\n");
        munmap(app->shm_data, app->shm_size);
        close(app->shm_fd);
        return false;
    }

    /* Puffer-Objekt aus dem Pool erzeugen */
    app->buffer = wl_shm_pool_create_buffer(pool, 0,
                                             app->width, app->height,
                                             app->width * 4,
                                             WL_SHM_FORMAT_XRGB8888);
    /* Pool-Referenz freigeben (Puffer bleibt gültig) */
    wl_shm_pool_destroy(pool);

    if (!app->buffer) {
        fprintf(stderr, "wl_shm_pool_create_buffer fehlgeschlagen\n");
        munmap(app->shm_data, app->shm_size);
        close(app->shm_fd);
        return false;
    }

    return true;
}

/* Puffer freigeben */
static void destroy_buffer(App *app)
{
    if (app->buffer) {
        wl_buffer_destroy(app->buffer);
        app->buffer = NULL;
    }
    if (app->shm_data && app->shm_data != MAP_FAILED) {
        munmap(app->shm_data, app->shm_size);
        app->shm_data = NULL;
    }
    if (app->shm_fd >= 0) {
        close(app->shm_fd);
        app->shm_fd = -1;
    }
}

/* =========================================================================
 * Layer-Surface-Ereignisse
 * =========================================================================
 * Der Compositor teilt uns hier die tatsächliche Größe der Surface mit.
 * Wir müssen daraufhin ack_configure senden und dann einen Puffer anhängen.
 */
static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *surface,
                                    uint32_t serial,
                                    uint32_t width, uint32_t height)
{
    App *app = data;

    /* Größe merken, die der Compositor vorgegeben hat */
    app->width  = (int)width;
    app->height = (int)height;

    /* Configure quittieren — Pflicht vor dem nächsten Commit */
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    if (!app->configured) {
        /* Erster Configure-Event: jetzt den schwarzen Puffer erstellen und anhängen */
        app->configured = true;

        if (!create_buffer(app)) {
            fprintf(stderr, "Puffer konnte nicht erstellt werden\n");
            app->running = false;
            return;
        }

        /* Puffer an die Surface binden und einreichen */
        wl_surface_attach(app->surface, app->buffer, 0, 0);
        wl_surface_commit(app->surface);
    } else {
        /*
         * Nachfolgende Configure-Events (z.B. bei Größenänderung durch den
         * Compositor): alten Puffer freigeben, neuen erstellen.
         */
        destroy_buffer(app);
        if (!create_buffer(app)) {
            fprintf(stderr, "Puffer-Neuerstellen fehlgeschlagen\n");
            app->running = false;
            return;
        }
        wl_surface_attach(app->surface, app->buffer, 0, 0);
        wl_surface_commit(app->surface);
    }
}

/* Compositor signalisiert, dass die Surface geschlossen werden soll */
static void layer_surface_closed(void *data,
                                  struct zwlr_layer_surface_v1 *surface)
{
    (void)surface;
    App *app = data;
    /* Overlay von unserer Seite aus abbauen */
    hide_overlay(app);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* =========================================================================
 * Overlay anzeigen
 * =========================================================================
 * Erstellt eine neue Layer-Surface, die vollflächig über allen anderen
 * Fenstern liegt, und sendet ein erstes Commit, um den Configure-Event
 * des Compositors auszulösen.
 */
static void show_overlay(App *app)
{
    /* Nichts tun, wenn das Overlay bereits sichtbar ist */
    if (app->overlay_visible)
        return;

    /* Neue Wayland-Surface erstellen */
    app->surface = wl_compositor_create_surface(app->compositor);
    if (!app->surface) {
        fprintf(stderr, "wl_compositor_create_surface fehlgeschlagen\n");
        app->running = false;
        return;
    }

    /*
     * Layer-Surface aus der Surface erzeugen.
     * Layer OVERLAY = höchste Ebene, liegt über allen anderen Fenstern.
     * Namespace "blkout" identifiziert das Overlay für den Compositor.
     */
    app->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        app->layer_shell,
        app->surface,
        NULL,                              /* output: NULL = Compositor wählt */
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "blkout"
    );
    if (!app->layer_surface) {
        fprintf(stderr, "get_layer_surface fehlgeschlagen\n");
        wl_surface_destroy(app->surface);
        app->surface = NULL;
        app->running = false;
        return;
    }

    /* Listener für Configure- und Closed-Events registrieren */
    zwlr_layer_surface_v1_add_listener(app->layer_surface,
                                       &layer_surface_listener, app);

    /*
     * Surface an alle vier Bildschirmränder verankern.
     * In Kombination mit set_size(0, 0) füllt die Surface den gesamten
     * Bildschirm aus — der Compositor teilt uns die genaue Größe per
     * Configure-Event mit.
     */
    zwlr_layer_surface_v1_set_anchor(app->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    /* Größe 0×0: Compositor füllt gemäß Anker-Konfiguration auf */
    zwlr_layer_surface_v1_set_size(app->layer_surface, 0, 0);

    /*
     * Exclusive Zone -1: das Overlay überdeckt auch Panels und andere
     * Layer-Shell-Surfaces (z.B. die KDE-Taskleiste).
     */
    zwlr_layer_surface_v1_set_exclusive_zone(app->layer_surface, -1);

    /*
     * EXCLUSIVE Keyboard-Interaktivität: alle Tastatureingaben gehen
     * ausschließlich an unser Overlay, solange es sichtbar ist.
     */
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        app->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);

    /* Zustand zurücksetzen, da wir gleich einen neuen Configure-Event erwarten */
    app->configured = false;

    /*
     * Erstes Commit ohne Puffer: veranlasst den Compositor, uns die
     * tatsächliche Bildschirmgröße per Configure-Event mitzuteilen.
     */
    wl_surface_commit(app->surface);

    /* Zustandsvariable setzen */
    app->overlay_visible = true;
}

/* =========================================================================
 * Overlay entfernen
 * =========================================================================
 * Zerstört die Layer-Surface und den Puffer. Entscheidet anschließend,
 * ob das Programm beendet wird oder von vorne beginnt.
 */
static void hide_overlay(App *app)
{
    /* Nichts tun, wenn das Overlay gar nicht sichtbar ist */
    if (!app->overlay_visible)
        return;

    /* Zustand sofort zurücksetzen, um Doppel-Aufrufe zu verhindern */
    app->overlay_visible = false;
    app->configured      = false;

    /* Layer-Surface zerstören */
    if (app->layer_surface) {
        zwlr_layer_surface_v1_destroy(app->layer_surface);
        app->layer_surface = NULL;
    }

    /* Wayland-Surface zerstören */
    if (app->surface) {
        wl_surface_destroy(app->surface);
        app->surface = NULL;
    }

    /* Pixel-Puffer freigeben */
    destroy_buffer(app);

    /* Ausstehende Requests zum Compositor schicken */
    wl_display_flush(app->display);

    /* -e gesetzt: Programm beenden */
    if (app->exit_on_hide) {
        app->running = false;
        return;
    }

    /*
     * Kein Timeout (-s nicht gesetzt): Overlay sofort wieder anzeigen.
     * Mit Timeout: die Idle-Notification ist automatisch neu gespannt und
     * wird nach erneutem Ablauf wieder feuern — nichts weiter zu tun.
     */
    if (app->timeout_ms == 0)
        show_overlay(app);
}

/* =========================================================================
 * Tastaturereignisse
 * =========================================================================
 * Bei jedem Tastendruck wird das Overlay geschlossen.
 */
static void keyboard_keymap(void *data, struct wl_keyboard *kb,
                             uint32_t format, int32_t fd, uint32_t size)
{
    /* Keymap-Daten werden von uns nicht ausgewertet */
    (void)data; (void)kb; (void)format; (void)size;
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb,
                            uint32_t serial, struct wl_surface *surface,
                            struct wl_array *keys)
{
    /* Fokus erhalten — keine Aktion nötig */
    (void)data; (void)kb; (void)serial; (void)surface; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *kb,
                            uint32_t serial, struct wl_surface *surface)
{
    /* Fokus verloren — keine Aktion nötig */
    (void)data; (void)kb; (void)serial; (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *kb,
                          uint32_t serial, uint32_t time,
                          uint32_t key, uint32_t state)
{
    (void)kb; (void)serial; (void)time; (void)key;
    App *app = data;

    /* Nur beim Drücken (state=1) reagieren, nicht beim Loslassen */
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        hide_overlay(app);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb,
                                uint32_t serial, uint32_t mods_depressed,
                                uint32_t mods_latched, uint32_t mods_locked,
                                uint32_t group)
{
    /* Modifier-Zustände werden nicht ausgewertet */
    (void)data; (void)kb; (void)serial; (void)mods_depressed;
    (void)mods_latched; (void)mods_locked; (void)group;
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb,
                                  int32_t rate, int32_t delay)
{
    /* Wiederholungsrate wird nicht verwendet */
    (void)data; (void)kb; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap      = keyboard_keymap,
    .enter       = keyboard_enter,
    .leave       = keyboard_leave,
    .key         = keyboard_key,
    .modifiers   = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* =========================================================================
 * Mausereignisse
 * =========================================================================
 * Jede Mausbewegung oder Maustaste schließt das Overlay.
 */
static void pointer_enter(void *data, struct wl_pointer *ptr,
                           uint32_t serial, struct wl_surface *surface,
                           wl_fixed_t sx, wl_fixed_t sy)
{
    /* Zeiger betritt unsere Surface — Cursor verstecken */
    (void)data; (void)surface; (void)sx; (void)sy;

    /* Unsichtbaren Cursor setzen: NULL-Surface = kein Cursor */
    wl_pointer_set_cursor(ptr, serial, NULL, 0, 0);
}

static void pointer_leave(void *data, struct wl_pointer *ptr,
                           uint32_t serial, struct wl_surface *surface)
{
    /* Zeiger verlässt unsere Surface — keine Aktion nötig */
    (void)data; (void)ptr; (void)serial; (void)surface;
}

static void pointer_motion(void *data, struct wl_pointer *ptr,
                            uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    /* Mausbewegung erkannt: Overlay schließen */
    (void)ptr; (void)time; (void)sx; (void)sy;
    App *app = data;
    hide_overlay(app);
}

static void pointer_button(void *data, struct wl_pointer *ptr,
                            uint32_t serial, uint32_t time,
                            uint32_t button, uint32_t state)
{
    /* Maustaste gedrückt: Overlay schließen */
    (void)ptr; (void)serial; (void)time; (void)button;
    App *app = data;

    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        hide_overlay(app);
}

static void pointer_axis(void *data, struct wl_pointer *ptr,
                          uint32_t time, uint32_t axis, wl_fixed_t value)
{
    /* Mausrad: Overlay schließen */
    (void)ptr; (void)time; (void)axis; (void)value;
    App *app = data;
    hide_overlay(app);
}

static void pointer_frame(void *data, struct wl_pointer *ptr)
{
    /* Frame-Event: keine Aktion nötig */
    (void)data; (void)ptr;
}

static void pointer_axis_source(void *data, struct wl_pointer *ptr,
                                 uint32_t axis_source)
{
    (void)data; (void)ptr; (void)axis_source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *ptr,
                               uint32_t time, uint32_t axis)
{
    (void)data; (void)ptr; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *ptr,
                                   uint32_t axis, int32_t discrete)
{
    (void)data; (void)ptr; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter         = pointer_enter,
    .leave         = pointer_leave,
    .motion        = pointer_motion,
    .button        = pointer_button,
    .axis          = pointer_axis,
    .frame         = pointer_frame,
    .axis_source   = pointer_axis_source,
    .axis_stop     = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

/* =========================================================================
 * Seat-Capabilities
 * =========================================================================
 * Wird aufgerufen, wenn der Compositor mitteilt, welche Eingabegeräte
 * (Tastatur, Maus, Touch) an diesem Seat verfügbar sind.
 */
static void seat_capabilities(void *data, struct wl_seat *seat,
                               uint32_t capabilities)
{
    App *app = data;

    /* Tastatur verfügbar und noch nicht angemeldet: Listener registrieren */
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    }

    /* Maus/Touchpad verfügbar und noch nicht angemeldet: Listener registrieren */
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !app->pointer) {
        app->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(app->pointer, &pointer_listener, app);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    /* Seat-Name wird nicht verwendet */
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

/* =========================================================================
 * Idle-Notification-Ereignisse
 * =========================================================================
 * Der Compositor feuert diese Events, wenn der Benutzer die eingestellte
 * Zeit inaktiv war (idled) bzw. wieder aktiv wurde (resumed).
 */
static void idle_notification_idled(void *data,
                                    struct ext_idle_notification_v1 *notif)
{
    /* Inaktivitäts-Schwelle erreicht: schwarzes Overlay anzeigen */
    (void)notif;
    App *app = data;
    show_overlay(app);
}

static void idle_notification_resumed(void *data,
                                      struct ext_idle_notification_v1 *notif)
{
    /*
     * Benutzer wieder aktiv: Overlay schließen, falls noch sichtbar.
     * Normalerweise wird hide_overlay() bereits durch Tastatur-/Mausereignisse
     * auf dem Overlay-Fenster ausgelöst. Dieser Callback dient als Absicherung.
     */
    (void)notif;
    App *app = data;
    hide_overlay(app);
}

static const struct ext_idle_notification_v1_listener idle_notification_listener = {
    .idled   = idle_notification_idled,
    .resumed = idle_notification_resumed,
};

/* =========================================================================
 * Wayland Registry
 * =========================================================================
 * Der Compositor kündigt hier alle verfügbaren globalen Objekte an.
 * Wir binden die Objekte, die wir benötigen.
 */
static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface,
                             uint32_t version)
{
    App *app = data;

    /* wl_compositor: zum Erstellen von Surfaces */
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface,
                                           (version < 4 ? version : 4));

    /* wl_shm: für Shared-Memory-Pixelpuffer */
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name,
                                    &wl_shm_interface, 1);

    /* wl_seat: für Tastatur- und Mauseingaben */
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        app->seat = wl_registry_bind(registry, name,
                                     &wl_seat_interface,
                                     (version < 5 ? version : 5));
        wl_seat_add_listener(app->seat, &seat_listener, app);

    /* zwlr_layer_shell_v1: für das Overlay-Fenster über allen anderen */
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        app->layer_shell = wl_registry_bind(registry, name,
                                            &zwlr_layer_shell_v1_interface,
                                            (version < 4 ? version : 4));

    /* ext_idle_notifier_v1: für die Inaktivitätserkennung */
    } else if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        app->idle_notifier = wl_registry_bind(registry, name,
                                              &ext_idle_notifier_v1_interface,
                                              1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    /* Objekte, die zur Laufzeit entfernt werden, ignorieren wir */
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* =========================================================================
 * Idle-Notification einrichten
 * =========================================================================
 * Erstellt die Benachrichtigung für den angegebenen Timeout und registriert
 * den Listener. Der Compositor beginnt sofort mit der Zeitmessung.
 */
static bool setup_idle_notification(App *app)
{
    if (!app->idle_notifier) {
        fprintf(stderr, "Compositor unterstützt ext-idle-notify-v1 nicht\n");
        return false;
    }
    if (!app->seat) {
        fprintf(stderr, "Kein Seat gefunden\n");
        return false;
    }

    /* Notification-Objekt für den gewünschten Timeout erstellen */
    app->idle_notification = ext_idle_notifier_v1_get_idle_notification(
        app->idle_notifier,
        (uint32_t)app->timeout_ms,
        app->seat
    );

    if (!app->idle_notification) {
        fprintf(stderr, "get_idle_notification fehlgeschlagen\n");
        return false;
    }

    /* Listener für idled- und resumed-Events registrieren */
    ext_idle_notification_v1_add_listener(app->idle_notification,
                                          &idle_notification_listener, app);
    return true;
}

/* =========================================================================
 * Kommandozeile auswerten
 * =========================================================================
 * Parst -s <sekunden> und -e. Schreibt Ergebnisse direkt in App-Struktur.
 * Gibt true zurück bei Erfolg, false bei Fehler.
 */
static bool parse_args(App *app, int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            /* Auf den Sekundenwert prüfen */
            if (i + 1 >= argc) {
                fprintf(stderr, "Fehler: -s benötigt einen Wert\n");
                return false;
            }
            i++;
            char *end;
            long secs = strtol(argv[i], &end, 10);
            if (*end != '\0' || secs <= 0) {
                fprintf(stderr, "Fehler: Ungültiger Wert für -s: %s\n", argv[i]);
                return false;
            }
            /* Sekunden in Millisekunden umrechnen */
            app->timeout_ms = (int)(secs * 1000);

        } else if (strcmp(argv[i], "-e") == 0) {
            app->exit_on_hide = true;

        } else {
            fprintf(stderr, "Unbekannter Parameter: %s\n", argv[i]);
            fprintf(stderr, "Verwendung: blkout [-s <sekunden>] [-e]\n");
            return false;
        }
    }
    return true;
}

/* =========================================================================
 * Hauptprogramm
 * ========================================================================= */
int main(int argc, char *argv[])
{
    /* --- Anwendungszustand initialisieren --- */
    App app = {
        .timeout_ms    = 0,
        .exit_on_hide  = false,
        .overlay_visible = false,
        .configured    = false,
        .running       = true,
        .shm_fd        = -1,
    };

    /* --- Kommandozeilenparameter auswerten --- */
    if (!parse_args(&app, argc, argv))
        return EXIT_FAILURE;

    /* --- Verbindung zum Wayland-Compositor herstellen --- */
    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "Keine Verbindung zum Wayland-Display möglich\n");
        return EXIT_FAILURE;
    }

    /* --- Registry anfordern, um globale Objekte zu binden --- */
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);

    /*
     * Zwei Roundtrips durchführen:
     * - Erster Roundtrip: Registry-Events empfangen (Objekte ankündigen)
     * - Zweiter Roundtrip: Seat-Capabilities empfangen (Keyboard/Pointer binden)
     */
    wl_display_roundtrip(app.display);
    wl_display_roundtrip(app.display);

    /* --- Pflichtkomponenten prüfen --- */
    if (!app.compositor) {
        fprintf(stderr, "wl_compositor nicht verfügbar\n");
        goto cleanup;
    }
    if (!app.shm) {
        fprintf(stderr, "wl_shm nicht verfügbar\n");
        goto cleanup;
    }
    if (!app.layer_shell) {
        fprintf(stderr, "zwlr_layer_shell_v1 nicht verfügbar\n"
                        "Ist der Compositor kompatibel (KDE Plasma 6+)?\n");
        goto cleanup;
    }

    /* --- Idle-Notification einrichten (nur bei -s) --- */
    if (app.timeout_ms > 0) {
        if (!setup_idle_notification(&app))
            goto cleanup;
    } else {
        /* Kein Timeout: Overlay sofort anzeigen */
        show_overlay(&app);
    }

    /*
     * --- Hauptschleife ---
     * wl_display_dispatch() blockiert, bis mindestens ein Ereignis
     * verarbeitet wurde, und ruft alle registrierten Listener-Callbacks auf.
     * Die Schleife läuft, bis app.running auf false gesetzt wird.
     */
    while (app.running && wl_display_dispatch(app.display) != -1)
        ;

    /* --- Aufräumen --- */
cleanup:
    /* Overlay abbauen, falls noch sichtbar */
    if (app.overlay_visible) {
        app.exit_on_hide = true;   /* Verhindert rekursives show_overlay() */
        hide_overlay(&app);
    }

    /* Idle-Notification freigeben */
    if (app.idle_notification)
        ext_idle_notification_v1_destroy(app.idle_notification);
    if (app.idle_notifier)
        ext_idle_notifier_v1_destroy(app.idle_notifier);

    /* Eingabeobjekte freigeben */
    if (app.keyboard)
        wl_keyboard_destroy(app.keyboard);
    if (app.pointer)
        wl_pointer_destroy(app.pointer);
    if (app.seat)
        wl_seat_destroy(app.seat);

    /* Layer-Shell freigeben */
    if (app.layer_shell)
        zwlr_layer_shell_v1_destroy(app.layer_shell);

    /* Wayland-Kernobjekte freigeben */
    if (app.shm)
        wl_shm_destroy(app.shm);
    if (app.compositor)
        wl_compositor_destroy(app.compositor);
    if (app.registry)
        wl_registry_destroy(app.registry);

    /* Verbindung zum Compositor trennen */
    if (app.display)
        wl_display_disconnect(app.display);

    return EXIT_SUCCESS;
}
