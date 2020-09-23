#pragma once

#include <stdio.h>

inline void print_sep()
{
    printf("\""
           "  },"
           "  {   \"full_text\": \"");
}

inline void print_gray()
{
    printf("\", \"color\": \"#aaaaaa");
}

inline void print_black()
{
    printf("\", \"color\": \"#000000");
}

inline void print_red()
{
    printf("\", \"color\": \"#ff9999");
}

inline void print_red_background()
{
    printf("\", \"background\": \"#ff9999");
}

inline void print_yellow()
{
    printf("\", \"color\": \"#ffff00");
}

inline void print_green()
{
    printf("\", \"color\": \"#00ff00");
}

inline void print_white_background()
{
    printf("\", \"background\": \"#ffffff");
}

