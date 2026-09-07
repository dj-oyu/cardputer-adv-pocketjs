// Host check, no board involved:
//   gcc -std=c11 -O1 -Wall -Wextra -fsanitize=address,undefined -I main/text
//       tools/test_app_registry.c main/pocket/app_registry.c -o /tmp/test-registry
//   /tmp/test-registry
//
// app_registry.c is the part of section 3 that decides things: whether an app
// may start against this firmware's API version and capabilities, and what it
// may do with the works library. None of those decisions need a board, and all
// of them are the kind that is wrong in one corner rather than everywhere — a
// ">=" read as ">", a picked work still refused, a title carrying a newline
// into the picker's row. They are settled here.
#include "../main/pocket/app_registry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(cond) do { checks++; if(!(cond)) { \
    printf("FAIL %s:%d  %s\n",__FILE__,__LINE__,#cond); failures++; } } while(0)
static unsigned failures;

// ------------------------------------------------------------ version ranges

static void test_ranges(void) {
    CHECK(app_registry_api_ok(">=0.1.0 <0.2.0","0.1.0"));
    CHECK(app_registry_api_ok(">=0.1.0 <0.2.0","0.1.9"));
    CHECK(app_registry_api_ok(">=0.1.0 <0.2.0","0.1.99"));
    CHECK(!app_registry_api_ok(">=0.1.0 <0.2.0","0.2.0"));
    CHECK(!app_registry_api_ok(">=0.1.0 <0.2.0","0.0.9"));
    CHECK(!app_registry_api_ok(">=0.1.0 <0.2.0","1.0.0"));

    // ">=" is not ">".
    CHECK(app_registry_api_ok(">=0.1.0","0.1.0"));
    CHECK(!app_registry_api_ok(">0.1.0","0.1.0"));
    CHECK(app_registry_api_ok("<=0.1.0","0.1.0"));
    CHECK(!app_registry_api_ok("<0.1.0","0.1.0"));
    CHECK(app_registry_api_ok("=0.1.0","0.1.0"));
    CHECK(app_registry_api_ok("==0.1.0","0.1.0"));

    // Fields compare as numbers, not as text: 0.10.0 is above 0.9.0.
    CHECK(app_registry_api_ok(">=0.9.0","0.10.0"));
    CHECK(!app_registry_api_ok(">=0.10.0","0.9.0"));

    // Empty admits; unreadable refuses. A range nobody can parse must not be
    // read as "no constraint" — that is the failure that ships an app onto a
    // firmware it was never written for.
    CHECK(app_registry_api_ok(NULL,"0.1.0"));
    CHECK(app_registry_api_ok("","0.1.0"));
    CHECK(!app_registry_api_ok("0.1.0","0.1.0"));        // no operator
    CHECK(!app_registry_api_ok(">=0.1","0.1.0"));        // two fields
    CHECK(!app_registry_api_ok(">=0.1.0.0","0.1.0"));    // four
    CHECK(!app_registry_api_ok(">=x.y.z","0.1.0"));
    CHECK(!app_registry_api_ok("~>0.1.0","0.1.0"));      // not this grammar
    CHECK(!app_registry_api_ok(">=0.1.0","0.1"));        // the host's own
    CHECK(!app_registry_api_ok(">=0.1.0",NULL));

    // Separators: spaces, tabs and commas all end a clause.
    CHECK(app_registry_api_ok(">=0.1.0,<0.2.0","0.1.5"));
    CHECK(app_registry_api_ok("  >=0.1.0\t<0.2.0  ","0.1.5"));
}

// --------------------------------------------------------------- the table

static const char *missing;   // the one capability the fake host lacks

static bool supported(const char *name, void *user) {
    (void)user;
    return !missing || strcmp(name,missing)!=0;
}

static void test_admission(void) {
    char reason[48];
    const app_manifest_t *pet=app_registry_find("local.pet");
    const app_manifest_t *cal=app_registry_find("local.imucal");
    const app_manifest_t *play=app_registry_find("local.playground");
    CHECK(pet && cal && play);
    CHECK(app_registry_find("local.nothing")==NULL);
    CHECK(app_registry_find(NULL)==NULL);
    CHECK(app_registry_find("")==NULL);

    missing=NULL;
    CHECK(app_registry_admit(pet,"0.1.0",supported,NULL,reason,sizeof reason));
    CHECK(reason[0]==0);

    // A required capability the firmware does not implement stops the app
    // before it runs, and says which one.
    missing="sensors.imu";
    CHECK(!app_registry_admit(cal,"0.1.0",supported,NULL,reason,sizeof reason));
    CHECK(!strcmp(reason,"NEEDS sensors.imu"));

    // The pet's IMU is optional, so the same absent capability does not stop it.
    CHECK(app_registry_admit(pet,"0.1.0",supported,NULL,reason,sizeof reason));

    // A legacy app is not held to an API range; a pocket-app is.
    missing=NULL;
    CHECK(app_registry_admit(pet,"9.9.9",supported,NULL,reason,sizeof reason));
    CHECK(!app_registry_admit(play,"0.2.0",supported,NULL,reason,sizeof reason));
    CHECK(!strncmp(reason,"NEEDS API",9));
    CHECK(app_registry_admit(play,"0.1.0",supported,NULL,reason,sizeof reason));

    // No manifest is not a refusal: a host that has not registered anything
    // still starts what it is asked to.
    CHECK(app_registry_admit(NULL,"0.1.0",supported,NULL,reason,sizeof reason));

    // Selection falls back rather than inheriting the last session's identity.
    app_registry_select("local.pet");
    CHECK(!strcmp(app_registry_current()->id,"local.pet"));
    app_registry_select("local.nothing");
    CHECK(!strcmp(app_registry_current()->id,APP_ID_DEFAULT));
    app_registry_select(NULL);
    CHECK(!strcmp(app_registry_current()->id,APP_ID_DEFAULT));

    // Every row's required names must be spelled the way the host spells them.
    // The fake host answers true for everything here, so this only catches a
    // manifest whose list is malformed.
    for(const char *const *n=pet->required;*n;n++) CHECK((*n)[0]!=0);
}

// What the host reads before it sizes the guest heap: an app that will want a
// capability whose acquisition needs room has to be knowable while there is
// still room. Required and optional both answer yes.
static void test_wants(void) {
    const app_manifest_t *companion=app_registry_find("local.companion");
    const app_manifest_t *pet=app_registry_find("local.pet");
    CHECK(companion && pet);

    CHECK(app_registry_wants(companion,"net.http"));    // optional
    CHECK(app_registry_wants(companion,"storage.kv"));  // required
    CHECK(!app_registry_wants(companion,"net.wifi"));   // named neither list
    CHECK(!app_registry_wants(pet,"net.http"));
    CHECK(app_registry_wants(pet,"sensors.imu"));

    CHECK(!app_registry_wants(NULL,"net.http"));
    CHECK(!app_registry_wants(companion,NULL));
    CHECK(!app_registry_wants(companion,""));
}

// ---------------------------------------------------------------- permissions

static void test_permissions(void) {
    // NONE refuses everything, including creating its own work.
    for(int op=APP_WORK_PICK;op<=APP_WORK_RUN;op++) {
        CHECK(!app_registry_may_work(APP_WORKS_NONE,(app_work_op_t)op,true,true));
        CHECK(!app_registry_may_work(APP_WORKS_NONE,(app_work_op_t)op,false,false));
    }

    // SELF creates and reaches its own, and cannot open the picker — nor reach
    // a work by claiming it was picked, since a level that cannot pick can
    // never be handed a picked reference.
    CHECK(app_registry_may_work(APP_WORKS_SELF,APP_WORK_CREATE,false,false));
    CHECK(app_registry_may_work(APP_WORKS_SELF,APP_WORK_READ,true,false));
    CHECK(app_registry_may_work(APP_WORKS_SELF,APP_WORK_SAVE,true,false));
    CHECK(app_registry_may_work(APP_WORKS_SELF,APP_WORK_RUN,true,false));
    CHECK(!app_registry_may_work(APP_WORKS_SELF,APP_WORK_PICK,false,false));
    CHECK(!app_registry_may_work(APP_WORKS_SELF,APP_WORK_READ,false,false));
    CHECK(!app_registry_may_work(APP_WORKS_SELF,APP_WORK_READ,false,true));
    CHECK(!app_registry_may_work(APP_WORKS_SELF,APP_WORK_SAVE,false,true));

    // PICK adds the picker, and what the person chose there.
    CHECK(app_registry_may_work(APP_WORKS_PICK,APP_WORK_PICK,false,false));
    CHECK(app_registry_may_work(APP_WORKS_PICK,APP_WORK_READ,false,true));
    CHECK(app_registry_may_work(APP_WORKS_PICK,APP_WORK_SAVE,false,true));
    CHECK(app_registry_may_work(APP_WORKS_PICK,APP_WORK_RUN,false,true));
    // A reference that was never picked and is not its own stays refused, at
    // every level. This is the one that matters: it is what stops an app from
    // reaching a work by holding a reference it was never given.
    CHECK(!app_registry_may_work(APP_WORKS_PICK,APP_WORK_READ,false,false));
    CHECK(!app_registry_may_work(APP_WORKS_PICK,APP_WORK_SAVE,false,false));
    CHECK(!app_registry_may_work(APP_WORKS_PICK,APP_WORK_RUN,false,false));

    // Section 7's other half of a save onto someone else's work.
    CHECK(app_registry_save_needs_revision(false));
    CHECK(!app_registry_save_needs_revision(true));
}

// -------------------------------------------------------------------- titles

static void test_titles(void) {
    char out[32];
    CHECK(app_registry_clean_title("notes",5,out,sizeof out)==5);
    CHECK(!strcmp(out,"notes"));

    // Japanese is a title like any other; the picker draws it with jpfont.
    const char *jp="\xe3\x83\xa1\xe3\x83\xa2";   // メモ
    CHECK(app_registry_clean_title(jp,6,out,sizeof out)==6);

    CHECK(app_registry_clean_title("",0,out,sizeof out)==0);
    CHECK(app_registry_clean_title("a\nb",3,out,sizeof out)==0);   // control
    CHECK(app_registry_clean_title("a\tb",3,out,sizeof out)==0);
    CHECK(app_registry_clean_title("\xff\xfe",2,out,sizeof out)==0);  // not UTF-8
    CHECK(app_registry_clean_title("\xe3\x83",2,out,sizeof out)==0);  // truncated
    CHECK(app_registry_clean_title("\xed\xa0\x80",3,out,sizeof out)==0);  // surrogate
    CHECK(app_registry_clean_title("\xc0\xaf",2,out,sizeof out)==0);  // overlong

    // One byte too long is a refusal, not a truncation. out_size counts the
    // NUL, so three bytes are the most a four-byte buffer takes.
    char small[4];
    CHECK(app_registry_clean_title("abcd",4,small,sizeof small)==0);
    CHECK(small[0]==0);
    CHECK(app_registry_clean_title("abc",3,small,sizeof small)==3);
    CHECK(small[3]==0);
}

int main(void) {
    test_ranges();
    test_admission();
    test_wants();
    test_permissions();
    test_titles();
    printf("%s: %u checks, %u failures\n",failures?"FAIL":"ok",checks,failures);
    return failures?1:0;
}
