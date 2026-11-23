// Host test of components/quickjs-ng/quickjs-ng/lean_trig.h against libm
// (docs/vm/qjs-observe.md sec.18).
//   gcc -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined
//       -Icomponents/quickjs-ng/quickjs-ng tools/test_lean_trig.c -lm -o /tmp/t && /tmp/t
#include "lean_trig.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    // what the specification fixes
    CHECK(isnan(lean_sin(NAN)) && isnan(lean_cos(NAN)) && isnan(lean_tan(NAN)), "NaN");
    CHECK(isnan(lean_sin(INFINITY)) && isnan(lean_sin(-INFINITY)), "sin(+-inf)");
    CHECK(isnan(lean_cos(INFINITY)) && isnan(lean_tan(-INFINITY)), "cos/tan(inf)");
    CHECK(lean_sin(0.0) == 0 && !signbit(lean_sin(0.0)), "sin(+0)");
    CHECK(lean_sin(-0.0) == 0 && signbit(lean_sin(-0.0)), "sin(-0)");
    CHECK(signbit(lean_tan(-0.0)), "tan(-0)");
    CHECK(lean_cos(0.0) == 1 && lean_cos(-0.0) == 1, "cos(0)");
    CHECK(lean_sin(1e-12) == 1e-12, "tiny");
    // accuracy, over small and large arguments
    double worst_s = 0, worst_c = 0, at_s = 0, at_c = 0;
    srand(1);
    for (int i = 0; i < 2000000; i++) {
        const double r = (double)rand() / RAND_MAX;
        const double x = i < 1000000 ? (r * 2 - 1) * 40 : (r * 2 - 1) * 1e6;
        const double es = fabs(lean_sin(x) - sin(x)), ec = fabs(lean_cos(x) - cos(x));
        if (es > worst_s) { worst_s = es; at_s = x; }
        if (ec > worst_c) { worst_c = ec; at_c = x; }
        if (lean_sin(-x) != -lean_sin(x) || lean_cos(-x) != lean_cos(x) || lean_tan(-x) != -lean_tan(x)) {
            fails++;
            if (fails < 5) printf("FAIL symmetry at %.17g\n", x);
        }
    }
    // quarter points: exact table entries
    CHECK(lean_sin(M_PI / 2) > 0.99999 && fabs(lean_cos(M_PI)) > 0.99999, "quarters");
    // tan where cos is small stays finite and has the right sign near pi/2
    CHECK(isfinite(lean_tan(M_PI / 2)) && lean_tan(1.5) > 14 && lean_tan(1.5) < 14.2, "tan(1.5)=%g", lean_tan(1.5));
    // a huge argument still lands in [-1, 1]
    CHECK(fabs(lean_sin(1e300)) <= 1 && fabs(lean_cos(-3e15)) <= 1, "huge");
    printf("max |error| sin %.3g at %.6g, cos %.3g at %.6g\n", worst_s, at_s, worst_c, at_c);
    CHECK(worst_s < 2e-5 && worst_c < 2e-5, "error bound");
    printf("KOBAKO_TRIG %s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
