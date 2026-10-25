#!/bin/sh
# packs initrd/ into initrd/initrd.tar. args are path:program pairs from mk/programs.mk
set -e

keep=""
for pair; do
    if [ -e "initrd/bin/${pair##*:}" ]; then
        keep="$keep ${pair%%:*}"
    fi
done

skip=""
excludes=""
for pair; do
    path=${pair%%:*}
    case " $keep $skip " in
        *" $path "*) ;;
        *) skip="$skip $path"; excludes="$excludes --exclude initrd/$path" ;;
    esac
done
[ -z "$skip" ] || echo "[TAR] Leaving out$skip"

mkdir -p initrd
rm -f initrd/initrd.tar
find initrd -name '._*' -delete
find initrd -name '.DS_Store' -delete
echo "[TAR] initrd/initrd.tar"
COPYFILE_DISABLE=1 tar --no-xattrs -cf initrd/initrd.tar $excludes initrd/*/* initrd/*.*
#this also gets rid of macos bullshit
