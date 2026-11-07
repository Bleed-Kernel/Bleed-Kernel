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

repo_done() {
    for pair in $done_repos; do
        if [ "${pair%%=*}" = "$1" ]; then
            echo "${pair#*=}"
            return
        fi
    done
}

has_target() {
    cat "$1/Makefile" "$1"/*.mk 2>/dev/null | grep -q "^$2:"
}

fetch() {
    repo=$1
    name=$2
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
}

lib_args() {
    blibc_arg=$(lib_arg BLIBC_DIR blibc)
    crime_arg=$(lib_arg CRIME_DIR Crime-Scene)
    audio_arg=$(lib_arg AUDIO_DIR libaudio)
}

compile() {
    echo "[USER] Preparing blibc for $2"
    $MAKE -s -C "$1" blibc $USERPROG_TOOLCHAIN $blibc_arg
    echo "[USER] Building $2"
    $MAKE -s -C "$1" $USERPROG_TOOLCHAIN $blibc_arg $crime_arg $audio_arg
}

build() {
    lib_args
    mkdir -p "$USER_BIN_DIR" "$INITRD/bin"

    names=""
    done_repos=""
    for entry; do
        repo=${entry%% *}
        name=${entry##* }

        # one repo can make lots of programs (coreutils), get it and build it once
        dir=$(repo_done "$repo")
        if [ -z "$dir" ]; then
            fetch "$repo" "$name"
            compile "$dir" "$name"
            done_repos="$done_repos $repo=$dir"
        fi

        if [ ! -f "$dir/bin/$name" ]; then
            echo "ERROR: $name binary not found"
            exit 1
        fi

        names="$names $name"
        COPYFILE_DISABLE=1 cp "$dir/bin/$name" "$INITRD/bin/$name"

        if grep -q '^initrd:' "$dir/Makefile"; then
            echo "[USER] Copying $name's files into the initrd"
            $MAKE -s -C "$dir" initrd INITRD="$INITRD" $USERPROG_TOOLCHAIN $blibc_arg
        fi
    done

    # libc.so is the dynamic linker too, dynamically linked programs name /lib/libc.so
    blibc_dir=${blibc_arg#BLIBC_DIR=}
    if [ -z "$blibc_dir" ]; then
        blibc_dir=$(dirname "$(dirname "$(find "$USER_BIN_DIR" -path '*/blibc/sysroot/lib/libc.so' | head -n 1)")" 2>/dev/null)/..
    fi
    if [ -f "$blibc_dir/sysroot/lib/libc.so" ]; then
        mkdir -p "$INITRD/lib"
        COPYFILE_DISABLE=1 cp "$blibc_dir/sysroot/lib/libc.so" "$INITRD/lib/libc.so"
    fi

    # c++ programs linked against libc++.so (firefox) find it in /lib. only copied when
    # Bleed-LibCXX has been built next to us, building it pulls all of llvm
    if [ -n "$USERPROG_SRC" ] && [ -f "$USERPROG_SRC/Bleed-LibCXX/sysroot/lib/libc++.so" ]; then
        mkdir -p "$INITRD/lib"
        COPYFILE_DISABLE=1 cp "$USERPROG_SRC/Bleed-LibCXX/sysroot/lib/libc++.so" "$INITRD/lib/libc++.so"
    fi

    # whatever an older build of another profile left here would end up in the tar
    for f in "$INITRD"/bin/*; do
        [ -e "$f" ] || continue
        case " $names " in
            *" ${f##*/} "*) ;;
            *) echo "[USER] Removing ${f##*/}, its not in this profile"; rm -rf "$f" ;;
        esac
    done
}

bpkg() {
    out=$1
    shift
    lib_args
    mkdir -p "$USER_BIN_DIR" "$out" "$PKG_STAGE"

    # packages that arent wanted any more, versions are dates so the name is before the last -
    wanted=""
    for entry; do wanted="$wanted ${entry##* }"; done
    for f in "$out"/*.bpkg; do
        [ -e "$f" ] || continue
        base=${f##*/}
        case " $wanted " in *" ${base%-*} "*) ;; *) echo "[BPKG] dropping $base"; rm -f "$f" ;; esac
    done

    for entry; do
        repo=${entry%% *}
        name=${entry##* }
        var=$(echo "$name" | tr -c 'a-zA-Z0-9\n' _)
        desc=$(eval echo "\${DESC_$var}")
        exec=$(eval echo "\${EXEC_$var}")

        fetch "$repo" "$name"
        if has_target "$dir" bpkg; then
            echo "[BPKG] $name from ${dir##*/}"
            $MAKE -s -C "$dir" bpkg BPKG_OUT="$out" MKBPKG="$MKBPKG"
            continue
        fi

        compile "$dir" "$name"
        stage=$PKG_STAGE/$name
        if has_target "$dir" disk; then
            echo "[BPKG] staging $name with ${dir##*/}'s disk target"
            $MAKE -s -C "$dir" disk DISK="$PKG_STAGE"
        else
            [ -f "$dir/bin/$name" ] || { echo "ERROR: $name binary not found"; exit 1; }
            rm -rf "$stage"
            mkdir -p "$stage"
            COPYFILE_DISABLE=1 cp "$dir/bin/$name" "$stage/"
            for pair in $PKG_ASSETS; do
                [ "${pair##*:}" = "$name" ] || continue
                COPYFILE_DISABLE=1 cp -R "$INITRD/${pair%%:*}" "$stage/"
            done
        fi
        sh "$(dirname "$0")/packpkg.sh" "$MKBPKG" "$stage" "$out" "$name" "$desc" "$exec"
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
    build|bpkg|distclean) "$action" "$@" ;;
    *) echo "usage: $0 build|bpkg|distclean ..." >&2; exit 1 ;;
esac
