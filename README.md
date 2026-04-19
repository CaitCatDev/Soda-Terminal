# PROJECT-TERMINAL
a better name is pending but the goal is to make a Linux and X11 terminal supporting ligatures and rendering with OGL or VK.

### Building:
call either `make` for a full build or meson/muon and ninja/samurai to do a configurable build with to disable x11 or wl support.

The make file is mostly POSIX and has been tested in `gmake, bmake, smake, dmake, pdpmake & svr4.make`, however for `bmake` the make command needs to be run twice. This is due to the generation of the `xdg-shell-protocol-code.c` as bmake seems to not process the file is there till restart(likely some form of caching/unaware of directory changes). So call `bmake xdg-shell-protocol-code.c && bmake` for bmake systems. 
