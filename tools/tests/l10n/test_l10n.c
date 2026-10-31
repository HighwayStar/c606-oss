#include "l10n.h"
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static void utf8(const char *s)
{
    mbstate_t state = {0};
    const char *p = s;
    assert(mbsrtowcs(NULL, &p, 0, &state) != (size_t)-1);
}

int main(void)
{
    assert(setlocale(LC_CTYPE, "C.UTF-8"));
    l10n_set(LANG_EN);
    assert(!strcmp(tr("Settings"), "Settings"));
    l10n_set(LANG_RU);
    assert(!strcmp(tr("Settings"), "Настройки"));
    assert(!strcmp(tr("my-route.gpx"), "my-route.gpx"));
    assert(tr(NULL) == NULL);
    char buf[128];
    l10n_format(buf, sizeof buf, "Page %d", 3);
    assert(!strcmp(buf, "Экран 3"));
    l10n_format(buf, sizeof buf, "GPS %s  %u/%u sats", tr("fix"), 7u, 12u);
    assert(!strcmp(buf, "GPS позиция  7/12 спут."));
    const char *text = "Ёжик — Русский";
    for (size_t size = 1; size <= strlen(text) + 2; size++) {
        memset(buf, 0x55, sizeof buf);
        int n = l10n_format(buf, size, "%s", text);
        assert(n >= 0 && (size_t)n < size && (size_t)n == strlen(buf));
        assert((unsigned char)buf[size] == 0x55);
        utf8(buf);
    }
    assert(l10n_format(NULL, 0, "%s", text) == 0);
#define T(en, ru) assert(!strcmp(tr(en), ru)); utf8(ru);
#include "l10n_ru.inc"
#undef T
    l10n_set(LANG_COUNT);
    assert(l10n_current() == LANG_EN);
    l10n_set((language_t)-1);
    assert(l10n_current() == LANG_EN);
    puts("PASS: translations, fallback, formats, UTF-8 truncation, language validation");
}
