.POSIX:
.SUFFIXES: .c .o

CC=cc
LD=cc
WL_SCANNER=`pkg-config --variable=wayland_scanner wayland-scanner`
WL_PROTOCOLS=`pkg-config --variable=pkgdatadir wayland-protocols`

CFLAGS=-O0 -g `pkg-config --cflags fontconfig freetype2 wayland-client xkbcommon harfbuzz`
LDFLAGS=`pkg-config --libs fontconfig freetype2 wayland-client xkbcommon harfbuzz`

TARGET=project_term
COBJS=./src/main.o ./xdg-shell-protocol-code.o

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
