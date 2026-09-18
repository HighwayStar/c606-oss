#pragma once
#include <stdint.h>

/* Page "fences": a page is a stack of rows, each row has 1 or 2 cells and a
 * relative height (weight 1 or 2). Named like the reference device
 * (1, 2, 3A, 3B, 4A ... 12); the name's number is the cell count. */

#define LAYOUT_MAX_ROWS  6
#define LAYOUT_MAX_CELLS 12

typedef struct { uint8_t cells, weight; } layout_row_t;
typedef struct {
    const char *name;
    layout_row_t rows[LAYOUT_MAX_ROWS];   /* cells == 0 terminates */
} layout_t;

int layout_count(void);
const layout_t *layout_get(int idx);        /* idx clamped to a valid one */
int layout_cells(const layout_t *l);

/* Rectangle of cell `cell` (row-major, left to right) inside an area of
 * w x h pixels at (0,0). */
void layout_cell_rect(const layout_t *l, int cell, int w, int h, int *x, int *y, int *cw, int *ch);

/* Index of the layout with this name, 0 if unknown. */
int layout_index(const char *name);
