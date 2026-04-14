.POSIX:
.SUFFIXES: .c .o

CC=clang
LD=clang
WL_SCANNER=`pkg-config --variable=wayland_scanner wayland-scanner`
WL_PROTOCOLS=`pkg-config --variable=pkgdatadir wayland-protocols`

CFLAGS=-O0 -DTERM_WL_SUPPORT=1 -DTERM_X11_SUPPORT=1 -I ./include/ -I ./ -g -fsanitize=address `pkg-config --cflags fontconfig freetype2 wayland-client xkbcommon harfbuzz`
LDFLAGS=`pkg-config --libs fontconfig freetype2 wayland-client xkbcommon xkbcommon-x11 harfbuzz xcb xcb-shm xcb-xkb`

TARGET=project_terminal
COBJS=./src/main.o ./src/wayland.o ./src/x11.o ./xdg-shell-protocol-code.o

all: xdg-shell-client-protocol.h $(TARGET)

xdg-shell-protocol-code.c:
	@echo "Scanner: $(WL_SCANNER)"
	@echo "protcols: $(WL_PROTOCOLS)"
	$(WL_SCANNER) private-code $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-client-protocol.h:
	$(WL_SCANNER) client-header $(WL_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

$(TARGET): $(COBJS)
	$(CC) $(CFLAGS) $(COBJS) -o $@ $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm $(TARGET) $(COBJS)
