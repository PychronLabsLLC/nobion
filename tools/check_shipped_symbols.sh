#!/usr/bin/env bash
# The symbols archive must match the programs a package ships.
#
#   tools/check_shipped_symbols.sh <build dir> <symbols dir> macos|linux
#
# <build dir> holds CPack's staging tree (_CPack_Packages), made by `cpack`;
# <symbols dir> holds the archive tools/split_symbols.py wrote.
#   macOS  the UUIDs (dwarfdump --uuid) of each staged program equal those of
#          its dSYM: the strip, signing and macdeployqt kept the binary the
#          dSYM was made from.
#   Linux  each staged program has a .gnu_debuglink section, which strip keeps.
# Exit status 1 on a mismatch, 2 when there is nothing to check.
set -euo pipefail
build=$1 symbols=$2 os=$3
programs=(elctl nobion Nobion)
checked=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if [ "$os" = macos ]; then
  unzip -q "$symbols"/*.dSYM.zip -d "$tmp"
fi

for name in "${programs[@]}"; do
  while IFS= read -r staged; do
    [ -n "$staged" ] || continue
    case "$os" in
      macos)
        dsym="$tmp/$name.dSYM"
        # Nobion.app/Contents/MacOS/Nobion is the target "nobion" (OUTPUT_NAME Nobion).
        [ -d "$dsym" ] || dsym="$tmp/nobion.dSYM"
        [ -d "$dsym" ] || continue
        want=$(dwarfdump --uuid "$dsym" | awk '{print $2}' | sort | tr '\n' ' ')
        have=$(dwarfdump --uuid "$staged" | awk '{print $2}' | sort | tr '\n' ' ')
        echo "$staged: $have(dSYM: $want)"
        [ -n "$have" ] && [ "$have" = "$want" ] || { echo "UUID mismatch: $staged"; exit 1; }
        ;;
      linux)
        echo "$staged"
        readelf -S "$staged" | grep -q '\.gnu_debuglink' || { echo "no .gnu_debuglink: $staged"; exit 1; }
        ;;
    esac
    checked=$((checked + 1))
  done < <(find "$build/_CPack_Packages" -type f -name "$name" -perm -u+x 2>/dev/null | grep -E '/(MacOS|bin)/' || true)
done

if [ "$checked" -eq 0 ]; then echo "no staged program found under $build/_CPack_Packages"; exit 2; fi
echo "$checked program(s) match their symbols"
