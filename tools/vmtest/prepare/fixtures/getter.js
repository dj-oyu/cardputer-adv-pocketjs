const original = pocket.kasane.procedural.register;
Object.defineProperty(pocket.kasane.procedural,'register', {get(){side(); return original;}});
pocket.kasane.procedural.register('prep.two',[1,2]);
