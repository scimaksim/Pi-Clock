/*
 * gcat.h - End-of-life dates from Jonathan McDowell's General Catalog
 * of Artificial Space Objects (GCAT, planet4589.org, CC-BY).
 */
#ifndef PIC_GCAT_H
#define PIC_GCAT_H

#include <stddef.h>

/* Download the payload catalogues when the saved copies are over three
 * days old. Returns the number of catalogue files available (0-2). */
int pic_gcat_refresh(void);

/* Column positions, read from a catalogue's "#JCAT ..." header line.
 * Returns 0 when the line has the columns needed, -1 otherwise. */
typedef struct { int top, tdate, tf, max; } pic_gcat_cols_t;
int pic_gcat_header(const char *line, pic_gcat_cols_t *c);

/*
 * One catalogue row. Returns 1 and fills *norad and note ("defunct
 * Apr 2012", "defunct about 2020", "retired Mar 1984") when the row
 * says the satellite no longer works; 0 when it is believed working,
 * or the row can't be read. "defunct" is GCAT's end-of-transmission
 * date; "retired" is its end-of-operations date for a satellite it
 * flags as no longer controlled but still sending a beacon.
 */
int pic_gcat_row(const char *line, const pic_gcat_cols_t *c, int *norad,
                 char *note, size_t n);

/* Call fn(norad, note, ud) for every no-longer-working satellite in the
 * saved catalogues. Returns rows read, or -1 with no catalogue saved. */
int pic_gcat_each(void (*fn)(int norad, const char *note, void *ud), void *ud);
int pic_gcat_each_file(const char *path,
                       void (*fn)(int norad, const char *note, void *ud), void *ud);

#endif
