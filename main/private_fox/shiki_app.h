#ifndef FOX_SHIKI_APP_H
#define FOX_SHIKI_APP_H
/* Asset-specific native app, only embedded in the explicit private profile. */
static const char fox_shiki_app[]=
 "const c=new Float64Array([0,0,0,0,0,0,1,.5,0,0,0]);"
 "let n=0;pocket.app.start({});pocket.app.onFrame(()=>{"
 "if(privateModelBackend.accepted()!==privateModelBackend.presented())return;"
 "c[0]=((n%120)<12)?1:0;c[9]=((n%160)-80)/80;"
 "privateModelBackend.commit(c);n++;});";
#endif
