/*
 * Description: DeckLink test app - iterate cards and print some info.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2010,2011 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>

#include "dlutil.h"
#include "DeckLinkAPI.h"

const char *appname = "dlinfo";

int main(int argc, char *argv[])
{
    /* create an object to enumerate all cards in the system */
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (iterator==NULL) {
        fprintf(stderr, "%s: error: could not initialise, the DeckLink driver may not be installed\n", appname);
        return 1;
    }

    /* query the API information */
    IDeckLinkAPIInformation *api = CreateDeckLinkAPIInformationInstance();
    const char *version;
    if (api->GetString(BMDDeckLinkAPIVersion, &version) == S_OK) {
        printf("info: decklink api %s\n", version);
        free((char *)version);
    }
    api->Release();

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
        if (1) {
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
            for (int i=0; displaymode_iterator->Next(&display_mode) == S_OK; i++)
            {
                const char *name = NULL;
                display_mode->GetName(&name);
                printf("%c %s", i? ',' : ' ', name);

                /* tidy up */
                free((char *)name);
                display_mode->Release();
            }
            printf("\n");

            displaymode_iterator->Release();
            interface->Release();
        }

        /* list the video input display modes supported by the card */
        if (1) {
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
            printf("  input modes:");
            IDeckLinkDisplayMode *display_mode = NULL;
            for (int i=0; displaymode_iterator->Next(&display_mode) == S_OK; i++)
            {
                const char *name = NULL;
                display_mode->GetName(&name);
                printf("%c %s", i? ',' : ' ', name);

                /* tidy up */
                free((char *)name);
                display_mode->Release();
            }
            printf("\n");

            displaymode_iterator->Release();
            interface->Release();
        }

        /* list the available profiles of the card */
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0b000000
        if (1) {
            /* query the card for its profile manager */
            IDeckLinkProfileManager *manager = NULL;
            switch (card->QueryInterface(IID_IDeckLinkProfileManager, (void **)&manager)) {
                case E_NOINTERFACE:
                    printf("  profiles: card only supports one profile\n");
                    break;

                case S_OK:
                    /* TODO iterate the available profiles */
                    printf("  profiles: TODO multiple profile support\n");
                    manager->Release();
                    break;

                default:
                    dlexit("error: could not obtain the profile manager interface");
            }
        }
#endif

        /* list the output configuration of the card */
        if (1) {
            /* query the card for its configuration interface */
            IDeckLinkConfiguration *config = NULL;
            if (card->QueryInterface(IID_IDeckLinkConfiguration, (void **)&config)!=S_OK)
                dlexit("error: could not obtain the configuration interface");

            /* obtain the default configuration */
            int64_t VideoConnections, LinkConfigurations, VideoOutputMode, VideoOutputModeFlags, VideoOutputConversion;
            bool SMPTELevelAOutput, Output1080pAsPsF;
            if (config->GetInt(bmdDeckLinkConfigVideoOutputConnection, &VideoConnections)!=S_OK)
                dlmessage("warning: failed to get card configuration for output SDI");
            if (config->GetInt(bmdDeckLinkConfigSDIOutputLinkConfiguration, &LinkConfigurations)!=S_OK)
                dlmessage("warning: failed to get card configuration for link SDI");
            if (config->GetFlag(bmdDeckLinkConfigSMPTELevelAOutput, &SMPTELevelAOutput)!=S_OK)
                dlmessage("warning: failed to get card configuration for SMPTE A");
            if (config->GetFlag(bmdDeckLinkConfigOutput1080pAsPsF, &Output1080pAsPsF))
                dlmessage("warning: failed to get card configuration");
            if (config->GetInt(bmdDeckLinkConfigDefaultVideoOutputMode, &VideoOutputMode)!=S_OK)
                dlmessage("warning: failed to get card configuration for video output mode");
            if (config->GetInt(bmdDeckLinkConfigDefaultVideoOutputModeFlags, &VideoOutputModeFlags)!=S_OK)
                dlmessage("warning: failed to get card configuration for video output mode flags");
            if (config->GetInt(bmdDeckLinkConfigVideoOutputConversionMode, &VideoOutputConversion)!=S_OK)
                dlmessage("warning: failed to get card configuration for video output conversion mode");

            /* display the default configuration */
            char s[4096];
            int n = 0;
            if (VideoConnections & bmdVideoConnectionSDI)
                n += snprintf(s+n, sizeof(s)-n, "SDI, ");
            if (VideoConnections & bmdVideoConnectionHDMI)
                n += snprintf(s+n, sizeof(s)-n, "HDMI, ");
            if (VideoConnections & bmdVideoConnectionOpticalSDI)
                n += snprintf(s+n, sizeof(s)-n, "OpticalSDI, ");
            if (VideoConnections & bmdVideoConnectionComponent)
                n += snprintf(s+n, sizeof(s)-n, "Component, ");
            if (VideoConnections & bmdVideoConnectionComposite)
                n += snprintf(s+n, sizeof(s)-n, "Composite, ");
            if (VideoConnections & bmdVideoConnectionSVideo)
                n += snprintf(s+n, sizeof(s)-n, "SVideo, ");
            printf("  output connections are: %s\n", s);
            const char *l = "";
            switch (LinkConfigurations)
            {
                case bmdLinkConfigurationSingleLink: l = "single-link"; break;
                case bmdLinkConfigurationDualLink: l = "dual-link"; break;
                case bmdLinkConfigurationQuadLink: l = "quad-link"; break;
            }
            printf("  default output config is: %s\n", l);
            printf("  default output: SMPTE Level %c, 1080p as PsF is: %s\n", SMPTELevelAOutput? 'A' : 'B', Output1080pAsPsF? "true" : "false");
            char mode[5] = {
                char((VideoOutputMode>>24)&0xff), char((VideoOutputMode>>16)&0xff), char((VideoOutputMode>>8)&0xff), char((VideoOutputMode>>0)&0xff), 0
            };
            printf("  default output video mode is %4s with flags 0x%lx\n", mode, VideoOutputModeFlags);

#if 0
            n = 0;
            if (VideoOutputConversion & bmdVideoOutputLetterboxDownconversion)
                n += snprintf(s+n, sizeof(s)-n, "Down-converted letterbox SD output\n");
            if (VideoOutputConversion & bmdVideoOutputAnamorphicDownconversion)
                n += snprintf(s+n, sizeof(s)-n, "Down-converted anamorphic SD output\n");
            if (VideoOutputConversion & bmdVideoOutputHD720toHD1080Conversion)
                n += snprintf(s+n, sizeof(s)-n, "HD720 to HD1080 conversion output\n");
            if (VideoOutputConversion & bmdVideoOutputHardwareLetterboxDownconversion)
                n += snprintf(s+n, sizeof(s)-n, "Simultaneous output of HD and down-converted letterbox SD\n");
            if (VideoOutputConversion & bmdVideoOutputHardwareAnamorphicDownconversion)
                n += snprintf(s+n, sizeof(s)-n, "Simultaneous output of HD and down-converted anamorphic SD\n");
            if (VideoOutputConversion & bmdVideoOutputHardwareCenterCutDownconversion)
                n += snprintf(s+n, sizeof(s)-n, "Simultaneous output of HD and center cut SD\n");
            if (VideoOutputConversion & bmdVideoOutputHardware720p1080pCrossconversion)
                n += snprintf(s+n, sizeof(s)-n, "The simultaneous output of 720p and 1080p cross-conversion\n");
            if (VideoOutputConversion & bmdVideoOutputHardwareAnamorphic720pUpconversion)
                n += snprintf(s+n, sizeof(s)-n, "The simultaneous output of SD and up-converted anamorphic 720p\n");
            if (VideoOutputConversion & bmdVideoOutputHardwareAnamorphic1080iUpconversion)
                n += snprintf(s+n, sizeof(s)-n, "The simultaneous output of SD and up-converted anamorphic 1080i\n");
            if (VideoOutputConversion & bmdVideoOutputHardwareAnamorphic149To720pUpconversion)
                n += snprintf(s+n, sizeof(s)-n, "The simultaneous output of SD and up-converted anamorphic widescreen aspect ratio 14:9 to 720p\n");
            if (VideoOutputConversion & bmdVideoOutputHardwareAnamorphic149To1080iUpconversion)
                n += snprintf(s+n, sizeof(s)-n, "The simultaneous output of SD and up-converted anamorphic widescreen aspect ratio 14:9 to 1080i\n");
            if (VideoOutputConversion & bmdVideoOutputHardwarePillarbox720pUpconversion)
                n += snprintf(s+n, sizeof(s)-n, "The simultaneous output of SD and up-converted pillarbox 720p\n");
            if (VideoOutputConversion & bmdVideoOutputHardwarePillarbox1080iUpconversion)
                n += snprintf(s+n, sizeof(s)-n, "The simultaneous output of SD and up-converted pillarbox 1080i\n");
            printf("    output conversions are: %s", s);
#endif

            /* display the card device information */
            const char *label, *serialno, *company;
            if (config->GetString(bmdDeckLinkConfigDeviceInformationLabel, &label)!=S_OK)
                label = "";
            if (config->GetString(bmdDeckLinkConfigDeviceInformationSerialNumber, &serialno)!=S_OK)
                serialno = "";
            if (config->GetString(bmdDeckLinkConfigDeviceInformationCompany, &company)!=S_OK)
                company = "";
            printf("    label: %s, serial %s, company %s\n", label, serialno, company);

            config->Release();
        }

        /* tidy up */
        card->Release();
    }

    /* tidy up */
    iterator->Release();

    /* report no devices found */
    if (num_devices==0)
        dlmessage("no DeckLink cards were found");

    return 0;
}
