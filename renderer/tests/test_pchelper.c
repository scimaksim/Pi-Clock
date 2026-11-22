/* The helper publishes a daytime bird summary without a raster. */
#include "check.h"
#include "../layers_pchelper.c"

int main(void)
{
    pch_t d = { .mutex = PTHREAD_MUTEX_INITIALIZER };
    char files[PCH_MAXF][48];
    const char day[] = "{\"generated\":1000,\"expires\":4600,\"width\":0,\"height\":0,"
                       "\"frames\":[],\"note\":\"Last night: peak migration\"}";
    const char bad[] = "{\"width\":0,\"height\":0,\"frames\":[{\"file\":\"a.u8\"}]}";
    const char missing[] = "{\"width\":0,\"height\":0}";
    const char night[] = "{\"width\":2,\"height\":2,\"frames\":[{\"file\":\"a.u8\"}]}";
    CHECK(parse_manifest(day, strlen(day), &d, files) == 0, "daytime manifest accepted");
    d.last_ok = 1000;
    CHECK(usable(&d, 1200) && strstr(d.note, "peak migration"), "fresh note available");
    CHECK(pch_idle(1200, &d) && current_frame(&d, 1200) == -1, "no raster scheduled");
    CHECK(!usable(&d, 6000), "old summary expires");
    CHECK(parse_manifest(bad, strlen(bad), &d, files) < 0, "framed product needs dimensions");
    CHECK(parse_manifest(missing, strlen(missing), &d, files) < 0, "missing frames rejected");
    CHECK(parse_manifest(night, strlen(night), &d, files) == 0 && d.nf == 1, "night frame accepted");
    d.nf = 1;
    d.surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    release_raster(&d);
    CHECK(d.surf != NULL, "night raster kept while frames exist");
    d.nf = 0;
    release_raster(&d);
    CHECK(d.surf == NULL && d.surf_frame == -1, "raster freed for a frameless product");
    pthread_mutex_destroy(&d.mutex);
    return CHECK_DONE("test_pchelper");
}
