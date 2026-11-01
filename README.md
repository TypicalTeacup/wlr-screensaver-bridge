# wlr-screensaver-bridge

Owns `org.freedesktop.ScreenSaver` on the session bus and translates
`Inhibit`/`UnInhibit` cookies into `zwp_idle_inhibit_manager_v1` Wayland idle
inhibitors.

```
Firefox / Electron
    → org.freedesktop.portal.Inhibit
    → xdg-desktop-portal-gtk
    → org.freedesktop.ScreenSaver        ← this daemon owns the name
    → zwp_idle_inhibit_manager_v1        ← …and mirrors it here
    → compositor (sway, dwl, mango, labwc, river, …) → swayidle/wayidle never fire
```

**Why this exists:** apps suppress the screensaver through the XDG portal,
which the GTK portal backend forwards to the legacy
`org.freedesktop.ScreenSaver` D-Bus name. Wayland-native idle daemons
(swayidle, wayidle) only honor the compositor's idle-inhibit protocol, so on
compositors that don't implement the D-Bus name themselves (everything
wlroots-based), video playback never suppresses idle.

Works with any compositor providing `zwp_idle_inhibit_manager_v1`. The
inhibitor is created on a headless unmapped `wl_surface` — no window ever
appears, and the compositor's idle logic counts it all the same.

The "proper" home for this is in-compositor (Hyprland answers the D-Bus name
itself). This daemon exists so every other compositor doesn't have to grow
D-Bus plumbing.

## Install

### Nix (flake)

```nix
# flake.nix
inputs.wlr-screensaver-bridge.url = "github:typicalteacup/wlr-screensaver-bridge";
```

```nix
# configuration
environment.systemPackages = [inputs.wlr-screensaver-bridge.packages.${system}.default];
systemd.user.services.wlr-screensaver-bridge = {
  unitConfig.ConditionEnvironment = ["WAYLAND_DISPLAY"];
  after = ["graphical-session.target"];
  wantedBy = ["graphical-session.target"];
  partOf = ["graphical-session.target"];
  serviceConfig = {
    ExecStart = lib.getExe inputs.wlr-screensaver-bridge.packages.${system}.default;
    Restart = "always";
    RestartSec = 2;
  };
};
```

### make

Requires `pkg-config`, `wayland-scanner`, `wayland-protocols`, `libsystemd`
and `wayland-client` headers.

```sh
make
sudo make install   # installs the binary + systemd user unit
systemctl --user enable --now wlr-screensaver-bridge
```

## Notes

- If the service logs `org.freedesktop.ScreenSaver already owned`, another
  screensaver implementation (GNOME/KDE session, xscreensaver, caffeine, …)
  already provides the name — you don't need this bridge there.
- Cookies are dropped automatically if the D-Bus owner disconnects without
  calling `UnInhibit` (common app-crash leak).
- `Throttle`/`UnThrottle` are treated as `Inhibit`/`UnInhibit`.
- On a session-bus disconnect the daemon exits and relies on the service
  manager's restart to re-acquire the name (sd-bus reconnects would silently
  drop it).

## License

MIT
