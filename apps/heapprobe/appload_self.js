// Asks for itself while it loads.
try { pocket.app.load('self'); } catch (e) { globalThis.alSelf = e.code; }
