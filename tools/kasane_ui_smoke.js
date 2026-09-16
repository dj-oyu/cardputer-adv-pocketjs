function base(tx, updated) {
    tx.background(0x111827ff);
    tx.rect({bounds:[12,14,228,116],color:0x243247ff});
    tx.rect({bounds:[24,26,120,32],color:0xe2e8f0ff});
    tx.rect({bounds:[24,44,120,82],color:0x22c5b0ff});
    tx.rect({bounds:[84,56,164,94],color:0xef6fbc80});
    tx.rect({bounds:[24,102,216,108],color:0x111827ff});
    globalThis.meter = tx.rect({bounds:[24,102,updated?184:88,108],
        clip:[0,0,240,135],color:0xfacc15ff});
}
function initial() { kasane.replace(tx=>base(tx,false)); }
function update() { kasane.patch(tx=>meter.setRect(tx,[24,102,184,108])); }
function modal() {
    kasane.replace(tx=>{
        base(tx,true);
        tx.modal.open({backdrop:'dim-live',color:0x00000099,focus:7});
        tx.rect({bounds:[48,34,192,100],color:0xeee8ddff});
        tx.rect({bounds:[64,48,158,54],color:0x334155ff});
        tx.rect({bounds:[116,74,176,88],color:0x14b8a6ff});
    });
}
function closeModal() { kasane.replace(tx=>{tx.modal.close();base(tx,true);}); }
