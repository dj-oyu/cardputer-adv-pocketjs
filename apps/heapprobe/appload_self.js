// Asks for itself while it loads.
try { kobako.app.load('self'); } catch (e) { globalThis.alSelf = e.code; }
