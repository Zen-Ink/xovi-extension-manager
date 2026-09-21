objects = main.o inventory.o jsonutil.o settings.o notifications.o xovi.o

XOVI_REPO ?= $(CURDIR)/xovi
XOVIGEN ?= $(XOVI_REPO)/util/xovigen.py
CXXFLAGS += -D_GNU_SOURCE -fPIC -std=c++17 -I. $(shell pkg-config --cflags Qt6Core)
name = xovi-extension-manager
VPATH = src

xovi-extension-manager : $(objects)
	${CXX} ${CXXFLAGS} ${LDFLAGS} -Wl,-Bsymbolic -shared -o $(name).so $(objects) $(shell pkg-config --libs Qt6Core)

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
