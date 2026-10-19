"""Build-only DERBY input control; the shipping view chunk is untouched."""

FIXED_SAVED_RACE = 19


def instrument(view: str) -> str:
    replacements = (
        ('let sn = 0;', f'let sn = {FIXED_SAVED_RACE};'),
        ("function save() { if (dm) return; try { ST.set('derby.v1', {v: 1, pts: pts, race: raceNo}).then(nop, nop); } catch (e) {} }",
         'function save() { /* Diagnostic image: never write player storage. */ }'),
        ('raceNo = sn = v.race | 0;', 'raceNo = v.race | 0;'),
        ("v.race > 0 && scene === 'pad')", "v.race > 0 && scene === 'pad' && !dm)"),
    )
    for before, after in replacements:
        if view.count(before) != 1:
            raise ValueError(f'DERBY diagnostic view source drift: {before}')
        view = view.replace(before, after)
    return view
