// The safety valves of docs/common-api.md 3.1, on the host.
//
// These are settled here rather than on the board because of what they
// protect against: the crash flag exists so that an overlay which faults
// during startup cannot boot-loop the device, and a test you can only run by
// flashing the thing that boot-loops is not a test. Everything below is a pure
// function of its arguments; ui/overlay.c holds the NVS and the guest and this
// file holds the decisions.
//
//   gcc -O2 -Wall -Wextra -Werror -I main/ui tools/test_overlay.c
//   main/ui/overlay_core.c -o /tmp/t && /tmp/t   (from the repository root:
//   the shape checks below read main/ui/shell.c and main/main.c by path)

#include "overlay_core.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond,...) do { if(!(cond)) { \
    printf("FAIL %s:%d ",__FILE__,__LINE__); printf(__VA_ARGS__); printf("\n"); \
    failures++; } } while(0)

// ---------------------------------------------------------------- the region

static void region_confinement(void) {
    const overlay_region_t box={.x=140,.y=46,.w=96,.h=22};

    CHECK(overlay_region_holds(&box,0,0,96,22),"the whole box is inside itself");
    CHECK(overlay_region_holds(&box,4,4,29,7),"a five character clock face fits");

    // One pixel over, on each edge. 3.1 refuses rather than clips, so each of
    // these is an exception the author sees and not a letter quietly missing.
    CHECK(!overlay_region_holds(&box,0,0,97,22),"one column too wide");
    CHECK(!overlay_region_holds(&box,0,0,96,23),"one row too tall");
    CHECK(!overlay_region_holds(&box,1,0,96,22),"shifted one right");
    CHECK(!overlay_region_holds(&box,0,1,96,22),"shifted one down");
    CHECK(!overlay_region_holds(&box,-1,0,10,10),"negative x");
    CHECK(!overlay_region_holds(&box,0,-1,10,10),"negative y");

    // Empty and negative extents. A width of zero draws nothing, and an
    // overlay that computed one has a bug that silence would hide.
    CHECK(!overlay_region_holds(&box,0,0,0,10),"zero width");
    CHECK(!overlay_region_holds(&box,0,0,10,0),"zero height");
    CHECK(!overlay_region_holds(&box,0,0,-4,10),"negative width");

    // The overflow that would otherwise wrap into the box.
    CHECK(!overlay_region_holds(&box,2000000000,0,2000000000,4),"x+w overflows int");
    CHECK(!overlay_region_holds(&box,0,2000000000,4,2000000000),"y+h overflows int");

    // The region's screen position is not addressable: coordinates are
    // region-local, so the shell's own rows cannot be named even by accident.
    // 140 is where the box sits on the LCD; as a local x it is off the end.
    CHECK(!overlay_region_holds(&box,140,46,10,7),"screen coordinates are not local ones");

    CHECK(!overlay_region_holds(NULL,0,0,1,1),"no region means nowhere to draw");
}

// ------------------------------------------------------------- the boot valve

static void boot_valve(void) {
    // The whole table. The one row that matters is the last.
    CHECK(overlay_boot_state(false,false)==OVERLAY_OFF,"not armed, clean");
    CHECK(overlay_boot_state(false,true)==OVERLAY_OFF,"not armed, flag set");
    CHECK(overlay_boot_state(true,false)==OVERLAY_STARTING,"armed and clean starts");
    CHECK(overlay_boot_state(true,true)==OVERLAY_BLOCKED,
          "armed with the flag still set does NOT auto-start");

    // The sequence that bricks a device without this valve: arm, start, crash
    // before the flag is cleared, boot. Three boots in a row must all refuse,
    // because nothing about a reset clears the flag.
    bool armed=true, flag=true;
    for(int boot=0;boot<3;boot++)
        CHECK(overlay_boot_state(armed,flag)==OVERLAY_BLOCKED,"boot %d still blocked",boot);

    // Only a human turning it off clears it, and turning it back on then runs.
    armed=false; flag=false;                       // overlay_armed_set(0)
    CHECK(overlay_boot_state(armed,flag)==OVERLAY_OFF,"off after the human act");
    armed=true;
    CHECK(overlay_boot_state(armed,flag)==OVERLAY_STARTING,"and on again may start");
}

// ----------------------------------------------------------- the frame budget

static overlay_budget_t fresh(void) {
    overlay_budget_t b={.budget_us=8000,.over_limit=60,.healthy_us=5000000};
    overlay_budget_start(&b,0);
    return b;
}

static void frame_budget(void) {
    overlay_budget_t b=fresh();

    // A single slow turn is not a fault: a garbage collection is one turn.
    for(int i=0;i<1000;i++) {
        CHECK(!overlay_budget_turn(&b,i%100==0?40000:1000),"turn %d must not stop",i);
    }
    CHECK(b.worst_us==40000,"the worst turn is remembered: %u",(unsigned)b.worst_us);

    // A run of them is. Exactly at the limit, not before it.
    b=fresh();
    for(int i=0;i<59;i++) CHECK(!overlay_budget_turn(&b,9000),"stopped early at %d",i);
    CHECK(overlay_budget_turn(&b,9000),"the 60th consecutive over-budget turn stops it");

    // One good turn resets the run, which is what makes "repeatedly" mean
    // repeatedly and not "sixty times since boot".
    b=fresh();
    for(int i=0;i<59;i++) overlay_budget_turn(&b,9000);
    CHECK(!overlay_budget_turn(&b,100),"a healthy turn clears the run");
    for(int i=0;i<59;i++) CHECK(!overlay_budget_turn(&b,9000),"and the count starts over");

    // Exactly at the budget is inside it.
    b=fresh();
    for(int i=0;i<200;i++) CHECK(!overlay_budget_turn(&b,8000),"8000us is within 8000us");

    // A limit of zero disables the stop rather than stopping immediately,
    // which is the reading a caller would expect of "no limit".
    b=fresh(); b.over_limit=0;
    for(int i=0;i<200;i++) CHECK(!overlay_budget_turn(&b,1000000),"no limit, no stop");
}

static void healthy_period(void) {
    overlay_budget_t b=fresh();
    overlay_budget_turn(&b,1000);
    CHECK(!overlay_budget_healthy(&b,4999999),"not healthy before the period is up");
    CHECK(overlay_budget_healthy(&b,5000000),"healthy at the period");

    // The case the second half of the rule exists for: an overlay that has
    // been over budget from its first turn must NOT have its crash flag
    // cleared at the five second mark just because it is still alive.
    b=fresh();
    for(int i=0;i<10;i++) overlay_budget_turn(&b,9000);
    CHECK(!overlay_budget_healthy(&b,9000000),"over budget right now is not healthy");
    overlay_budget_turn(&b,100);
    CHECK(overlay_budget_healthy(&b,9000000),"and healthy once it recovers");
}

// -------------------------------------- the home screen without the overlay
//
// 3.1's premise: if an overlay failure can take the shell down, the person
// cannot reach the screen that turns it off. The firmware side of that is
// painting order and one guard, and this is the check that keeps it true --
// it reads the sources rather than the binary, because what it is asserting
// is a shape and not a value.

static char *slurp(const char *path) {
    static char buffer[400000];
    FILE *f=fopen(path,"rb");
    if(!f) { printf("FAIL cannot open %s\n",path); failures++; return NULL; }
    size_t n=fread(buffer,1,sizeof(buffer)-1,f);
    fclose(f);
    buffer[n]='\0';
    return buffer;
}

// How many times `needle` occurs in `haystack`.
static int occurrences(const char *haystack, const char *needle) {
    int n=0;
    for(const char *p=haystack;(p=strstr(p,needle));p+=strlen(needle)) n++;
    return n;
}

static void home_survives_without_the_overlay(void) {
    char *shell=slurp("main/ui/shell.c");
    if(!shell) return;

    // The shell reaches the overlay exactly once, and it is the composite.
    // Anything else -- a state read that decides a layout, an early return, a
    // menu row whose text comes from the overlay -- would make the home
    // screen's drawing depend on a guest, which is the dependency 3.1 forbids.
    CHECK(occurrences(shell,"overlay_paint(")==1,
          "shell.c composites the overlay once and does nothing else with it");
    CHECK(occurrences(shell,"pocket_overlay_")==0,
          "the shell does not reach past ui/overlay.c into the JS surface");
    CHECK(occurrences(shell,"app_overlay_tick")==0 &&
          occurrences(shell,"app_start_overlay")==0,
          "the shell never runs guest code from inside a draw");

    // And the composite is under the shell's own labels. The order is the
    // whole guarantee that an overlay cannot cover the shell's UI, so it is
    // asserted rather than left to a reviewer's eye.
    const char *paint=strstr(shell,"overlay_paint(strip");
    const char *labels=strstr(shell,"paint_labels()");
    const char *error_text=strstr(shell,"\"APP ERROR\"");
    CHECK(paint && labels && paint<labels,"the overlay is painted before the menu");
    CHECK(paint && error_text && paint<error_text,
          "and before the app error line, which is how a failed app is reported");

    char *main_c=slurp("main/main.c");
    if(!main_c) return;
    // Every path that takes the display releases the overlay first. Three of
    // them: leaving the home screen, starting a foreground app, and the USB
    // diagnostics, which reach app_start_test() without going through
    // begin_run().
    CHECK(occurrences(main_c,"overlay_release()")==3,
          "every path that takes the guest releases the overlay first");
    const char *tick=strstr(main_c,"overlay_tick()");
    CHECK(tick && strstr(tick-200,"!running && screen==SCREEN_HOME"),
          "and a turn only runs while the home screen owns the display");
}

int main(void) {
    region_confinement();
    boot_valve();
    frame_budget();
    healthy_period();
    home_survives_without_the_overlay();
    if(failures) { printf("%d FAILURES\n",failures); return 1; }
    printf("OVERLAY_OK region, boot valve, frame budget, shell independence\n");
    return 0;
}
