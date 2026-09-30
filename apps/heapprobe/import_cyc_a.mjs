// A cycle that is fine: nothing is read before both have run.
import { cb } from 'imcycb';
export function ca() { return 'a' + cb(); }
