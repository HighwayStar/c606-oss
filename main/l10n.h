#pragma once
#include <stddef.h>

typedef enum { LANG_EN = 0, LANG_RU, LANG_COUNT } language_t;
void l10n_set(language_t language);
language_t l10n_current(void);
const char *l10n_name(language_t language);
/* Unknown keys (including user filenames) pass through unchanged. */
const char *tr(const char *english);
/* Returns bytes actually written, excluding NUL; never splits UTF-8. */
int l10n_format(char *out, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
