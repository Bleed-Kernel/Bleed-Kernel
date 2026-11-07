#pragma once

#define PACKAGES_DIR    "opt"
#define PACKAGES_LINKS  "/usr/local/bin"

/// @brief link the programs (exec= in .bpkg/PKGINFO) of every package installed on a mounted
/// drive into PACKAGES_LINKS, and drop links that point at something thats gone. run again
/// whenever a drive turns up or goes away
void packages_scan(void);
