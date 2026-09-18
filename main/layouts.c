#include "layouts.h"

/* {cells, weight} per row, top to bottom */
static const layout_t k_layouts[] = {
    { "1",   0, { {1,1} } },
    { "2", 0, { {1,1}, {1,1} } },
    { "3A", 0, { {1,1}, {1,1}, {1,1} } },
    { "3B", 0, { {1,2}, {1,1}, {1,1} } },
    { "4A", 0, { {1,2}, {1,1}, {1,1}, {1,1} } },
    { "4B", 0, { {2,1}, {1,2}, {1,2} } },
    { "4C", 0, { {1,1}, {2,2}, {1,1} } },
    { "5A", 0, { {1,1}, {1,1}, {1,2}, {1,1}, {1,1} } },
    { "5B", 0, { {1,1}, {1,2}, {1,2}, {2,1} } },
    { "6A", 0, { {1,1}, {1,1}, {1,1}, {1,1}, {1,1}, {1,1} } },
    { "6B", 0, { {1,1}, {1,1}, {1,2}, {1,1}, {2,1} } },
    { "6C", 0, { {1,2}, {1,1}, {2,1}, {2,1} } },
    { "7A", 0, { {1,1}, {1,1}, {1,2}, {2,1}, {2,1} } },
    { "7B", 0, { {2,1}, {1,2}, {2,2}, {2,1} } },
    { "8A", 0, { {1,1}, {2,1}, {1,2}, {2,1}, {2,1} } },
    { "9A", 0, { {2,1}, {2,1}, {1,2}, {2,1}, {2,1} } },
    { "10A", 0, { {1,1}, {2,1}, {1,1}, {2,1}, {2,1}, {2,1} } },
    { "12", 0, { {2,1}, {2,1}, {2,1}, {2,1}, {2,1}, {2,1} } },
    /* map page: map only, map + 1 field, map + 2 fields side by side */
    { "Map", 1, { {0,0} } },
    { "M1",  1, { {1,1} } },
    { "M2",  1, { {2,1} } },
};
#define N (int)(sizeof k_layouts / sizeof k_layouts[0])

int layout_count(void) { return N; }

const layout_t *layout_get(int idx)
{
    if (idx < 0 || idx >= N) idx = 0;
    return &k_layouts[idx];
}

int layout_next(int idx, int dir, bool map)
{
    for (int i = 0; i < N; i++) {
        idx = (idx + N + dir) % N;
        if (!!k_layouts[idx].map == map) return idx;
    }
    return idx;
}

int layout_strip_h(const layout_t *l)
{
    if (!l->map) return 0;
    int rows = 0;
    for (int r = 0; r < LAYOUT_MAX_ROWS && l->rows[r].cells; r++) rows++;
    return rows * LAYOUT_MAP_ROW_H;
}

int layout_cells(const layout_t *l)
{
    int n = 0;
    for (int r = 0; r < LAYOUT_MAX_ROWS && l->rows[r].cells; r++) n += l->rows[r].cells;
    return n;
}

void layout_cell_rect(const layout_t *l, int cell, int w, int h, int *x, int *y, int *cw, int *ch)
{
    int wsum = 0, nrows = 0;
    for (int r = 0; r < LAYOUT_MAX_ROWS && l->rows[r].cells; r++) { wsum += l->rows[r].weight; nrows++; }
    if (!wsum) wsum = 1;
    int idx = 0, top = 0;
    for (int r = 0; r < nrows; r++) {
        int rh = (r == nrows - 1) ? h - top : h * l->rows[r].weight / wsum;
        for (int c = 0; c < l->rows[r].cells; c++, idx++) {
            if (idx == cell) {
                int cwid = w / l->rows[r].cells;
                *x = c * cwid;
                *y = top;
                *cw = (c == l->rows[r].cells - 1) ? w - *x : cwid;
                *ch = rh;
                return;
            }
        }
        top += rh;
    }
    *x = *y = *cw = *ch = 0;
}

int layout_index(const char *name)
{
    for (int i = 0; i < N; i++) {
        const char *a = k_layouts[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return i;
    }
    return 0;
}
