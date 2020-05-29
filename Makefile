all:
	$(CC) -shared $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Werror=implicit-function-declaration $(shell pkg-config --cflags liblzma gdk-pixbuf-2.0) -o libpixbufloader-xz.so $(LDFLAGS) xz-pixbuf-loader.c $(shell pkg-config --libs liblzma gdk-pixbuf-2.0) $(LIBS)
install:
	install -c -d /usr/lib/gdk-pixbuf-2.0/2.10.0/loaders
	install -c -m 755 -s libpixbufloader-xz.so /usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/
	gdk-pixbuf-query-loaders --update-cache
