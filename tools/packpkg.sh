#!/bin/sh
set -e

mkbpkg=$1
dir=$2
out=$3
name=$4
desc=$5
exec=$6

[ -d "$dir" ] || { echo "[BPKG] ERROR: $dir wasnt staged"; exit 1; }
mkdir -p "$out"

sumfile=$out/.$name.sum
sum=$(cd "$dir" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 cksum | cksum)
old=$(ls "$out/$name"-*.bpkg 2>/dev/null | head -n 1)
if [ -n "$old" ] && [ "$sum" = "$(cat "$sumfile" 2>/dev/null)" ]; then
    echo "[BPKG] ${old##*/} is up to date"
    exit 0
fi

rm -f "$out/$name"-*.bpkg
version=$(date +%Y.%m.%d)
"$mkbpkg" -n "$name" -v "$version" ${desc:+-d "$desc"} ${exec:+-e "$exec"} -o "$out/$name-$version.bpkg" "$dir"
echo "$sum" > "$sumfile"
