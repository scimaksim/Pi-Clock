/* GCAT end-of-life notes: made-up catalogue rows in the real layout */
#include "check.h"
#include "gcat.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int seen[8], nseen;
static char notes[8][40];
static void got(int id, const char *note, void *ud)
{
    (void)ud;
    if (nseen < 8) { seen[nseen] = id; snprintf(notes[nseen], 40, "%s", note); nseen++; }
}

#define HDR "#JCAT\tPiece\tName\tLDate\tTLast\tTOp\tTDate\tTF\tProgram\n"
#define ROW(j, top, td, tf) j "\t1999-001A\tTestsat\t1999 Jan  1\t2001 Jan\t" top "\t" td "\t" tf "\tX\n"

int main(void)
{
    pic_gcat_cols_t c;
    char note[40];
    int id = 0;
    const char *path = "/tmp/test_gcat.tsv";
    FILE *f;

    CHECK(pic_gcat_header(HDR, &c) == 0 && c.top == 5 && c.tdate == 6 && c.tf == 7, "header columns");
    CHECK(pic_gcat_header("#JCAT\tPiece\tName\n", &c) == -1, "header without the columns refused");
    CHECK(pic_gcat_header("<html>", &c) == -1, "not a catalogue");
    pic_gcat_header(HDR, &c);

    CHECK(pic_gcat_row(ROW("S00011    ", "*", "*", "-"), &c, &id, note, sizeof(note)) == 0, "working: no note");
    CHECK(pic_gcat_row(ROW("S00012    ", "2012 Apr  8", "2012 Apr  8", "-"), &c, &id, note, sizeof(note)) == 1
          && id == 12 && !strcmp(note, "defunct Apr 2012"), "day date -> month and year (%s)", note);
    CHECK(pic_gcat_row(ROW("S00013    ", "2025 Aug 13", "2025 Aug 13 1655", "-"), &c, &id, note, sizeof(note)) == 1
          && !strcmp(note, "defunct Aug 2025"), "time of day ignored (%s)", note);
    CHECK(pic_gcat_row(ROW("S00014    ", "2011?", "2012?", "-"), &c, &id, note, sizeof(note)) == 1
          && !strcmp(note, "defunct about 2012"), "uncertain year (%s)", note);
    CHECK(pic_gcat_row(ROW("S00015    ", "2020", "2020", "X"), &c, &id, note, sizeof(note)) == 1
          && !strcmp(note, "defunct about 2020"), "X flag = a guess (%s)", note);
    CHECK(pic_gcat_row(ROW("S00016    ", "2019 Q2", "2019 Q2", "-"), &c, &id, note, sizeof(note)) == 1
          && !strcmp(note, "defunct 2019"), "quarter -> year (%s)", note);
    CHECK(pic_gcat_row(ROW("S00017    ", "1990s?", "1990s?", "-"), &c, &id, note, sizeof(note)) == 1
          && !strcmp(note, "defunct about 1990s"), "decade (%s)", note);
    CHECK(pic_gcat_row(ROW("S00018    ", "1984 Mar", "*", "Z"), &c, &id, note, sizeof(note)) == 1
          && !strcmp(note, "retired Mar 1984"), "out of service, still heard (%s)", note);
    CHECK(pic_gcat_row(ROW("S00019    ", "2006?", "*", "U"), &c, &id, note, sizeof(note)) == 0, "U: thought working");
    CHECK(pic_gcat_row(ROW("S00029    ", "1981?", "*", "-"), &c, &id, note, sizeof(note)) == 0,
          "end of operations alone is not enough");
    CHECK(pic_gcat_row(ROW("S00020    ", "-", "-", "-"), &c, &id, note, sizeof(note)) == 0, "no date: no note");
    CHECK(pic_gcat_row(ROW("S00021    ", "2999 Jan", "2999 Jan", "-"), &c, &id, note, sizeof(note)) == 0, "future year refused");
    CHECK(pic_gcat_row(ROW("S101353   ", "*", "2026 Feb?", "-"), &c, &id, note, sizeof(note)) == 1
          && id == 101353, "six-digit number (%d)", id);
    CHECK(pic_gcat_row(ROW("S00022A   ", "2001", "2001", "-"), &c, &id, note, sizeof(note)) == 0, "lettered id skipped");
    CHECK(pic_gcat_row(ROW("A00023    ", "2001", "2001", "-"), &c, &id, note, sizeof(note)) == 0, "not an S entry");
    CHECK(pic_gcat_row("S00024    \t1999-001A\n", &c, &id, note, sizeof(note)) == 0, "short row");

    f = fopen(path, "w");
    CHECK(f != NULL, "temp file");
    if (f) {
        int k;
        fputs(HDR "# Updated 2026 Jan  1\n", f);
        fputs(ROW("S00030    ", "*", "*", "-"), f);
        fputs(ROW("S00031    ", "2003 May  2", "2003 May  2", "-"), f);
        fputs("S00032    \t1999-001A\tLongsat\t1999\t2001\t2004 Jun\t2004 Jun\t-\t", f);
        for (k = 0; k < 5000; k++) fputc('x', f);      /* over-long line */
        fputc('\n', f);
        fputs(ROW("S00033    ", "2005 Jul", "*", "Z"), f);
        fclose(f);
        CHECK(pic_gcat_each_file(path, got, NULL) == 4, "four rows read");
        CHECK(nseen == 3 && seen[0] == 31 && seen[1] == 32 && seen[2] == 33, "three notes (%d)", nseen);
        CHECK(!strcmp(notes[1], "defunct Jun 2004") && !strcmp(notes[2], "retired Jul 2005"), "notes right");
        unlink(path);
    }
    CHECK(pic_gcat_each_file("/tmp/no-such-gcat", got, NULL) == -1, "missing file");
    return CHECK_DONE("test_gcat");
}
