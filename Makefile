# Makefile for dltools
# Ryan Dalzell, 13th Sep 2010

# Build configuration
BINDIR = /usr/local/bin
SDKDIR = /usr/local/decklink/include

# Targets
APPS = dltest

# FLAGS
CXXFLAGS = -Wall -D_FILE_OFFSET_BITS=64 -g -I $(SDKDIR)
LFLAGS = -lm -ldl -lpthread DeckLinkAPIDispatch.o

# Targets
all : $(APPS)
clean :
	rm -f $(APPS) $(foreach i,$(APPS),$i.o) DeckLinkAPIDispatch.o

$(APPS):
	$(CXX) -o $@ $(LFLAGS) $@.o 

$(APPS): % : %.o DeckLinkAPIDispatch.o

DeckLinkAPIDispatch.o: $(SDKDIR)/DeckLinkAPIDispatch.cpp
	$(CXX) -c -o $@ $(CXXFLAGS) $<
