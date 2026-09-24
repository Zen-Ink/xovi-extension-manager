objects = main.o inventory.o jsonutil.o settings.o notifications.o sockets.o xovi.o

XOVI_REPO ?= $(CURDIR)/xovi
XOVIGEN ?= $(XOVI_REPO)/util/xovigen.py
CXXFLAGS += -D_GNU_SOURCE -fPIC -std=c++17 -I. $(shell pkg-config --cflags Qt6Core Qt6Network)
name = xovi-extension-manager
VPATH = src

xovi-extension-manager : $(objects)
	${CXX} ${CXXFLAGS} ${LDFLAGS} -Wl,-Bsymbolic -shared -o $(name).so $(objects) $(shell pkg-config --libs Qt6Core Qt6Network)

xovi.cpp xovi.h manifest.json	&: $(name).xovi $(XOVIGEN)
	python3 $(XOVIGEN) -o xovi.cpp -H xovi.h -m manifest.json --manifest-entry $(name).so $(name).xovi

main.o		: xovi.h src/main.cpp src/inventory.h
inventory.o	: xovi.h src/inventory.cpp src/inventory.h src/jsonutil.h
jsonutil.o	: src/jsonutil.cpp src/jsonutil.h

.PHONY  : clean
clean :
	rm -f $(name).so manifest.json $(objects) xovi.cpp xovi.h

settings.o: xovi.h src/settings.cpp src/settings.h sdk/xovi-settings.h sdk/qrr-api.h

notifications.o: src/notifications.cpp src/notifications.h sdk/xovi-notifications.h

main.o inventory.o settings.o notifications.o: src/diagnostics.h
main.o settings.o notifications.o: src/diagnostics_qt.h

sockets.o: src/sockets.cpp src/sockets.h sdk/xovi-sockets.h src/diagnostics.h src/diagnostics_qt.h
