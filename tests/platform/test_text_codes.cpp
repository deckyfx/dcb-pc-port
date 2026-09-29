// Port-only text codes (src/platform/text_codes.hpp): *yN / *y-N.

#include "text_codes.hpp"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

int main() {
    int dy = 99;
    CHECK(text::parse_y_code("y1W", dy) == 2 && dy == 1);
    CHECK(text::parse_y_code("y0", dy) == 2 && dy == 0);
    CHECK(text::parse_y_code("y-2Tot.", dy) == 3 && dy == -2);
    // Not a y code, or cut short by the end of the string: nothing consumed, dy untouched.
    dy = 7;
    CHECK(text::parse_y_code("w-1Cds", dy) == 0 && dy == 7);
    CHECK(text::parse_y_code("y", dy) == 0 && dy == 7);
    CHECK(text::parse_y_code("y-", dy) == 0 && dy == 7);
    CHECK(text::parse_y_code("yes", dy) == 0 && dy == 7);
    CHECK(text::parse_y_code("", dy) == 0 && dy == 7);
    std::puts("text_codes: ok");
    return 0;
}
