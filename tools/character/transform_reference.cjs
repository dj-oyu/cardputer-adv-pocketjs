// Independent host oracle. Device authoring uses the shared C evaluator.
'use strict';
function invalid(message) { throw new TypeError('TransformReference: ' + message); }
function finiteFloat32(values, length) {
  if (!values || values.length !== length) invalid('length');
  for (let i=0;i<length;++i) if (!Number.isFinite(values[i]) || !Number.isFinite(Math.fround(values[i]))) invalid('finite float32 input');
}
function compose(pose, parent, out) {
  finiteFloat32(pose,9);
  if (parent !== null && parent !== undefined) finiteFloat32(parent,12);
  if (!out || out.length !== 12) invalid('output length');
  for (let i=6;i<9;++i) if (pose[i]===0) invalid('nonzero scale');
  const candidate=new Array(12);
  const x=pose[3],y=pose[4],z=pose[5],cx=Math.cos(x),sx=Math.sin(x),
    cy=Math.cos(y),sy=Math.sin(y),cz=Math.cos(z),sz=Math.sin(z);
  const a=cz*cy*pose[6],b=(cz*sy*sx-sz*cx)*pose[7],c=(cz*sy*cx+sz*sx)*pose[8],
    d=sz*cy*pose[6],e=(sz*sy*sx+cz*cx)*pose[7],f=(sz*sy*cx-cz*sx)*pose[8],
    g=-sy*pose[6],h=cy*sx*pose[7],j=cy*cx*pose[8];
  if (!parent) {
    candidate[0]=a;candidate[1]=b;candidate[2]=c;candidate[3]=pose[0];
    candidate[4]=d;candidate[5]=e;candidate[6]=f;candidate[7]=pose[1];
    candidate[8]=g;candidate[9]=h;candidate[10]=j;candidate[11]=pose[2];
  } else for(let k=0;k<12;k+=4) {
    candidate[k]=parent[k]*a+parent[k+1]*d+parent[k+2]*g;
    candidate[k+1]=parent[k]*b+parent[k+1]*e+parent[k+2]*h;
    candidate[k+2]=parent[k]*c+parent[k+1]*f+parent[k+2]*j;
    candidate[k+3]=parent[k]*pose[0]+parent[k+1]*pose[1]+parent[k+2]*pose[2]+parent[k+3];
  }
  for(let i=0;i<12;++i) {
    candidate[i]=Math.fround(candidate[i]);
    if(!Number.isFinite(candidate[i])) invalid('transform overflow');
  }
  for(let i=0;i<12;++i)out[i]=candidate[i];
}
module.exports={compose};
