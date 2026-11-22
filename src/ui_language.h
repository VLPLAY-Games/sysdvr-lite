#ifndef UI_LANGUAGE_H
#define UI_LANGUAGE_H
/* SYSDVR_LANGUAGE=en|ru; English is the default. Read once at startup. */
void ui_language_init(void);
int ui_is_russian(void);
#endif
