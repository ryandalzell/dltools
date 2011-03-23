# Makefile for dltools
# Ryan Dalzell, 13th Sep 2010

# Build configuration
BINDIR = /usr/local/bin
SDKDIR = /usr/local/decklink/include

# Targets
APPS = dlskel dltest

# Common files
OBJS = dlutil.o dlterm.o DeckLinkAPIDispatch.o

# Flags
CXXFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -g -I $(SDKDIR)
all  : CXXFLAGS += -O2
debug: CXXFLAGS +=
LFLAGS = -lm -ldl -lpthread

# Targets
all   : $(APPS) dlplay
debug : $(APPS) dlplay
clean :
	rm -f $(APPS) $(foreach i,$(APPS),$i.o) $(OBJS)

$(APPS):
	$(CXX) -o $@ $(LFLAGS) $^

$(APPS): % : %.o $(OBJS)

DeckLinkAPIDispatch.o: $(SDKDIR)/DeckLinkAPIDispatch.cpp
	$(CXX) -c -o $@ $(CXXFLAGS) $<

%.o: %.cpp
	$(CXX) -c -o $@ $(CXXFLAGS) $<

dlplay: dlplay.o $(OBJS)
	$(CXX) -o $@ $^ $(LFLAGS) -lmpeg2 -lmpeg2convert -lmpg123 -la52
