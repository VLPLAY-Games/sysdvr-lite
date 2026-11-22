#include "ui_language.h"
#include <stdlib.h>
#include <string.h>
static int russian;
void ui_language_init(void)
{
    const char *language = getenv("SYSDVR_LANGUAGE");
    russian = language && !strcmp(language, "ru");
}
int ui_is_russian(void) { return russian; }
