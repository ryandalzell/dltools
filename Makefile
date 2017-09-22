# Makefile for dltools
# Ryan Dalzell, 13th Sep 2010

# Build options
LIBYUV = 1
HEVC = 0

# Build configuration
BINDIR = /usr/local/bin
SDKDIR = /usr/local/decklink/include
PLATFORM = $(shell uname -p)

# Targets
APPS = dlskel dltest dlcap

# Common files
OBJS = dlutil.o dlterm.o dlconv.o dlts.o dlalloc.o dlsource.o dlformat.o DeckLinkAPIDispatch.o

# Flags
CXXFLAGS = -Wall -g -I $(SDKDIR)
ifeq ($(PLATFORM),i686)
CXXFLAGS += -D_FILE_OFFSET_BITS=64
endif

all  : CXXFLAGS += -O2 -ffast-math -fomit-frame-pointer
debug: CXXFLAGS +=
depend:CXXFLAGS += -MMD
LFLAGS = -lm -ldl -lpthread

# Options
ifeq ($(LIBYUV),1)
CXXFLAGS += -DHAVE_LIBYUV
LFLAGS += -lyuv
endif
ifeq ($(HEVC),1)
CXXFLAGS += -DHAVE_LIBDE265
LFLAGS += -lde265
endif

# Targets
all   : $(APPS) dlplay
debug : $(APPS) dlplay
depend: $(APPS) dlplay
clean :
	rm -f $(APPS) $(foreach i,$(APPS),$i.o) dlplay dlplay.o dldecode.o $(OBJS)

install: all
	install --strip $(filter-out dlskel,$(APPS) dlplay) $(BINDIR)

$(APPS):
	$(CXX) -o $@ $^ $(LFLAGS)

$(APPS): % : %.o $(OBJS)

DeckLinkAPIDispatch.o: $(SDKDIR)/DeckLinkAPIDispatch.cpp
	$(CXX) -c -o $@ $(CXXFLAGS) $<

%.o: %.cpp
	$(CXX) -c -o $@ $(CXXFLAGS) $<

dlplay: dlplay.o dldecode.o $(OBJS)
	$(CXX) -o $@ $^ $(LFLAGS) -lmpeg2 -lmpg123 -la52

dist: dltools.tar.gz

dltools.tar.gz: Makefile *.cpp *.h BUGS COPYING INSTALL
	tar zcf $@ -C .. $(foreach i,$^,dltools/$i)

# dependencies
DeckLinkAPIDispatch.o: \
 /usr/local/decklink/include/DeckLinkAPIDispatch.cpp \
 /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h
dlalloc.o: dlalloc.cpp dlutil.h /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h dlalloc.h
dlcap.o: dlcap.cpp /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h dlutil.h
dlconv.o: dlconv.cpp dlutil.h /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h
dldecode.o: dldecode.cpp dldecode.h dlutil.h \
 /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h dlformat.h \
 dlsource.h dlconv.h
dlformat.o: dlformat.cpp dlformat.h dlutil.h \
 /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h dlsource.h dlts.h
dlplay.o: dlplay.cpp /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h dlutil.h dlterm.h \
 dldecode.h dlformat.h dlsource.h dlalloc.h dlts.h
dlskel.o: dlskel.cpp /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h dlutil.h
dlsource.o: dlsource.cpp dlutil.h \
 /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h dlsource.h
dlterm.o: dlterm.cpp dlterm.h
dltest.o: dltest.cpp /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h
dlts.o: dlts.cpp dlutil.h /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h dlts.h dlsource.h
dlutil.o: dlutil.cpp dlutil.h /usr/local/decklink/include/DeckLinkAPI.h \
 /usr/local/decklink/include/LinuxCOM.h \
 /usr/local/decklink/include/DeckLinkAPITypes.h \
 /usr/local/decklink/include/DeckLinkAPIModes.h \
 /usr/local/decklink/include/DeckLinkAPIDiscovery.h \
 /usr/local/decklink/include/DeckLinkAPIConfiguration.h \
 /usr/local/decklink/include/DeckLinkAPIDeckControl.h
