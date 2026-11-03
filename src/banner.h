#ifndef BANNER_H
#define BANNER_H

/* A short status text on the text console (tty1), in English and Russian, for the time the client waits for the
 * console: the screen is cleared and the text is written like any terminal output, so that someone looking at
 * the TV after the boot sees that everything is running and the Switch can be connected. When setfont and a
 * console font with Cyrillic letters are available the font is changed too (bigger on a large screen), otherwise
 * only the English part is written. Does nothing when there is no /dev/tty1. */
void banner_set(const char *decoder, const char *audio);   /* what to report; call before banner_show */
void banner_show(void);

#endif
