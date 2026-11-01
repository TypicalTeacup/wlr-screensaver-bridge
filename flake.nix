{
  description = "Bridge org.freedesktop.ScreenSaver D-Bus inhibits to Wayland idle-inhibit";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";

  outputs = {nixpkgs, ...}: let
    systems = ["x86_64-linux" "aarch64-linux"];
    forAllSystems = nixpkgs.lib.genAttrs systems;
  in {
    packages = forAllSystems (system: let
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      default = pkgs.stdenv.mkDerivation {
        pname = "wlr-screensaver-bridge";
        version = "0.1.0";
        src = ./.;

        nativeBuildInputs = with pkgs; [pkg-config wayland-scanner];
        buildInputs = with pkgs; [systemd wayland wayland-protocols];

        buildPhase = ''
          runHook preBuild
          wayland-scanner client-header \
            ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml \
            idle-inhibit-unstable-v1-client-protocol.h
          wayland-scanner private-code \
            ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml \
            idle-inhibit-protocol.c
          $CC -O2 -Wall -Wextra -o wlr-screensaver-bridge main.c idle-inhibit-protocol.c \
            $(${pkgs.pkg-config}/bin/pkg-config --cflags --libs libsystemd wayland-client)
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          install -Dm755 wlr-screensaver-bridge -t $out/bin
        '';

        meta = with pkgs.lib; {
          description = "Bridge org.freedesktop.ScreenSaver D-Bus inhibits to Wayland idle-inhibit";
          longDescription = ''
            Owns org.freedesktop.ScreenSaver on the session bus and translates
            Inhibit/UnInhibit cookies into zwp_idle_inhibit_manager_v1
            inhibitors, so apps that suppress the screensaver via the XDG
            portal (Firefox, Electron, ...) keep the screen awake on
            compositors whose idle daemons (swayidle, wayidle) only honor the
            Wayland idle-inhibit protocol.
          '';
          license = licenses.mit;
          platforms = platforms.linux;
          mainProgram = "wlr-screensaver-bridge";
        };
      };
    });
  };
}
