// Imports a module the entry imports too: one instance, not two.
import { base } from 'imbase';
export let count = base;
export function bump() { count++; }
export const name = 'ok';
