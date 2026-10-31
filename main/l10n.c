#include "l10n.h"
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static atomic_int s_language = LANG_EN;
static const struct { const char *en, *ru; } k_strings[] = {
#define T(en, ru) { en, ru },
#include "l10n_ru.inc"
#undef T
};

void l10n_set(language_t language)
{
    atomic_store(&s_language, language < LANG_COUNT && language >= LANG_EN ? language : LANG_EN);
}

language_t l10n_current(void) { return atomic_load(&s_language); }
const char *l10n_name(language_t language) { return language == LANG_RU ? "Русский" : "English"; }

const char *tr(const char *english)
{
    if (!english || l10n_current() != LANG_RU) return english;
    for (size_t i = 0; i < sizeof k_strings / sizeof k_strings[0]; i++) {
        if (!strcmp(english, k_strings[i].en)) return k_strings[i].ru;
    }
    return english;
}

int l10n_format(char *out, size_t size, const char *format, ...)
{
    if (!out || !size) return 0;
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(out, size, tr(format), ap);
    va_end(ap);
    if (n < 0) { out[0] = 0; return 0; }
    if ((size_t)n < size) return n;
    size_t len = size - 1;
    /* If truncation cut a multibyte character, remove its incomplete tail. */
    size_t start = len;
    while (start && ((unsigned char)out[start - 1] & 0xc0) == 0x80) start--;
    if (start) {
        start--;
        unsigned char c = out[start];
        size_t width = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        if (len - start < width) len = start;
    }
    out[len] = 0;
    return (int)len;
}
