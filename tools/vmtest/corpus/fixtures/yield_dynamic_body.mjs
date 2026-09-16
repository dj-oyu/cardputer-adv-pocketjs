function work() { let n = 0; for (let i = 0; i < 100; i++) n += i; return n; }
print('module-enter', work());
await 0;
export const value = work();
print('module-leave', value);
