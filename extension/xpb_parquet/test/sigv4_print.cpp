#include "xpb_s3_reader.h"
#include <cstdio>
#include <cstring>
int main(int argc, char **argv)
{
    /* what method uri host range amzdate datestamp region access secret */
    if (argc < 7) { fprintf(stderr, "usage: what method uri host range amzdate [datestamp region access secret]\n"); return 2; }
    const char *what = argv[1];
    const char *rng = argv[5][0] ? argv[5] : nullptr;
    if (strcmp(what, "canon") == 0)
        printf("%s", xpb::s3_test_canonical_request(argv[2], argv[3], argv[4], rng, argv[6]).c_str());
    else if (argc == 11)
        printf("%s\n", xpb::s3_test_authorization(argv[2], argv[3], argv[4], rng,
                                                  argv[6], argv[7], argv[8], argv[9], argv[10]).c_str());
    else { fprintf(stderr, "auth needs 10 args\n"); return 2; }
    return 0;
}
