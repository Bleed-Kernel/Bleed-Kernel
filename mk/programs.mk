COREUTILS := cat echo write ioctl grep cp mv rm mkdir rmdir touch ln ls stat du df wc head tail tee cmp \
             sort uniq cut tr basename dirname pwd realpath seq yes sleep true false printf find xargs \
             hexdump dd uname whoami hostname date kill which ps audioout

ALL_PROGRAMS := verdict doom quake2 specseek taskv tvi tetris crime-scene about \
                cabinet exhibit playback bpm $(COREUTILS) tcc fastfetch curl halflife hlds

GH := https://github.com/Bleed-Kernel

REPO_verdict     := $(GH)/Verdict-Shell
REPO_doom        := $(GH)/Bleed-Doom
REPO_quake2      := $(GH)/Bleed-Quake2
REPO_specseek    := $(GH)/Bleed-SpecSeek
REPO_taskv       := $(GH)/Bleed-Taskman
REPO_tvi         := $(GH)/Bleed-tvi
REPO_tetris      := $(GH)/Bleed-Tetris
REPO_crime-scene := $(GH)/Crime-Scene
REPO_about       := $(GH)/Bleed-About
REPO_cabinet     := $(GH)/Bleed-Cabinet
REPO_exhibit     := $(GH)/Bleed-Exhibit
REPO_playback    := $(GH)/Bleed-Playback
REPO_bpm         := $(GH)/Bleed-BPM
$(foreach p,$(COREUTILS),$(eval REPO_$(p) := $(GH)/Bleed-Coreutils))
REPO_tcc         := $(GH)/Bleed-TCC
REPO_fastfetch   := $(GH)/Bleed-Fastfetch
REPO_curl        := $(GH)/Bleed-Curl

REPO_halflife    := $(GH)/Bleed-HalfLife
REPO_hlds        := $(GH)/Bleed-HalfLife
REPO_firefox     := $(GH)/Bleed-Firefox

# stuff under initrd/ (the root filesystem) that only goes in the tar when one of its programs is in initrd/bin
ASSETS_doom     := doom1.wad
ASSETS_quake2   := baseq2
ASSETS_playback := usr/share/playback usr/share/soundfonts
ASSETS_tcc      := usr/lib/tcc usr/lib/libc.a usr/lib/crt1.o usr/lib/crti.o usr/lib/crtn.o usr/include
ASSETS_curl     := etc/ssl

DESC_firefox  := Mozilla Firefox ESR
DESC_fastfetch := fastfetch system for Bleed
DESC_doom     := Doom shareware
DESC_quake2   := Quake II demo
DESC_halflife := Half-Life on Xash3D FWGS, with the hlds dedicated server
# the programs a package links into /usr/local/bin when its not just the one named after it
EXEC_halflife := halflife hlds

SHARED_LIBS := blibc libaudio Bleed-Mediakit

program_entries = $(foreach p,$(1),"$(REPO_$(p)) $(p)")
INITRD_ASSETS   = $(foreach p,$(ALL_PROGRAMS),$(foreach a,$(ASSETS_$(p)),$(a):$(p)))
PKG_ASSETS      = $(foreach p,$(BPKGS),$(foreach a,$(ASSETS_$(p)),$(a):$(p)))
