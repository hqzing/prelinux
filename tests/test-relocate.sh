#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The PreLinux Authors

# test-relocate.sh — relocate unit tests (design: ELF, shebang,
# env-shebang, non-executable samples).
#
# Usage: test-relocate.sh <repo-root>   (run on host; needs cc)
set -eu
repo=$1

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
inst="$work/inst"

# fake instance root: just needs glibc for root detection
mkdir -p "$inst/glibc/lib"

# --- 1. ELF executable: interp rewritten ------------------------------
cat > "$work/hello.c" <<'EOF'
#include <stdio.h>
int main (void) { puts("hi"); return 0; }
EOF
cc -O2 -o "$work/hello" "$work/hello.c"

"$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/hello"
interp=$(readelf -lW "$work/hello" | grep -oE 'interpreter: [^]]+')
case "$interp" in
  "interpreter: $inst/glibc/lib/ld-linux-"*) ;;
  *) echo "test-relocate: ELF interp FAIL: $interp" >&2; exit 1 ;;
esac

# --- 2. absolute shebang ----------------------------------------------
printf '#!/bin/sh\necho hi\n' > "$work/script.sh"
chmod +x "$work/script.sh"
"$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/script.sh"
head -1 "$work/script.sh" | grep -q "^#!$inst/bin/sh$" || {
  echo "test-relocate: shebang FAIL: $(head -1 "$work/script.sh")" >&2
  exit 1; }
[ "$(sed -n '2p' "$work/script.sh")" = "echo hi" ] || {
  echo "test-relocate: shebang content mangled" >&2; exit 1; }

# --- 3. env shebang: untouched ----------------------------------------
printf '#!/usr/bin/env python3\nprint(1)\n' > "$work/envscript"
chmod +x "$work/envscript"
"$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/envscript"
[ "$(head -1 "$work/envscript")" = "#!/usr/bin/env python3" ] || {
  echo "test-relocate: env shebang must stay untouched" >&2; exit 1; }

# --- 4. non-executable file: untouched --------------------------------
printf 'just text\n' > "$work/plain.txt"
sum0=$(sha256sum "$work/plain.txt" | cut -d' ' -f1)
"$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/plain.txt"
sum1=$(sha256sum "$work/plain.txt" | cut -d' ' -f1)
[ "$sum0" = "$sum1" ] || {
  echo "test-relocate: plain file modified" >&2; exit 1; }

# --- 5. idempotency: second pass changes nothing ----------------------
sum0=$(sha256sum "$work/hello" | cut -d' ' -f1)
"$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/hello"
sum1=$(sha256sum "$work/hello" | cut -d' ' -f1)
[ "$sum0" = "$sum1" ] || {
  echo "test-relocate: not idempotent" >&2; exit 1; }

# --- 6. absolute symlink on a mapped prefix: anchored into the instance
ln -s /etc/pki/ca-trust/bundle.pem "$work/abs.lnk"
"$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/abs.lnk"
[ "$(readlink "$work/abs.lnk")" = "$inst/etc/pki/ca-trust/bundle.pem" ] || {
  echo "test-relocate: absolute symlink FAIL: $(readlink "$work/abs.lnk")" >&2
  exit 1; }

# --- 7. pass-through + relative + in-instance symlinks: untouched -----
ln -s /proc/self/status "$work/proc.lnk"
ln -s ../relative/target "$work/rel.lnk"
ln -s "$inst/already/anchored" "$work/anchored.lnk"
"$repo/prelinux/prelinux" relocate --root "$inst" --quiet \
  "$work/proc.lnk" "$work/rel.lnk" "$work/anchored.lnk"
[ "$(readlink "$work/proc.lnk")" = "/proc/self/status" ] || {
  echo "test-relocate: pass-through symlink modified" >&2; exit 1; }
[ "$(readlink "$work/rel.lnk")" = "../relative/target" ] || {
  echo "test-relocate: relative symlink modified" >&2; exit 1; }
[ "$(readlink "$work/anchored.lnk")" = "$inst/already/anchored" ] || {
  echo "test-relocate: anchored symlink modified" >&2; exit 1; }

# --- 8. appended page must not clobber trailing section headers -------
# (the append page sits at file offset == vaddr, above max(file size);
# a shorter placement corrupts the section table — qemu-user-compat fix)
cc -O2 -o "$work/hello2" "$work/hello.c"
"$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/hello2"
readelf -S "$work/hello2" >/dev/null 2>&1 || {
  echo "test-relocate: section table clobbered by append page" >&2; exit 1; }
readelf -lW "$work/hello2" | grep -q "interpreter: $inst/glibc" || {
  echo "test-relocate: interp lost after append" >&2; exit 1; }

# --- 9. signing gate: only aarch64 ELFs get signed (HarmonyOS PC) ------
# (if/else form: a bare failing command would trip set -e before the
# status assignment)
# the CLI has no sign subcommand; build the vendored checker instead
cc -O2 -o "$work/selfsign" "$repo/prelinux/src/selfsign.c"
if "$work/selfsign" --check "$work/hello2" >/dev/null 2>&1; then
  signed=0
else
  signed=1
fi
case "$(uname -m)" in
  aarch64) [ $signed -eq 0 ] || {
    echo "test-relocate: aarch64 ELF not signed after relocate" >&2; exit 1; } ;;
  x86_64)  [ $signed -ne 0 ] || {
    echo "test-relocate: x86_64 ELF wrongly signed" >&2; exit 1; } ;;
esac

# --- 10. oversized PT_INTERP segment is shrunk on a shorter rewrite ----
# A binary relocated once against a long (placeholder-sized) root keeps
# a long PT_INTERP segment.  Rewriting the path to a shorter root must
# shrink p_filesz too: the kernel reads the interpreter into a 256-byte
# buffer and refuses to exec anything longer (HarmonyOS PC returns
# ENOMEM, so dpkg did not run at all there — architecture notes, A.18).
cc -O2 -o "$work/hello3" "$work/hello.c"
longroot="/$(printf 'L%.0s' $(seq 200))"
"$repo/prelinux/prelinux" relocate --root "$longroot" --quiet "$work/hello3"
longinterp=$(readelf -lW "$work/hello3" | sed -n 's/.*interpreter: \(.*\)\]$/\1/p')
fsz_long=$(readelf -lW "$work/hello3" | awk '$1 == "INTERP" { print $5 }')
[ $(( fsz_long )) -eq $(( ${#longinterp} + 1 )) ] || {
  echo "test-relocate: long root PT_INTERP size $fsz_long != ${#longinterp}+1" >&2
  exit 1; }
"$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/hello3"
newinterp=$(readelf -lW "$work/hello3" | sed -n 's/.*interpreter: \(.*\)\]$/\1/p')
fsz_short=$(readelf -lW "$work/hello3" | awk '$1 == "INTERP" { print $5 }')
[ $(( fsz_short )) -eq $(( ${#newinterp} + 1 )) ] || {
  echo "test-relocate: PT_INTERP not shrunk ($fsz_short != ${#newinterp}+1)" >&2
  exit 1; }

# --- 11. an interpreter path beyond the kernel limit is refused --------
# (the instance root is too deep; import rejects it up front, relocate
# must refuse to write a binary the kernel cannot exec)
cc -O2 -o "$work/hello4" "$work/hello.c"
tooroot="/$(printf 'X%.0s' $(seq 240))"
if "$repo/prelinux/prelinux" relocate --root "$tooroot" --quiet \
     "$work/hello4" >/dev/null 2>&1; then
  echo "test-relocate: over-long interpreter accepted" >&2; exit 1
fi

echo "test-relocate: OK"

# --- 12. ET_EXEC: appended page honours the image's load bias --------
# Non-PIE binaries need the appended interpreter page at
# load_bias + file_offset: the kernel's AT_PHDR is
# (first PT_LOAD vaddr - offset) + e_phoff.  A page at vaddr == offset
# puts AT_PHDR in unmapped memory and every exec dies with SIGSEGV
# (gcc's cc1/lto1 on HarmonyOS PC, architecture notes, A.24).
if cc -no-pie -O2 -o "$work/hello-exec" "$work/hello.c" 2>/dev/null; then
  "$repo/prelinux/prelinux" relocate --root "$inst" --quiet "$work/hello-exec"
  loadinfo=$(readelf -lW "$work/hello-exec" |
	     awk '$1 == "LOAD" { print $3, $2; exit }')
  lvaddr=${loadinfo%% *}
  loff=${loadinfo##* }
  lbias=$(( lvaddr - loff ))
  phoff=$(readelf -h "$work/hello-exec" |
	  sed -n 's/.*Start of program headers: *\([0-9][0-9]*\).*/\1/p')
  phdr=$(readelf -lW "$work/hello-exec" | awk '$1 == "PHDR" { print $3 }')
  [ $(( phdr )) -eq $(( lbias + phoff )) ] || {
    echo "test-relocate: ET_EXEC PHDR vaddr $phdr != bias+phoff" \
	 "$((lbias + phoff))" >&2
    exit 1; }
fi

echo "test-relocate: OK"
