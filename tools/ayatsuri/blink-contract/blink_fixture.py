"""Public synthetic only. No fox geometry, authored shader or runtime implementation."""
import math

def evaluate(both, left, right):
    vals=(both,left,right)
    if any(type(v) not in (int,float) or not math.isfinite(v) or not 0<=v<=1 for v in vals):
        raise ValueError('finite normalized controls required')
    parts=[]
    for side,v in ((-1,left),(1,right)):
        b=max(both,v)
        # Arbitrary public rectangle with independent upper/lower lid endpoints.
        cx=3*side; aperture=1-b
        parts.append({'side':side,'blink':b,'visible':b<0.98,
                      'upper':[(cx-1,aperture),(cx+1,aperture)],
                      'lower':[(cx-1,-aperture),(cx+1,-aperture)],
                      'aperture':aperture})
    return parts

def mask(part,x,y):
    """Binary synthetic spatial mask, NOT the original private material."""
    return part['visible'] and abs(x-3*part['side'])<=1 and abs(y)<=part['aperture']
