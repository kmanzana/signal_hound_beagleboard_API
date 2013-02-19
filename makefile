CC=g++
CFLAGS= -c 
LFLAGS= -lm -lrt $(shell pkg-config --libs gtk+-2.0)
LIBPATHS=-I/usr/local/include/libftdi -I/usr/local/include/libusb-1.0 \
 -I/usr/include/gtk-2.0  -I/usr/include/gtk-2.0/include \
 -I/usr/include/glib-2.0 -I/usr/include/glib-2.0/include \
 -I/usr/include/pango-1.0  -I/usr/X11R6/include \
 -I/usr/lib/glib-2.0/include -I/usr/include/cairo\
 -I/usr/lib/gtk-2.0/include  \
 -I/usr/include/freetype2 -I/usr/include/atk-1.0

 
LLIBPATHS= -L/usr/local/lib/ -L/usr/lib/ -L/usr/X11R6/lib/ -L \
 -lgtk-x11-2.0 -lgdk-x11-2.0 \
 -lgdk_pixbuf-2.0 \
 -lXft -lXrender -lXext -lX11 -lfreetype   \
 -lgobject-2.0 -lgmodule-2.0 -ldl -lglib-2.0

OBJS =  main.o

GTK1: $(OBJS)
	$(CC) $(LFLAGS) main.o libHOUND.a \
	  /usr/local/lib/libftdi.a \
	/usr/local/lib/libusb-1.0.a /usr/lib/librt.a -o GTK1

main.o : main.cpp 
	$(CC) $(CFLAGS) $(LIBPATHS) main.cpp 


