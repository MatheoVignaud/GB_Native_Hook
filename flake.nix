{
  description = "DevShell tmc";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";

  outputs = { self, nixpkgs, ... }:
  let
    system = "x86_64-linux";
    pkgs = import nixpkgs { inherit system; config.allowUnfree = true; };
  in {
    devShells.${system}.default = pkgs.mkShell {
      buildInputs = with pkgs; [
        # Outils de build
        xmake pkg-config cmake ninja python3
        # X/Wayland
        xorg.libX11 xorg.libXext xorg.libXrandr xorg.libXrender
        xorg.libXau xorg.libXdmcp xorg.xorgproto
        libxkbcommon wayland wayland-protocols
        wayland-utils
        # SDL / Vulkan
        sdl3 vulkan-headers vulkan-loader vulkan-validation-layers vulkan-memory-allocator
        # Shaders / math / tests
        glslang spirv-tools spirv-headers
        fmt
        xwayland
      ];

      shellHook = ''
        export PKG_CONFIG_PATH=/run/current-system/sw/lib/pkgconfig:$PKG_CONFIG_PATH
        export LD_LIBRARY_PATH=/run/current-system/sw/lib:$LD_LIBRARY_PATH
        export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
        export XMAKE_ROOT=y
        SDL_VIDEODRIVER=wayland
      '';
    };
  };
}
