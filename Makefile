.POSIX:
.SUFFIXES: .c .o

CC=clang
LD=clang
WL_SCANNER=`pkg-config --variable=wayland_scanner wayland-scanner`
WL_PROTOCOLS=`pkg-config --variable=pkgdatadir wayland-protocols`

CFLAGS=-O0 -std=c99 -DTERM_WL_SUPPORT=1 -DTERM_X11_SUPPORT=1 -I ./include/ -I ./ -g `pkg-config --cflags fontconfig freetype2 wayland-client xkbcommon harfbuzz`
LDFLAGS=`pkg-config --libs fontconfig freetype2 wayland-client xkbcommon xkbcommon-x11 harfbuzz xcb xcb-shm xcb-xkb`

TARGET=project_terminal
COBJS=./src/main.o ./src/wayland.o ./src/x11.o ./xdg-shell-protocol-code.o ./src/log.o ./src/font.o

all: xdg-shell-client-protocol.h xdg-shell-protocol-code.c $(TARGET)

xdg-shell-protocol-code.c:
	@echo "Scanner: $(WL_SCANNER)"
	@echo "Protcols: $(WL_PROTOCOLS)"
	$(WL_SCANNER) private-code $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-client-protocol.h:
	@echo "Scanner: $(WL_SCANNER)"
	@echo "Protcols: $(WL_PROTOCOLS)"
	$(WL_SCANNER) client-header $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol-code.o: xdg-shell-protocol-code.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(COBJS)
	$(CC) $(CFLAGS) $(COBJS) -o $@ $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm $(TARGET) $(COBJS)
