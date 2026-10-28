/* A/B output oracle for primitive arithmetic and coercion order.
 * 126144 primitive result/tag/binary64 checks (including 10000 seeded bit
 * patterns in both operand orders), then 2166 coercion/throw combinations.
 * Force float-tagged inputs with __JS_NewFloat64 to exercise mixed tags.
 * compare.py requires byte-identical output from the pinned baseline.
 */
#include "quickjs.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static JSContext *ctx;
static JSValue ev(const char *s){JSValue v=JS_Eval(ctx,s,strlen(s),"numeric.js",JS_EVAL_TYPE_GLOBAL);assert(!JS_IsException(v));return v;}
static void output(JSValue v){
 if(JS_IsException(v)){JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);printf("throw %s\n",s?s:"?");if(s)JS_FreeCString(ctx,s);JS_FreeValue(ctx,e);return;}
 if(JS_IsNumber(v)){double d;assert(!JS_ToFloat64(ctx,&d,v));uint64_t b;memcpy(&b,&d,8);printf("number %d %016"PRIx64"\n",JS_VALUE_GET_NORM_TAG(v),b);}
 else {const char*s=JS_ToCString(ctx,v);assert(s);printf("other %s\n",s);JS_FreeCString(ctx,s);}
 JS_FreeValue(ctx,v);
}
static uint64_t rng=0x71325d59cc45ed13ULL;
static uint64_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
int main(void){
 JSRuntime*rt=JS_NewRuntime();assert(rt);ctx=JS_NewContext(rt);assert(ctx);
 const char*expr[]={"(a,b)=>a-b","(a,b)=>a*b","(a,b)=>a/b","(a,b)=>a%b","(a,b)=>a**b","(a,b)=>a+b"};
 JSValue fn[6];for(int k=0;k<6;k++)fn[k]=ev(expr[k]);
 const int iv[]={INT32_MIN,-123456789,-17,-1,0,1,2,17,123456789,INT32_MAX};
 const double dv[]={-INFINITY,-0x1.fffffffffffffp1023,-0x1p53,-2147483648.,-17.,-1.,-.5,-0x1p-1022,-0x0.0000000000001p-1022,-0.,0.,0x0.0000000000001p-1022,0x1p-1022,.5,1.,0x1.0000000000001p0,17.,2147483647.,0x1p53,0x1.fffffffffffffp1023,INFINITY,NAN};
 JSValue v[32];int n=0;for(unsigned i=0;i<sizeof(iv)/sizeof(*iv);i++)v[n++]=JS_NewInt32(ctx,iv[i]);for(unsigned i=0;i<sizeof(dv)/sizeof(*dv);i++)v[n++]=__JS_NewFloat64(dv[i]);
 for(int k=0;k<6;k++)for(int i=0;i<n;i++)for(int j=0;j<n;j++){JSValue a[]={v[i],v[j]};output(JS_Call(ctx,fn[k],JS_UNDEFINED,2,a));}
 for(int i=0;i<10000;i++){
  uint64_t bits=next();double d;memcpy(&d,&bits,8);JSValue a[]={__JS_NewFloat64(d),JS_NewInt32(ctx,(int32_t)next())};
  for(int k=0;k<6;k++){output(JS_Call(ctx,fn[k],JS_UNDEFINED,2,a));JSValue rev[]={a[1],a[0]};output(JS_Call(ctx,fn[k],JS_UNDEFINED,2,rev));}
 }
 output(ev("(()=>{const ops=[(a,b)=>a-b,(a,b)=>a*b,(a,b)=>a/b,(a,b)=>a%b,(a,b)=>a**b,(a,b)=>a+b];let trace=[],res=[];const f=[()=>true,()=>null,()=>undefined,()=>'',()=>'-0',()=>'.5',()=>[],()=>[3],()=>Symbol('s'),()=>1n,()=>-2n,()=>2n**70n,()=>({valueOf(){trace.push('v');return 1.5}}),()=>({valueOf(){trace.push('v');return 2n}}),()=>({valueOf(){trace.push('v');throw new Error('value')}}),()=>({valueOf(){trace.push('v');return {}},toString(){trace.push('s');return '.5'}}),()=>({[Symbol.toPrimitive](h){trace.push(h);return -0}}),()=>1.5,()=>2];for(let k=0;k<ops.length;k++)for(let i=0;i<f.length;i++)for(let j=0;j<f.length;j++){trace=[];let x;try{x=ops[k](f[i](),f[j]());x=typeof x+':'+(Object.is(x,-0)?'-0':String(x))}catch(e){x=e.name+':'+e.message}res.push([k,i,j,x,trace.join(',')]);}return JSON.stringify(res)})()"));
 for(int k=0;k<6;k++)JS_FreeValue(ctx,fn[k]);JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}
