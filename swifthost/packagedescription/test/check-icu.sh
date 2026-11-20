#!/bin/bash
# Reports whether linked manifest modules contain ICU, FoundationInternationalization or CoreFoundation code, by
# counting symbols in the name section. The ICU C API is built with the `swift_` prefix (swift_ucol_open, ...).
# Usage: TOOLS=<dir with llvm-nm> check-icu.sh <manifest.wasm>...
set -u
: "${TOOLS:?}"
printf 'file\ticu_c_symbols\tFoundationInternationalization_symbols\tCoreFoundation_c_symbols\tFoundationICU_symbols\tsymbols\n'
for f in "$@"; do
  s=$("$TOOLS/llvm-nm" "$f")
  icu=$(grep -c -E ' swift_u[a-z0-9]*_[A-Za-z]' <<< "$s")
  fi=$(grep -c 'FoundationInternationalization' <<< "$s")
  cf=$(grep -c -E ' _?CF[A-Z][A-Za-z]+$' <<< "$s")
  fic=$(grep -c '_FoundationICU' <<< "$s")
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$f" "$icu" "$fi" "$cf" "$fic" "$(wc -l <<< "$s" | tr -d ' ')"
done
