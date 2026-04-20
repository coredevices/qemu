{
  description = "Pebble QEMU – QEMU with Pebble smartwatch device models";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    # QEMU meson subprojects that can't be fetched inside the Nix sandbox
    keycodemapdb-src = {
      url = "gitlab:qemu-project/keycodemapdb/f5772a62ec52591ff6870b7e8ef32482371f22c6";
      flake = false;
    };
    berkeley-softfloat-3-src = {
      url = "gitlab:qemu-project/berkeley-softfloat-3/b64af41c3276f97f0e181920400ee056b9c88037";
      flake = false;
    };
    berkeley-testfloat-3-src = {
      url = "gitlab:qemu-project/berkeley-testfloat-3/e7af9751d9f9fd3b47911f51a5cfd08af256a9ab";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, keycodemapdb-src, berkeley-softfloat-3-src, berkeley-testfloat-3-src }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          inherit (pkgs) lib stdenv;
        in {
          default = stdenv.mkDerivation {
            pname = "qemu-pebble";
            version = "10.1.0-pebble";
            src = self;

            nativeBuildInputs = with pkgs; [
              pkg-config
              ninja
              flex
              bison
              git
              dtc
              (python3.withPackages (ps: with ps; [ meson tomli ]))
            ] ++ lib.optionals stdenv.hostPlatform.isLinux [
              pkgs.patchelf
            ];

            buildInputs = with pkgs; [
              glib
              pixman
              SDL2
              zlib
            ] ++ lib.optionals stdenv.hostPlatform.isDarwin [
              pkgs.apple-sdk_15
            ];

            postUnpack = ''
              chmod -R u+w $sourceRoot
              # Pre-populate meson subprojects so meson doesn't try to git-fetch
              cp -r ${keycodemapdb-src} $sourceRoot/subprojects/keycodemapdb
              cp -r ${berkeley-softfloat-3-src} $sourceRoot/subprojects/berkeley-softfloat-3
              chmod -R u+w $sourceRoot/subprojects/keycodemapdb $sourceRoot/subprojects/berkeley-softfloat-3
              cp -r ${berkeley-testfloat-3-src} $sourceRoot/subprojects/berkeley-testfloat-3
              chmod -R u+w $sourceRoot/subprojects/berkeley-testfloat-3
              # Apply packagefiles overlays (meson.build etc. that the wrap system normally adds)
              cp -r $sourceRoot/subprojects/packagefiles/berkeley-softfloat-3/* $sourceRoot/subprojects/berkeley-softfloat-3/
              cp -r $sourceRoot/subprojects/packagefiles/berkeley-testfloat-3/* $sourceRoot/subprojects/berkeley-testfloat-3/
              # Make Rez/SetFile optional (icon embedding, not available in Nix sandbox)
              sed -i 's|^Rez -append|command -v Rez \&>/dev/null \&\& Rez -append|' $sourceRoot/scripts/entitlement.sh
              sed -i 's|^SetFile -a|command -v SetFile \&>/dev/null \&\& SetFile -a|' $sourceRoot/scripts/entitlement.sh
            '';

            dontConfigure = true;

            buildPhase = ''
              runHook preBuild
              bash build-dist.sh
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/dist/bin $out/dist/lib
              cp dist/bin/qemu-pebble $out/dist/bin/
              chmod u+w $out/dist/bin/qemu-pebble
            '' + lib.optionalString stdenv.hostPlatform.isDarwin ''
              # Recursively collect all Nix store dylib deps
              changed=true
              while $changed; do
                changed=false
                for file in $out/dist/bin/qemu-pebble $out/dist/lib/*.dylib; do
                  [ -f "$file" ] || continue
                  for dep in $(otool -L "$file" | awk '/^\t.*\/nix\/store/ {print $1}'); do
                    libname=$(basename "$dep")
                    if [ ! -f "$out/dist/lib/$libname" ]; then
                      cp "$dep" "$out/dist/lib/$libname"
                      chmod u+w "$out/dist/lib/$libname"
                      changed=true
                    fi
                  done
                done
              done

              # Rewrite all references to use relative paths
              for file in $out/dist/bin/qemu-pebble $out/dist/lib/*.dylib; do
                [ -f "$file" ] || continue
                for dep in $(otool -L "$file" | awk '/^\t.*\/nix\/store/ {print $1}'); do
                  libname=$(basename "$dep")
                  if [ "$file" = "$out/dist/bin/qemu-pebble" ]; then
                    install_name_tool -change "$dep" "@executable_path/../lib/$libname" "$file"
                  else
                    install_name_tool -change "$dep" "@loader_path/$libname" "$file"
                  fi
                done
                if [ "$file" != "$out/dist/bin/qemu-pebble" ]; then
                  install_name_tool -id "@loader_path/$(basename "$file")" "$file"
                fi
              done

              /usr/bin/codesign --force --sign - $out/dist/bin/qemu-pebble
              for lib in $out/dist/lib/*.dylib; do
                /usr/bin/codesign --force --sign - "$lib"
              done
            '' + lib.optionalString stdenv.hostPlatform.isLinux ''
              # Recursively collect all Nix store shared lib deps (skip glibc)
              changed=true
              while $changed; do
                changed=false
                for file in $out/dist/bin/qemu-pebble $out/dist/lib/*.so*; do
                  [ -f "$file" ] || continue
                  for dep in $(ldd "$file" 2>/dev/null | awk '$3 ~ /\/nix\/store/ && $3 !~ /glibc/ {print $3}'); do
                    libname=$(basename "$dep")
                    if [ ! -f "$out/dist/lib/$libname" ]; then
                      cp "$dep" "$out/dist/lib/$libname"
                      chmod u+w "$out/dist/lib/$libname"
                      changed=true
                    fi
                  done
                done
              done

              patchelf --set-rpath '$ORIGIN/../lib' $out/dist/bin/qemu-pebble
              for lib in $out/dist/lib/*.so*; do
                patchelf --set-rpath '$ORIGIN' "$lib" 2>/dev/null || true
              done
              patchelf --set-interpreter ${
                if stdenv.hostPlatform.isx86_64
                then "/lib64/ld-linux-x86-64.so.2"
                else "/lib/ld-linux-aarch64.so.1"
              } $out/dist/bin/qemu-pebble
            '' + ''
              # Remove lib dir if empty (nothing was bundled)
              rmdir $out/dist/lib 2>/dev/null || true
              runHook postInstall
            '';

            meta = with lib; {
              description = "QEMU with Pebble smartwatch device models";
              license = licenses.gpl2Plus;
              platforms = supportedSystems;
            };
          };
        }
      );

      devShells = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = [ pkgs.git ];
          };
        }
      );
    };
}
