ALL_PROGRAMS := verdict doom quake2 specseek taskv tvi tetris crime-scene uikit-demo about \
                cabinet exhibit playback cat echo write ioctl tcc fastfetch halflife hlds

GH := https://github.com/Bleed-Kernel

REPO_verdict     := $(GH)/Verdict-Shell
REPO_doom        := $(GH)/Bleed-Doom
REPO_quake2      := $(GH)/Bleed-Quake2
REPO_specseek    := $(GH)/Bleed-SpecSeek
REPO_taskv       := $(GH)/Bleed-Taskman
REPO_tvi         := $(GH)/Bleed-tvi
REPO_tetris      := $(GH)/Bleed-Tetris
REPO_crime-scene := $(GH)/Crime-Scene
REPO_uikit-demo  := $(GH)/libuikit
REPO_about       := $(GH)/Bleed-About
REPO_cabinet     := $(GH)/Bleed-Cabinet
REPO_exhibit     := $(GH)/Bleed-Exhibit
REPO_playback    := $(GH)/Bleed-Playback
REPO_cat         := $(GH)/Bleed-Coreutils
REPO_echo        := $(GH)/Bleed-Coreutils
REPO_write       := $(GH)/Bleed-Coreutils
REPO_ioctl       := $(GH)/Bleed-Coreutils
REPO_tcc         := $(GH)/Bleed-TCC
REPO_fastfetch   := $(GH)/Bleed-Fastfetch
REPO_halflife    := $(GH)/Bleed-HalfLife
REPO_hlds        := $(GH)/Bleed-HalfLife

# stuff under initrd/ that only goes in the tar when one of its programs is in initrd/bin
ASSETS_doom     := doom1.wad soundfonts
ASSETS_quake2   := baseq2
ASSETS_playback := player soundfonts etc/1038.mp3
ASSETS_tcc      := lib include

# programs only put in a disk cause they make boot times too long in initrd and inflate the binary like crazy
DISK_PROGRAMS := halflife

SHARED_LIBS := blibc libaudio Bleed-Mediakit

program_entries = $(foreach p,$(1),"$(REPO_$(p)) $(p)")
INITRD_ASSETS   = $(foreach p,$(ALL_PROGRAMS),$(foreach a,$(ASSETS_$(p)),$(a):$(p)))
