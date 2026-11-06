# Stock Theresa outfit under armour

Baseline snapshot requested by owner: abff606. Experiment follows that commit.

Adult Sister material slots0/1 are torso/legs; slots2/3 are Face/Hair. Hide slot0
when Hero clothing category1 (torso) or5 (suit) is equipped. Hide slot1 for
category3 (legs) or5. Restore each independently when its covering is removed.
Headwear, gloves and boots do not hide an entire torso or leg section.
Child Sister has different materials and is excluded from this experiment.

Use native USkeletalMeshComponent::ShowMaterialSection RVA0070A450,
thiscall(mesh,materialIndex,show,LOD), verified RET0C and SEH handler023D1078.
The helper allocates/updates per-component hidden-material arrays and submits
the native render update. Apply on the existing graphics command boundary,
to the tracked Sister follower only, for each valid LOD. Never modify the
shared skeletal mesh asset. Reset cached mask when the follower is recreated.

Offline evidence: both adult package exports have torso/legs/Face/Hair.
Retail inventory handler RVA01A08040 lists head/torso/gloves/legs/feet/suit in
that order; names.bin confirms the six consecutive category names. Existing
HeroClothingComponent validates the six corresponding native categories.

Acceptance probe now reads actual LOD0 hidden-material flags at mesh+3D4;
it must match live equipped torso/legs/suit, with Face/Hair still visible.
The outfit driver removes/restores torso, legs or suit rather than a hat.
Runtime fit remains unverified; a section can include exposed skin/hands.
If body parts disappear, a finer geometry/section split is needed instead.

Manual check: equip chest only, then legs, remove each again; repeat on local
guest and host view, move and change camera distance, inspect arms/hands and
the face. No game process is launched automatically for this experiment.
