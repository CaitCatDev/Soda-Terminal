# Soda Terminal:
Soda Terminal's goal is to make a Linux and BSD terminal emulator. That works both in X11 and Wayland(possibly more). Hoping to support Ligatures and rendering with OpenGL and Vulkan.

## Features:
- [x] X11 Backend
- [x] WL Backend
- [ ] Vulkan Rendering
- [ ] OpenGL Rendering
- [x] Ligatures
- [x] Freetype Rendering

### Building:
call either `make` for a full build or meson/muon and ninja/samurai to do a configurable build with to disable x11 or wl support.

The make file is mostly POSIX and has been tested in `gmake, bmake, smake, dmake, pdpmake & svr4.make`, however for `bmake` the make command needs to be run twice. This is due to the generation of the `xdg-shell-protocol-code.c` as bmake seems to not process the file is there till restart(likely some form of caching/unaware of directory changes). So call `bmake xdg-shell-protocol-code.c && bmake` for bmake systems. 
