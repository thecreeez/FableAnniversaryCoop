# Live appearance controls

Current default: controls disabled after the owner's final F11 bookmark,
2026-11-07 02:02:21 local (run20261107-020037-766-22560, profile version4).
Defaults: chest1.50, hips0.45, buttocks0.35, shoulders0.60, waist0.25, torso1.35.
The INI and missing-file code defaults both retain this profile.

Optional controls: set Controls.EnableShapeHotkeys=1 in theresa-garment-shape.ini.
Settings are read at launch. Hold a key for repeated .05 steps.

| Keys | Effect |
| --- | --- |
| F1 / F2 | Chest larger / smaller |
| F3 / F4 | Hips larger / smaller |
| F6 / F7 | Buttocks larger / smaller |
| F9 / F10 | Shoulders narrower / wider |
| [ / ] | Waist narrower / wider (Russian layout: physical Х / Ъ) |
| Shift+[ / Shift+] | Whole torso narrower / wider |
| F11 | Write complete current profile to the diagnostic log |

Body.WaistNarrowing ranges0..1.5. Zero preserves previous waist;1.5 uses70%
of its previous cross section at Spine1. Chest, hips and limb factors remain
independent. Start with small steps and inspect clothing fit.

Body.TorsoNarrowing ranges0..1.5, default0. It narrows the cross section of
Spine1/2/3 together, composing with waist at Spine1, without changing the
longitudinal axis or limb factors. At1.5 torso cross section is73% of baseline.

F11 does not save the INI: it logs a recoverable version4 bookmark including
waist_narrowing/waist_cross_scale and torso_narrowing/torso_cross_scale.
Changes last for the current session.
