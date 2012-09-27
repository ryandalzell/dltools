/*
 * Description: asynchronous input from terminal
 * Author     : Beginning Linux Programming
 * Copyright  : (c) Wrox.
 * License    : GPL
 */

#include <unistd.h>

#include "dlterm.h"

dlterm::dlterm()
{
    peek_character = -1;
    tcgetattr(0,&initial_settings);
    new_settings = initial_settings;
    new_settings.c_lflag &= ~ICANON;
    new_settings.c_lflag &= ~ECHO;
    new_settings.c_cc[VMIN] = 1;
    new_settings.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &new_settings);
}

dlterm::~dlterm()
{
    tcsetattr(0, TCSANOW, &initial_settings);
}

int dlterm::kbhit()
{
    char ch;
    int nread;

    if (peek_character != -1)
        return 1;
    new_settings.c_cc[VMIN] = 0;
    tcsetattr(0, TCSANOW, &new_settings);
    nread = read(0,&ch,1);
    new_settings.c_cc[VMIN] = 1;
    tcsetattr(0, TCSANOW, &new_settings);

    if (nread == 1) {
        peek_character = ch;
        return 1;
    }
    return 0;
}

int dlterm::readchar()
{
    char ch;
    ssize_t r;

    if (peek_character != -1) {
        ch = peek_character;
        peek_character = -1;
        return ch;
    }
    r = read(0,&ch,1);
    return ch;
}
