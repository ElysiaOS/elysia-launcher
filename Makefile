CXX=g++
CXXFLAGS=-std=c++17 -O2 `pkg-config --cflags gtk+-3.0 gdk-3.0 gtk-layer-shell-0`
LIBS=`pkg-config --libs gtk+-3.0 gdk-3.0 gtk-layer-shell-0` -lcurl -ljsoncpp -lssl -lcrypto

all: ely_launcher

ely_launcher: main.cpp apps.cpp emoji.cpp gif.cpp files.cpp wallpaper.cpp ely_launcher.h
	$(CXX) $(CXXFLAGS) -o ely_launcher main.cpp $(LIBS)

clean:
	rm -f ely_launcher

