/*
 * wlr-screensaver-bridge
 *
 * Modes:
 *
 *   (default) Owns org.freedesktop.ScreenSaver on the session bus and
 *   translates Inhibit/UnInhibit cookies into zwp_idle_inhibit_manager_v1
 *   inhibitors.
 *
 *   --hold    No D-Bus: create a single Wayland idle inhibitor and hold it
 *   until killed. Manual "caffeine" for any wlroots compositor + swayidle.
 *
 * Why: Firefox (and other apps) suppress the screensaver via the XDG
 * portal's Inhibit interface, which xdg-desktop-portal-gtk forwards to the
 * legacy org.freedesktop.ScreenSaver D-Bus name. Wayland-native idle daemons
 * (swayidle, wayidle, ...) only honor the compositor's idle-inhibit protocol,
 * so without a provider for that D-Bus name, video playback never suppresses
 * idle.
 *
 * Works with any wlroots-based compositor (sway, dwl, mango, labwc, river,
 * ...) that implements zwp_idle_inhibit_manager_v1.
 *
 * The "proper" home for this is in-compositor (Hyprland answers the D-Bus
 * name itself); this daemon exists so every other compositor doesn't have to
 * grow D-Bus plumbing. If your compositor ever learns to answer the name
 * itself, delete this.
 *
 * The compositor counts idle inhibitors even from surfaces that are not part
 * of the scene graph (a plain unmapped wl_surface), so a headless inhibitor
 * is enough — no window is ever shown.
 *
 * Robustness: sd-bus transparent reconnects never re-request a lost bus
 * name, so auto-reconnect is disabled and a disconnect exits the daemon;
 * the systemd unit's Restart=always then re-acquires name + vtable cleanly.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <wayland-client.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct zwp_idle_inhibit_manager_v1 *inhibit_manager;
static struct wl_surface *surface;
static struct zwp_idle_inhibitor_v1 *inhibitor;

static sd_bus *bus;
static sd_event *event_loop;

/* active inhibit cookies, tagged with the D-Bus owner for cleanup */
static struct {
  uint32_t id;
  char *owner;
} *cookies;
static size_t n_cookies, alloc_cookies;
static uint32_t next_cookie = 1;
static bool inhibited;

static void add_cookie(uint32_t id, const char *owner) {
  if (n_cookies == alloc_cookies) {
    alloc_cookies = alloc_cookies ? alloc_cookies * 2 : 8;
    cookies = realloc(cookies, alloc_cookies * sizeof(*cookies));
    if (!cookies) {
      fprintf(stderr, "out of memory\n");
      exit(1);
    }
  }
  cookies[n_cookies].id = id;
  cookies[n_cookies].owner = strdup(owner);
  n_cookies++;
}

/* returns number of cookies removed */
static size_t remove_cookie(uint32_t id) {
  for (size_t i = 0; i < n_cookies; i++) {
    if (cookies[i].id == id) {
      free(cookies[i].owner);
      cookies[i] = cookies[n_cookies - 1];
      n_cookies--;
      return 1;
    }
  }
  return 0;
}

static size_t remove_cookies_by_owner(const char *owner) {
  size_t removed = 0;
  for (size_t i = 0; i < n_cookies;) {
    if (!strcmp(cookies[i].owner, owner)) {
      free(cookies[i].owner);
      cookies[i] = cookies[n_cookies - 1];
      n_cookies--;
      removed++;
    } else {
      i++;
    }
  }
  return removed;
}

static void apply_inhibit_state(void) {
  bool want = n_cookies > 0;
  if (want == inhibited)
    return;

  if (want && !inhibitor) {
    if (!inhibit_manager || !compositor) {
      fprintf(stderr, "wayland idle-inhibit manager not available yet\n");
      return;
    }
    if (!surface)
      surface = wl_compositor_create_surface(compositor);
    inhibitor =
        zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_manager, surface);
    wl_display_flush(display);
  } else if (!want && inhibitor) {
    zwp_idle_inhibitor_v1_destroy(inhibitor);
    inhibitor = NULL;
    wl_display_flush(display);
  }
  inhibited = want;
  fprintf(stderr, "inhibited=%d cookies=%zu\n", inhibited, n_cookies);
}

static int method_inhibit(sd_bus_message *m, void *userdata,
                          sd_bus_error *ret_error) {
  const char *app, *reason, *sender;
  int r = sd_bus_message_read(m, "ss", &app, &reason);
  if (r < 0)
    return r;
  sender = sd_bus_message_get_sender(m);

  uint32_t cookie = next_cookie++;
  add_cookie(cookie, sender ? sender : "");
  fprintf(stderr, "inhibit: %s (%s) cookie=%u owner=%s\n", app, reason, cookie,
          sender ? sender : "?");
  apply_inhibit_state();

  return sd_bus_reply_method_return(m, "u", cookie);
}

static int method_uninhibit(sd_bus_message *m, void *userdata,
                            sd_bus_error *ret_error) {
  uint32_t cookie;
  int r = sd_bus_message_read(m, "u", &cookie);
  if (r < 0)
    return r;

  if (remove_cookie(cookie))
    fprintf(stderr, "uninhibit: cookie=%u\n", cookie);
  else
    fprintf(stderr, "uninhibit: unknown cookie=%u\n", cookie);
  apply_inhibit_state();

  return sd_bus_reply_method_return(m, NULL);
}

static int method_get_active(sd_bus_message *m, void *userdata,
                             sd_bus_error *ret_error) {
  /* the screensaver itself never activates here; idle daemons do the blanking
   */
  return sd_bus_reply_method_return(m, "b", false);
}

static int method_zero(sd_bus_message *m, void *userdata,
                       sd_bus_error *ret_error) {
  return sd_bus_reply_method_return(m, "u", 0);
}

static int method_noop(sd_bus_message *m, void *userdata,
                       sd_bus_error *ret_error) {
  return sd_bus_reply_method_return(m, NULL);
}

static int name_owner_changed(sd_bus_message *m, void *userdata,
                              sd_bus_error *ret_error) {
  const char *name, *old_owner, *new_owner;
  if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
    return 0;
  if (new_owner[0] != '\0')
    return 0; /* only care about disconnects */

  size_t removed = remove_cookies_by_owner(old_owner);
  if (removed) {
    fprintf(stderr, "owner %s disconnected, dropped %zu cookie(s)\n", old_owner,
            removed);
    apply_inhibit_state();
  }
  return 0;
}

static const sd_bus_vtable screensaver_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Inhibit", "ss", "u", method_inhibit,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("UnInhibit", "u", "", method_uninhibit,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetActive", "", "b", method_get_active,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetActiveTime", "", "u", method_zero,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetSessionIdleTime", "", "u", method_zero,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Lock", "", "", method_noop, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SimulateUserActivity", "", "", method_noop,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetActive", "b", "", method_noop,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    /* Throttle/UnThrottle keep the screen awake too — treat like Inhibit */
    SD_BUS_METHOD("Throttle", "ss", "u", method_inhibit,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("UnThrottle", "u", "", method_uninhibit,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("ActiveChanged", "b", 0),
    SD_BUS_VTABLE_END,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t id, const char *interface,
                            uint32_t version) {
  if (!strcmp(interface, "wl_compositor"))
    compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
  else if (!strcmp(interface, "zwp_idle_inhibit_manager_v1"))
    inhibit_manager = wl_registry_bind(
        registry, id, &zwp_idle_inhibit_manager_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t id) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int wayland_event(sd_event_source *source, int fd, uint32_t revents,
                         void *userdata) {
  if (revents & (EPOLLERR | EPOLLHUP)) {
    fprintf(stderr, "wayland connection hung up\n");
    sd_event_exit(event_loop, 1);
    return 0;
  }
  if (wl_display_dispatch(display) < 0) {
    fprintf(stderr, "wayland dispatch failed\n");
    sd_event_exit(event_loop, 1);
    return 0;
  }
  wl_display_flush(display);
  return 0;
}

static int wayland_connect_and_require_inhibit(void) {
  display = wl_display_connect(NULL);
  if (!display) {
    fprintf(stderr, "cannot connect to wayland display\n");
    return -1;
  }
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  if (wl_display_roundtrip(display) < 0) {
    fprintf(stderr, "wayland roundtrip failed\n");
    return -1;
  }
  if (!inhibit_manager) {
    fprintf(stderr,
            "compositor does not provide zwp_idle_inhibit_manager_v1\n");
    return -1;
  }
  return 0;
}

/* --hold mode: one inhibitor, held until killed */
static int run_hold(void) {
  if (wayland_connect_and_require_inhibit() < 0)
    return 1;
  surface = wl_compositor_create_surface(compositor);
  inhibitor =
      zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_manager, surface);
  wl_display_flush(display);
  fprintf(stderr, "holding wayland idle inhibitor (pid %d); kill to release\n",
          getpid());
  for (;;)
    pause();
}

int main(int argc, char **argv) {
  setbuf(stderr, NULL);

  if (argc > 1 && !strcmp(argv[1], "--hold"))
    return run_hold();

  if (wayland_connect_and_require_inhibit() < 0)
    return 1;

  int r = sd_event_default(&event_loop);
  if (r < 0)
    goto fail;
  r = sd_bus_default_user(&bus);
  if (r < 0)
    goto fail;
  /* sd-bus reconnects would silently drop our bus name; exiting the event
   * loop on disconnect lets the service manager restart us with a clean
   * name + vtable. */
  sd_bus_set_exit_on_disconnect(bus, 1);

  r = sd_bus_request_name(bus, "org.freedesktop.ScreenSaver", 0);
  if (r < 0)
    goto fail;
  /* positive result: 1 = primary owner, 2 = queued, 3 = exists, 4 = already
   * owner */
  if (r != 1 && r != 4) {
    fprintf(stderr, "org.freedesktop.ScreenSaver already owned (result=%d)\n",
            r);
    return 1;
  }

  r = sd_bus_add_object_vtable(bus, NULL, "/org/freedesktop/ScreenSaver",
                               "org.freedesktop.ScreenSaver",
                               screensaver_vtable, NULL);
  if (r < 0)
    goto fail;
  r = sd_bus_add_match(
      bus, NULL,
      "type='signal',sender='org.freedesktop.DBus',"
      "interface='org.freedesktop.DBus',member='NameOwnerChanged'",
      name_owner_changed, NULL);
  if (r < 0)
    goto fail;
  r = sd_bus_attach_event(bus, event_loop, SD_EVENT_PRIORITY_NORMAL);
  if (r < 0)
    goto fail;
  r = sd_event_add_io(event_loop, NULL, wl_display_get_fd(display), EPOLLIN,
                      wayland_event, NULL);
  if (r < 0)
    goto fail;

  fprintf(stderr,
          "bridging org.freedesktop.ScreenSaver -> wayland idle-inhibit\n");
  r = sd_event_loop(event_loop);
  return r < 0 ? 1 : 0;

fail:
  fprintf(stderr, "error: %s\n", strerror(-r));
  return 1;
}
