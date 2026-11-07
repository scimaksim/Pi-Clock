/* Data-source status: hosts only (no credentials, paths or keys), JSON
 * written, streaks and recovery */
#include "check.h"
#include "datastatus.h"
#include <string.h>
#include <unistd.h>

int main(void)
{
    char buf[8192];
    FILE *f;
    size_t n;

    unlink(PIC_STATUS_PATH);
    pic_status_report_as("ebird", "https://user:pw@api.ebird.org:443/v2/data?key=SECRET42", 0, "HTTP 403");
    sleep(3);
    pic_status_report_as("ebird", "https://api.ebird.org/v2/x", 0, "timed out");
    sleep(3);
    pic_status_report_as("launches", "https://ll.thespacedevs.com/2.3.0/launches/", 1, NULL);
    f = fopen(PIC_STATUS_PATH, "r");
    CHECK(f != NULL, "status file written");
    if (!f) return CHECK_DONE("test_datastatus");
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "\"host\":\"api.ebird.org\"") != NULL, "host recorded: %s", buf);
    CHECK(strstr(buf, "SECRET42") == NULL && strstr(buf, "user:pw") == NULL &&
          strstr(buf, "/v2/") == NULL, "no URL parts or secrets in the file");
    CHECK(strstr(buf, "\"streak\":2") != NULL, "two failures in a row");
    CHECK(strstr(buf, "\"error\":\"timed out\"") != NULL, "last error kept");
    CHECK(strstr(buf, "\"users\":[\"launches\"]") != NULL, "user recorded");
    sleep(3);
    pic_status_report_as("ebird", "https://api.ebird.org/v2/x", 1, NULL);
    f = fopen(PIC_STATUS_PATH, "r");
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(strstr(buf, "\"streak\":0") != NULL && strstr(buf, "\"streak\":2") == NULL,
          "recovered source resets its streak");
    unlink(PIC_STATUS_PATH);
    return CHECK_DONE("test_datastatus");
}
