#!/bin/sh
set -e

# a checkout next to us wins over external/, found by the repos name
find_dir() {
    repo=$1
    name=$2
    dir=""
    if [ -n "$USERPROG_SRC" ] && [ -d "$USERPROG_SRC" ]; then
        dir=$(find "$USERPROG_SRC" -maxdepth 1 -type d -iname "${repo##*/}" | head -n 1)
    fi
    [ -n "$dir" ] || dir=$USER_BIN_DIR/$name
    echo "$dir"
}

lib_arg() {
    if [ -n "$USERPROG_SRC" ] && [ -d "$USERPROG_SRC/$2" ]; then
        echo "$1=$(cd "$USERPROG_SRC/$2" && pwd)"
    fi
}

build() {
    blibc_arg=$(lib_arg BLIBC_DIR blibc)
    crime_arg=$(lib_arg CRIME_DIR Crime-Scene)
    audio_arg=$(lib_arg AUDIO_DIR libaudio)
    mkdir -p "$USER_BIN_DIR" "$INITRD/bin"

    names=""
    for entry; do
        repo=${entry%% *}
        name=${entry##* }
        names="$names $name"
        dir=$(find_dir "$repo" "$name")

        if [ "$dir" != "$USER_BIN_DIR/$name" ]; then
            echo "[USER] Using local checkout $dir for $name"
        elif [ ! -d "$dir" ]; then
            echo "[USER] Cloning $name from $repo"
            git clone "$repo" "$dir"
        else
            echo "[USER] Pulling latest for $name"
            (cd "$dir" && git pull --rebase)
        fi

        echo "[USER] Preparing blibc for $name"
        $MAKE -s -C "$dir" blibc $USERPROG_TOOLCHAIN $blibc_arg
        echo "[USER] Building $name"
        $MAKE -s -C "$dir" $USERPROG_TOOLCHAIN $blibc_arg $crime_arg $audio_arg

        if [ ! -f "$dir/bin/$name" ]; then
            echo "ERROR: $name binary not found"
            exit 1
        fi
        COPYFILE_DISABLE=1 cp "$dir/bin/$name" "$INITRD/bin/$name"

        if grep -q '^initrd:' "$dir/Makefile"; then
            echo "[USER] Copying $name's files into the initrd"
            $MAKE -s -C "$dir" initrd INITRD="$INITRD" $USERPROG_TOOLCHAIN $blibc_arg
        fi
    done

    # whatever an older build of another profile left here would end up in the tar
    for f in "$INITRD"/bin/*; do
        [ -e "$f" ] || continue
        case " $names " in
            *" ${f##*/} "*) ;;
            *) echo "[USER] Removing ${f##*/}, its not in this profile"; rm -rf "$f" ;;
        esac
    done
}

disk() {
    out=$1
    shift
    for entry; do
        dir=$(find_dir "${entry%% *}" "${entry##* }")
        grep -q '^disk:' "$dir/Makefile" 2>/dev/null || continue
        echo "[DISK] Getting ${dir##*/}'s files"
        $MAKE -s -C "$dir" disk DISK="$out"
    done
}

distclean() {
    cleaned=""
    for entry; do
        dir=$(find_dir "${entry%% *}" "${entry##* }")
        [ "$dir" != "$USER_BIN_DIR/${entry##* }" ] || continue
        case " $cleaned " in *" $dir "*) continue ;; esac
        cleaned="$cleaned $dir"
        echo "[USER] distclean $dir"
        $MAKE -s -C "$dir" distclean || echo "[USER] distclean failed in $dir, carrying on"
    done

    [ -n "$USERPROG_SRC" ] || return 0
    for lib in $SHARED_LIBS; do
        [ -d "$USERPROG_SRC/$lib" ] || continue
        # blibc only has clean
        target=distclean
        grep -q '^distclean:' "$USERPROG_SRC/$lib/Makefile" 2>/dev/null || target=clean
        echo "[LIB] $target $USERPROG_SRC/$lib"
        $MAKE -s -C "$USERPROG_SRC/$lib" $target || echo "[LIB] $target failed in $lib, carrying on"
    done
}

action=$1
shift
case "$action" in
    build|disk|distclean) "$action" "$@" ;;
    *) echo "usage: $0 build|disk|distclean ..." >&2; exit 1 ;;
esac
