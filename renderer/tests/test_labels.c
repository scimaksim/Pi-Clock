/* Shared label placement: owners avoid each other's boxes and their own */
#include "check.h"
#include "labels.h"
#include "config.h"

int main(void)
{
    int a, b;
    double x, y, x2, y2, box[4] = { 900, 500, 1000, 520 };

    pic_labels_reset();
    a = pic_labels_owner("a");
    b = pic_labels_owner("b");
    CHECK(a != b && pic_labels_owner("a") == a, "owner ids stable");
    pic_labels_begin(a);
    pic_labels_begin(b);

    pic_labels_claim(a, box);
    CHECK(pic_labels_free(a, box) == 1, "own box does not block pic_labels_free");
    CHECK(pic_labels_clear(a, box) == 0, "own box blocks pic_labels_clear");
    CHECK(pic_labels_free(b, box) == 0, "another owner's box blocks");

    /* b wants a label right of (895, 510): that is a's box, so it goes elsewhere */
    CHECK(pic_labels_place(b, 895, 510, 5, 80, 20, 1920, 1080, &x, &y) == 1, "placed");
    {
        double got[4] = { x, y, x + 80, y + 20 };
        CHECK(got[2] < box[0] || got[0] > box[2] || got[3] < box[1] || got[1] > box[3],
              "placed clear of a (%.0f,%.0f)", x, y);
    }
    /* a second b label at the same point avoids the first too */
    CHECK(pic_labels_place(b, 895, 510, 5, 80, 20, 1920, 1080, &x2, &y2) == 1, "second placed");
    CHECK(x2 != x || y2 != y, "second label not on the first");

    /* a redraws with nothing: its box no longer blocks */
    pic_labels_begin(a);
    CHECK(pic_labels_free(b, box) == 1, "boxes forgotten on redraw");

    /* boxes from another view (zoom/pan changed) are ignored */
    pic_labels_claim(a, box);
    pic_config.viewport_gen++;
    CHECK(pic_labels_free(b, box) == 1, "old view's boxes ignored");
    pic_labels_begin(a);
    pic_labels_claim(a, box);
    CHECK(pic_labels_free(b, box) == 0, "new view's boxes count");

    /* not on the clock in the top-left corner */
    CHECK(pic_labels_place(b, 95, 60, 5, 80, 20, 1920, 1080, &x, &y) == 0, "clock area refused");

    /* no room at all (tiny screen): returns 0 and the right-hand spot */
    CHECK(pic_labels_place(b, 10, 10, 5, 80, 20, 60, 30, &x, &y) == 0 && x == 15, "no room reported");
    return CHECK_DONE("test_labels");
}
