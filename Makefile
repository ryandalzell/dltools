# Makefile for dltools
# Ryan Dalzell, 13th Sep 2010

# Build configuration
BINDIR = /usr/local/bin
SDKDIR = /usr/local/decklink/include

# Targets
APPS = dlskel dltest dlplay

# Common files
OBJS = dlutil.o DeckLinkAPIDispatch.o

# Flags
CXXFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -g -I $(SDKDIR)
LFLAGS = -lm -ldl -lpthread

# Targets
all : $(APPS)
clean :
	rm -f $(APPS) $(foreach i,$(APPS),$i.o) $(OBJS)

$(APPS):
	$(CXX) -o $@ $(LFLAGS) $^

$(APPS): % : %.o $(OBJS)

DeckLinkAPIDispatch.o: $(SDKDIR)/DeckLinkAPIDispatch.cpp
	$(CXX) -c -o $@ $(CXXFLAGS) $<

%.o: %.cpp
	$(CXX) -c -o $@ $(CXXFLAGS) $<

