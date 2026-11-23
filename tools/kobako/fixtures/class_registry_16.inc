
static JSRuntime **class_owners[16];
static unsigned class_owner_count;

bool kobako_api_class_ready(JSRuntime *rt, JSRuntime **owner, JSClassID *id) {
    if(*owner==rt) return true;      // already registered in this runtime
    *id=0;                           // and this one's counter decides the id
    *owner=rt;
    for(unsigned i=0;i<class_owner_count;i++) if(class_owners[i]==owner) return false;
    assert(class_owner_count<sizeof(class_owners)/sizeof(class_owners[0]));
    class_owners[class_owner_count++]=owner;
    return false;
}

void kobako_api_reset(void) {
    // Registrations belong to the session that made them. Every surface
    // registers from its install function, and a session installs only what it
    // was given: an overlay gets no radio or buses, and kobako.pet goes only to
    // the apps whose manifest names pet.companion. A table that outlived the
    // session would answer supported=true for whatever the previous session
    // installed -- the wrong answer, and exactly the one section 2 exists to
    // prevent.
    override_count=0;
    sub_live=0;
    for(unsigned i=0;i<class_owner_count;i++) *class_owners[i]=NULL;
    for(unsigned i=0;i<KOBAKO_MAX_PROMISES;i++) {
        kobako_promise_t *p=&promises[i];
        if(!atomic_load(&p->request)) continue;
        // The realm is going away, so there is nobody left to settle to. The
        // work is asked to stop and then left alone: its completion carries a
        // request number this session will never read again, and the driver
        // holds no JS value to free.
        if(p->ops && p->ops->stop) p->ops->stop(p->user,KOBAKO_ERR_CLOSED);
        promise_release(p);
    }
}
