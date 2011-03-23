#ifndef DLTERM_H
#define DLTERM_H

#include <termios.h>

/* terminal class */
class dlterm
{
public:
    dlterm();
    ~dlterm();

    int kbhit();
    int readchar();

private:
    struct termios initial_settings;
    struct termios new_settings;
    int peek_character;
};

#endif
