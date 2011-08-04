/*
 * Description: DeckLink test app - iterate cards and print some info.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2010,2011 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>

#include "DeckLinkAPI.h"

const char *appname = "dltest";

int main(int argc, char *argv[])
{
    /* create an object to enumerate all cards in the system */
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (iterator==NULL) {
        fprintf(stderr, "%s: error: could not initialise, the DeckLink driver may not be installed\n", appname);
        return 1;
    }

    /* enumerate all cards in this system */
    int num_devices = 0;
    IDeckLink *card;
    while (iterator->Next(&card) == S_OK) {

        /* increment the total number of cards found */
        num_devices++;
        if (num_devices>1)
            printf("\n");

        /* print the model name of the DeckLink card */
        char *name = NULL;
        HRESULT result = card->GetModelName((const char **) &name);
        if (result == S_OK) {
            printf("info: found a %s\n", name);
            free(name);
        }

        //print_attributes(deckLink);

        /* list the video output display modes supported by the card */
        do {
            /* query the card for its output interface */
            IDeckLinkOutput *interface = NULL;
            result = card->QueryInterface(IID_IDeckLinkOutput, (void**)&interface);
            if (result != S_OK) {
                fprintf(stderr, "%s: error: could not obtain the IDeckLinkOutput interface - result = %08x\n", appname, result);
                break;
            }

            /* obtain an IDeckLinkDisplayModeIterator to enumerate the display modes supported on output */
            IDeckLinkDisplayModeIterator *displaymode_iterator = NULL;
            result = interface->GetDisplayModeIterator(&displaymode_iterator);
            if (result != S_OK) {
                fprintf(stderr, "%s: error: could not obtain the video output display mode iterator - result = %08x\n", appname, result);
                interface->Release();
                break;
            }

            /* list all supported output display modes */
            printf("  output modes:");
            IDeckLinkDisplayMode *display_mode = NULL;
            while (displaymode_iterator->Next(&display_mode) == S_OK)
            {

                /* obtain the display mode's properties */
                long height = display_mode->GetHeight();
                BMDTimeValue framerate_duration;
                BMDTimeScale framerate_scale;
                display_mode->GetFrameRate(&framerate_duration, &framerate_scale);
                BMDFieldDominance field_dominance = display_mode->GetFieldDominance();

                if (framerate_duration==BMDTimeValue(1000))
                    printf(" %ld%c%d", height, field_dominance==bmdProgressiveFrame? 'p' : 'i', int(framerate_scale / framerate_duration));
                else
                    printf(" %ld%c%.2f", height, field_dominance==bmdProgressiveFrame? 'p' : 'i', (double)framerate_scale / (double)framerate_duration);

                /* tidy up */
                display_mode->Release();
            }
            printf("\n");


        } while(0);

        /* list the video input display modes supported by the card */
        do {
            /* query the card for its input interface */
            IDeckLinkInput *interface = NULL;
            result = card->QueryInterface(IID_IDeckLinkInput, (void**)&interface);
            if (result != S_OK) {
                fprintf(stderr, "%s: error: could not obtain the IDeckLinkInput interface - result = %08x\n", appname, result);
                break;
            }

            /* obtain an IDeckLinkDisplayModeIterator to enumerate the display modes supported on output */
            IDeckLinkDisplayModeIterator *displaymode_iterator = NULL;
            result = interface->GetDisplayModeIterator(&displaymode_iterator);
            if (result != S_OK) {
                fprintf(stderr, "%s: error: could not obtain the video input display mode iterator - result = %08x\n", appname, result);
                interface->Release();
                break;
            }

            /* list all supported output display modes */
            printf("   input modes:");
            IDeckLinkDisplayMode *display_mode = NULL;
            while (displaymode_iterator->Next(&display_mode) == S_OK)
            {

                /* obtain the display mode's properties */
                long height = display_mode->GetHeight();
                BMDTimeValue framerate_duration;
                BMDTimeScale framerate_scale;
                display_mode->GetFrameRate(&framerate_duration, &framerate_scale);
                BMDFieldDominance field_dominance = display_mode->GetFieldDominance();

                if (framerate_duration==BMDTimeValue(1000))
                    printf(" %ld%c%d", height, field_dominance==bmdProgressiveFrame? 'p' : 'i', int(framerate_scale / framerate_duration));
                else
                    printf(" %ld%c%.2f", height, field_dominance==bmdProgressiveFrame? 'p' : 'i', (double)framerate_scale / (double)framerate_duration);

                /* tidy up */
                display_mode->Release();
            }
            printf("\n");


        } while(0);

        /* list the input and output capabilities of the card */
        //print_capabilities(deckLink);

        /* tidy up */
        card->Release();
    }

    /* tidy up */
    iterator->Release();

    /* report no devices found */
    if (num_devices==0)
        printf("%s: no DeckLink cards were found\n", appname);

    return 0;
}
