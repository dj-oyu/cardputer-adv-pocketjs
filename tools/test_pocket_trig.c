// Host test of components/quickjs-ng/quickjs-ng/pocket_trig.h against libm
// (docs/vm/qjs-observe.md sec.18).
//   gcc -std=gnu11 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined
//       -Icomponents/quickjs-ng/quickjs-ng tools/test_pocket_trig.c -lm -o /tmp/t && /tmp/t
#include "pocket_trig.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    // what the specification fixes
    CHECK(isnan(pocket_sin(NAN)) && isnan(pocket_cos(NAN)) && isnan(pocket_tan(NAN)), "NaN");
    CHECK(isnan(pocket_sin(INFINITY)) && isnan(pocket_sin(-INFINITY)), "sin(+-inf)");
    CHECK(isnan(pocket_cos(INFINITY)) && isnan(pocket_tan(-INFINITY)), "cos/tan(inf)");
    CHECK(pocket_sin(0.0) == 0 && !signbit(pocket_sin(0.0)), "sin(+0)");
    CHECK(pocket_sin(-0.0) == 0 && signbit(pocket_sin(-0.0)), "sin(-0)");
    CHECK(signbit(pocket_tan(-0.0)), "tan(-0)");
    CHECK(pocket_cos(0.0) == 1 && pocket_cos(-0.0) == 1, "cos(0)");
    CHECK(pocket_sin(1e-12) == 1e-12, "tiny");
    // accuracy, over small and large arguments
    double worst_s = 0, worst_c = 0, at_s = 0, at_c = 0;
    srand(1);
    for (int i = 0; i < 2000000; i++) {
        const double r = (double)rand() / RAND_MAX;
        const double x = i < 1000000 ? (r * 2 - 1) * 40 : (r * 2 - 1) * 1e6;
        const double es = fabs(pocket_sin(x) - sin(x)), ec = fabs(pocket_cos(x) - cos(x));
        if (es > worst_s) { worst_s = es; at_s = x; }
        if (ec > worst_c) { worst_c = ec; at_c = x; }
        if (pocket_sin(-x) != -pocket_sin(x) || pocket_cos(-x) != pocket_cos(x) || pocket_tan(-x) != -pocket_tan(x)) {
            fails++;
            if (fails < 5) printf("FAIL symmetry at %.17g\n", x);
        }
    }
    // quarter points: exact table entries
    CHECK(pocket_sin(M_PI / 2) > 0.99999 && fabs(pocket_cos(M_PI)) > 0.99999, "quarters");
    // tan where cos is small stays finite and has the right sign near pi/2
    CHECK(isfinite(pocket_tan(M_PI / 2)) && pocket_tan(1.5) > 14 && pocket_tan(1.5) < 14.2, "tan(1.5)=%g", pocket_tan(1.5));
    // a huge argument still lands in [-1, 1]
    CHECK(fabs(pocket_sin(1e300)) <= 1 && fabs(pocket_cos(-3e15)) <= 1, "huge");
    printf("max |error| sin %.3g at %.6g, cos %.3g at %.6g\n", worst_s, at_s, worst_c, at_c);
    CHECK(worst_s < 2e-5 && worst_c < 2e-5, "error bound");
    printf("POCKET_TRIG %s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
