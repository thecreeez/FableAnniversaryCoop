# Native physique as a Theresa garment shaping alternative

Owner's latest preview confirms transitions now work but chest is absent.
The private-copy prototype never shaped the tested Ranger/Jack/Bandit shirts:
its live source-stream preflight rejected them. Offline vertex tests did not
prove those streams were available in the running game.

The native physique path is a viable next prototype for overall chest volume.
The [Bones config editor author](https://www.nexusmods.com/fableanniversary/mods/101)
documents Anniversary's blend of hero_weak/hero_strong bone configurations.
The [REDUX author](https://steamcommunity.com/app/288470/discussions/1/4330854300974987433/)
also uses hero_strong for proportions. Local shipped BNCFG files confirm chest
bone scales, e.g. Spine2 weak(1,.85,.8) versus strong(1,1.25,1.25).

This mod already reaches the same native MassBoneScaling controller through
006DEA30, called by the retail bridge01C4AAC0 and evaluated by006DED70. A
Theresa-only chest profile can use this path without altering gameplay stats,
global BNCFG files or activating a GPU morph shader. Apply deliberate shape
after the existing progression damping; compensate neighbouring body parts.
The BNCFG axes are bone-local: verify reference-pose directions first.

Limit: spine scaling changes the shared torso volume, including its back.
It cannot independently create two breast shapes on male armour. Sister has
L_breast_01S0/R_breast_01S0, but Hero and male garments have no corresponding
weighted bones. The current follower maps those extra bones to Spine3. Scaling
Sister-only breast bones cannot pull vertices of clothes weighted to Hero.

For distinct breast shapes, prepare different garment geometry with unchanged
UV/materials, or add a proper breast-weight/driver path. Offline private garment
UPKs avoid relying on retained live CPU streams; extending garment bones also
needs animation/parent mapping work. Neither has been implemented in this
research task. The recommendation is a small native torso-profile experiment
first, with explicit visual acceptance before calling it a breast solution.
