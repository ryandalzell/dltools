# Makefile for dltools
# Ryan Dalzell, 13th Sep 2010

# Build configuration
BINDIR = /usr/local/bin
SDKDIR = /usr/local/decklink/include

# Targets
APPS = dlskel dltest

# Common files
OBJS = dlutil.o dlterm.o dlconv.o dlts.o DeckLinkAPIDispatch.o

# Flags
CXXFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -g -I $(SDKDIR)
all  : CXXFLAGS += -O2 -ffast-math -fomit-frame-pointer
debug: CXXFLAGS +=
LFLAGS = -lm -ldl -lpthread

# Targets
all   : $(APPS) dlplay
debug : $(APPS) dlplay
clean :
	rm -f $(APPS) dlplay $(foreach i,$(APPS),$i.o) $(OBJS)

install: all
	install --strip $(filter-out dlskel,$(APPS) dlplay) $(BINDIR)

$(APPS):
	$(CXX) -o $@ $(LFLAGS) $^

$(APPS): % : %.o $(OBJS)

DeckLinkAPIDispatch.o: $(SDKDIR)/DeckLinkAPIDispatch.cpp
	$(CXX) -c -o $@ $(CXXFLAGS) $<

%.o: %.cpp
	$(CXX) -c -o $@ $(CXXFLAGS) $<

dlplay: dlplay.o dldecode.o $(OBJS)
	$(CXX) -o $@ $^ $(LFLAGS) -lmpeg2 -lmpeg2convert -lmpg123 -la52

# dependancies
dlplay.o: dlutil.h dlterm.h dlconv.h dldecode.h
dlterm.o: dlutil.h dlterm.h
dlconv.o: dlutil.h dlconv.h
dldecode.o : dlutil.h dldecode.h
