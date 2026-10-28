#!/usr/bin/env bash
# Builds the GoldenEye 007 portable Linux release in out/release (twin of Make-Release.ps1).
# The tar.gz holds one folder:
#
#   GoldenEye 007/
#     GoldenEye 007               the launcher: the only program to run
#     README.txt
#     licenses/
#     resources/
#       runtime/                  the game (GoldenEye 007, librexruntime.so,
#                                 librexgpu-xenos.so) and the port's own
#                                 pictures and version.txt
#
# The release carries no original game content; the audit below refuses to
# package anything that looks like game data, debug output or user state.
#
# Environment: BUILD_DIR (default out/build/linux-amd64-release), VERSION
# (default: contents of the VERSION file).
set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"

BUILD_DIR="${BUILD_DIR:-out/build/linux-amd64-release}"
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$project_root/$BUILD_DIR" ;; esac
VERSION="${VERSION:-$(tr -d '\r\n' < "$project_root/VERSION" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')}"
[[ "$VERSION" =~ ^[A-Za-z0-9._-]+$ ]] || die "Version may contain only letters, numbers, dots, underscores and hyphens."

packaging="$project_root/packaging"
release_root="$project_root/out/release"
release_name="GoldenEye 007"
stage_root="$release_root/staging/$release_name"
archive="$release_root/GoldenEye-007-XBLA-PC-Recomp-v$VERSION-linux-amd64.tar.gz"
exe_name="GoldenEye 007"
launcher="$BUILD_DIR/launcher/$exe_name"

# --- 1. Inputs ---------------------------------------------------------------
game_bin="$BUILD_DIR/$exe_name"
gpu_so="$BUILD_DIR/librexgpu-xenos.so"
rt_so="$BUILD_DIR/librexruntime.so"
[[ -f "$game_bin" ]] || die "Build output is missing: $exe_name. Build linux-amd64-release first."
[[ -f "$gpu_so" ]] || die "Build output is missing: librexgpu-xenos.so. Build linux-amd64-release first."
if [[ ! -f "$rt_so" ]]; then
    rt_so="$project_root/rexglue-sdk/out/linux-amd64/librexruntime.so"
    [[ -f "$rt_so" ]] || die "librexruntime.so not found in $BUILD_DIR or rexglue-sdk/out/linux-amd64."
    echo "notice: using librexruntime.so from rexglue-sdk/out/linux-amd64"
fi
[[ -f "$launcher" ]] || die "The launcher was not built: $launcher (build target ge_launcher)."

# A release must never ship a binary older than the source it claims to be.
newest_source="$(find "$project_root/src" -type f -printf '%T@ %p\n' | sort -rn | sed -n '1p' | cut -d' ' -f2-)"
if [[ -n "$newest_source" && "$newest_source" -nt "$game_bin" ]]; then
    die "$exe_name is older than src/${newest_source#"$project_root/src/"}. Rebuild before packaging."
fi

# --- 2. Stage -----------------------------------------------------------------
for played in resources/assets resources/userdata; do
    [[ -e "$stage_root/$played" ]] && die "The staging folder has been played from ($played; saves?). Move it away first: $stage_root"
done
rm -rf "$stage_root"
rm -f "$archive"

licenses_stage="$stage_root/licenses"
runtime_stage="$stage_root/resources/runtime"
mkdir -p "$licenses_stage" "$runtime_stage"

install -m 755 "$launcher" "$stage_root/$exe_name"
sed -e 's/\r$//' -e "s/@VERSION@/$VERSION/g" "$packaging/README.txt" > "$stage_root/README.txt"
chmod 644 "$stage_root/README.txt"
for f in "$packaging"/licenses/*; do
    [[ -f "$f" ]] && install -m 644 "$f" "$licenses_stage/"
done

install -m 755 "$game_bin" "$runtime_stage/$exe_name"
install -m 755 "$rt_so" "$runtime_stage/librexruntime.so"
install -m 755 "$gpu_so" "$runtime_stage/librexgpu-xenos.so"
for shape in 169 219; do
    install -m 644 "$packaging/menu-backdrop-$shape.png" "$runtime_stage/"
done
for folder in textures portraits; do
    # Optional: the game falls back to the original pictures without them.
    [[ -d "$packaging/$folder" ]] || continue
    mkdir -p "$runtime_stage/$folder"
    for f in "$packaging/$folder"/*.png; do
        [[ -f "$f" ]] && install -m 644 "$f" "$runtime_stage/$folder/"
    done
done
install -m 644 "$packaging/art/game-crosshair.png" "$runtime_stage/icon.png"
printf '%s' "$VERSION" > "$runtime_stage/version.txt"

# --- 3. GPU plugin ABI ----------------------------------------------------------
# ReXGlue 0.10.0 (GoldenEye SDK) uses GPU plugin ABI 1. Validate the exported
# ABI function on the staged plugin so a stale plugin cannot produce a broken release.
gpu_abi="skipped (python3 not found)"
if command -v python3 >/dev/null 2>&1; then
    gpu_abi="$(LD_LIBRARY_PATH="$runtime_stage${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" python3 -c '
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
fn = lib.rex_gpu_abi_version
fn.restype = ctypes.c_uint32
print(fn())
' "$runtime_stage/librexgpu-xenos.so")" || die "Could not load the staged GPU plugin for ABI validation."
    [[ "$gpu_abi" == "1" ]] || die "GPU plugin ABI mismatch: found $gpu_abi, expected 1. Rebuild with GPU_PLUGINS xenos."
else
    echo "warning: python3 not found; GPU plugin ABI check skipped" >&2
fi

# --- 4. Audit: nothing from the game package, no debug output, no user state -
for item in "$stage_root"/* "$stage_root"/.[!.]*; do
    [[ -e "$item" ]] || continue
    case "$(basename "$item")" in
        "$exe_name"|README.txt|licenses|resources) ;;
        *) die "Unexpected item in the release root: $(basename "$item")" ;;
    esac
done
for item in "$stage_root/resources"/* "$stage_root/resources"/.[!.]*; do
    [[ -e "$item" ]] || continue
    [[ "$(basename "$item")" == runtime ]] || die "Unexpected item in resources: $(basename "$item")"
done
forbidden_names=(assets userdata logs cache Game ge.toml default.xex ArcadeInfo.xml Bean.xex music.xwb sfx.xwb files guest_image.bin xstr_decompressed.bin)
forbidden_patterns=('*.pdb' '*.xex' '*.bin' '*.log' '*.ilk' '*.exp' '*.lib' '*.ps1' '*.xwb' '*.xsb' '*.xgs' '*.rba' '*.rbh' '*.str')
while IFS= read -r -d '' item; do
    base="$(basename "$item")"
    for name in "${forbidden_names[@]}"; do
        [[ "$base" == "$name" ]] && die "Forbidden item entered the release: $item"
    done
    for pattern in "${forbidden_patterns[@]}"; do
        # shellcheck disable=SC2053
        [[ "$base" == $pattern ]] && die "Forbidden file type entered the release: $item"
    done
    if [[ -f "$item" ]] && (( $(stat -c %s "$item") > 64 * 1024 * 1024 )); then
        die "Unexpectedly large file in the release: $item"
    fi
done < <(find "$stage_root" -mindepth 1 -print0)

# Byte-level check against the developer's extracted game data, when present:
# no shipped file may be identical to any file from the package.
assets_root="$BUILD_DIR/assets"
[[ -e "$assets_root" ]] || assets_root="$project_root/game"
asset_comparisons=0
if [[ -e "$assets_root" ]]; then
    declare -A staged_sizes=() staged_hashes=()
    while IFS= read -r -d '' f; do
        staged_sizes[$(stat -c %s "$f")]=1
        h="$(sha256sum "$f" | cut -d' ' -f1)"
        staged_hashes[$h]="$f"
    done < <(find "$stage_root" -type f -print0)
    while IFS= read -r -d '' a; do
        [[ -n "${staged_sizes[$(stat -L -c %s "$a")]:-}" ]] || continue
        asset_comparisons=$((asset_comparisons + 1))
        h="$(sha256sum "$a" | cut -d' ' -f1)"
        [[ -z "${staged_hashes[$h]:-}" ]] || die "Release file ${staged_hashes[$h]} is identical to game data $a"
    done < <(find -L "$assets_root" -type f -print0)
fi

# --- 5. The release tarball -----------------------------------------------------
mkdir -p "$release_root"
tar --owner=0 --group=0 --numeric-owner --sort=name -C "$release_root/staging" -czf "$archive" "$release_name"

echo "Portable release created."
echo "Folder:   $stage_root"
echo "Archive:  $archive"
echo "Root:     $(ls -1A "$stage_root" | paste -sd, - | sed 's/,/, /g')"
echo "Runtime:  $(find "$runtime_stage" -type f | wc -l) files in resources/runtime"
echo "GPU ABI:  $gpu_abi"
echo "Audit:    no forbidden items; $asset_comparisons same-size game files compared, none identical"
echo "Size:     $(du -sh "$stage_root" | cut -f1) uncompressed; $(du -h "$archive" | cut -f1) tar.gz"
echo "SHA256:   $(sha256sum "$archive" | cut -d' ' -f1)"
echo
echo "Tree:"
(cd "$release_root/staging" && find "$release_name" | sort | sed 's|[^/]*/|  |g')
