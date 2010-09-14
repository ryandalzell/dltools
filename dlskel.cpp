/*
 * Description: dltools skeleton app - copy to start a new app.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2010 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include "DeckLinkAPI.h"

#include "dlutil.h"

const char *appname = "dlskel";

void usage(int exitcode)
{
    fprintf(stderr, "%s: another great dltools program\n", appname);
    fprintf(stderr, "usage: %s [options] [<file>] [<file>...]\n", appname);
    fprintf(stderr, "  -n, --numframes     : number of frames (default: all)\n");
    fprintf(stderr, "  -o, --output        : write output to file\n");
    fprintf(stderr, "  -q, --quiet         : decrease verbosity, can be used multiple times\n");
    fprintf(stderr, "  -v, --verbose       : increase verbosity, can be used multiple times\n");
    fprintf(stderr, "  --                  : disable argument processing\n");
    fprintf(stderr, "  -u, --help, --usage : print this usage message\n");
    exit(exitcode);
}

int main(int argc, char *argv[])
{
    FILE *filein = stdin;
    FILE *fileout = stdout;
    char *filename[16] = {0};
    char *outfile = NULL;
    int numfiles = 0;

    /* command line defaults */
    int numframes = -1;
    int verbose = 0;

    /* parse command line for options */
    while (1) {
        static struct option long_options[] = {
            {"numframes", 1, NULL, 'n'},
            {"output",    1, NULL, 'o'},
            {"quiet",     0, NULL, 'q'},
            {"verbose",   0, NULL, 'v'},
            {"usage",     0, NULL, 'u'},
            {"help",      0, NULL, 'u'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "n:o:qvu", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
            case 'n':
                numframes = atoi(optarg);
                if (numframes<=0)
                    dlexit("invalid value for numframes: %d", numframes);
                break;

            case 'o':
                outfile = optarg;
                break;

            case 'q':
                verbose--;
                break;

            case 'v':
                verbose++;
                break;

            case 'u':
                usage(0);
                break;

            case '?':
                exit(1);
                break;
        }
    }

    /* all non-options are input filenames */
    while (optind<argc) {
        if (numfiles < sizeof(filename)/sizeof(filename[0]))
            filename[numfiles++] = argv[optind++];
        else
            dlexit("more than %d input files", numfiles);
    }

    /* initialise the DeckLink API */
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (iterator==NULL) {
        fprintf(stderr, "%s: error: could not initialise, the DeckLink driver may not be installed\n", appname);
        return 1;
    }

    /* connect to the first card in the system */
    IDeckLink *card;
    HRESULT result = iterator->Next(&card);
    if (result!=S_OK) {
        fprintf(stderr, "%s: error: no DeckLink cards found\n", appname);
        return 1;
    }

    /* print the model name of the DeckLink card */
    char *name = NULL;
    result = card->GetModelName((const char **) &name);
    if (result == S_OK) {
        printf("info: found a %s\n", name);
        free(name);
    }

    /* TODO body of app */

    /* tidy up */
    card->Release();
    iterator->Release();

    return 0;
}
