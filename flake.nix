{
  description = "86Box - Emulator of x86-based systems";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      forAllSystems = nixpkgs.lib.genAttrs [
        "x86_64-linux"
        "aarch64-linux"
      ];

      pkgsFor = system: nixpkgs.legacyPackages.${system};

      commonDeps = pkgs: with pkgs; [
        freetype
        libpng
        SDL2
        openal
        rtmidi
        fluidsynth
        libsndfile
        libslirp
        glib
      ];

      commonFlags = [
        "-DOPENAL=ON"
        "-DRTMIDI=ON"
        "-DFLUIDSYNTH=ON"
        "-DMUNT=ON"
      ];

      mkEightySixBox = pkgs: pkgs.stdenv.mkDerivation {
        pname = "86box-sdl";
        version = "6.0-dev";

        src = self;

        nativeBuildInputs = with pkgs; [ cmake ninja pkg-config ];
        buildInputs = commonDeps pkgs;

        cmakeFlags = commonFlags ++ [ "-DQT=OFF" ];

        meta = {
          description = "Emulator of x86-based systems (SDL UI)";
          homepage = "https://86box.net";
          license = pkgs.lib.licenses.gpl2Plus;
          platforms = [ "x86_64-linux" "aarch64-linux" ];
        };
      };

      mkEightySixBoxQt = pkgs: pkgs.stdenv.mkDerivation {
        pname = "86box-qt";
        version = "6.0-dev";

        src = self;

        nativeBuildInputs = with pkgs; [
          cmake
          ninja
          pkg-config
          qt6.wrapQtAppsHook
        ];

        buildInputs = commonDeps pkgs ++ (with pkgs; [
          qt6.qtbase
          qt6.qttools
          qt6.qtwayland
          libx11
          libxi
          libxcb
          libxkbcommon
          libevdev
          wayland
          wayland-scanner
          extra-cmake-modules
        ]);

        cmakeFlags = commonFlags ++ [
          "-DQT=ON"
          "-DUSE_QT6=ON"
        ];

        meta = {
          description = "Emulator of x86-based systems (Qt6 GUI)";
          homepage = "https://86box.net";
          license = pkgs.lib.licenses.gpl2Plus;
          platforms = [ "x86_64-linux" "aarch64-linux" ];
        };
      };
    in
    {
      packages = forAllSystems (system:
        let pkgs = pkgsFor system; in
        {
          default = self.packages.${system}.eighty-six-box-qt;

          eighty-six-box-sdl = mkEightySixBox pkgs;
          eighty-six-box-qt = mkEightySixBoxQt pkgs;
        }
        // nixpkgs.lib.optionalAttrs (system == "x86_64-linux") {
          # Cross-compiled aarch64 binary built on x86_64
          eighty-six-box-sdl-aarch64 =
            mkEightySixBox pkgs.pkgsCross.aarch64-multiplatform;
        }
      );

      devShells = forAllSystems (system:
        let pkgs = pkgsFor system; in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.eighty-six-box-sdl ];

            packages = with pkgs; [
              gdb
              ccache
              clang-tools
            ];
          };
        }
      );
    };
}
