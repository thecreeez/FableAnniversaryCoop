# Theresa asset preparation and manual preview

These source-only helpers expect the project workspace layout: this repository
in `src/`, the owner-provided game copy in sibling `test-install/`, fixtures in
`fixtures/`, and an existing `local/` output directory. They do not download game
data. Generated game assets, caches, saves and binaries are not committed.

From the workspace root, using its Python environment:

1. `python src/tools/theresa/build_sister_limb_overlay.py` generates the private
   Sister limb overlay from the local original package, preserving the source.
2. `python src/tools/theresa/verify_sister_limb_overlay.py` checks its integrity.
3. `python src/tools/theresa/build_theresa_garment_cache.py` generates the
   THGEOM03 cache in `src/bin/Release/`, verifying CPU/GPU vertex attributes and
   bone palettes against the local Startup package. Copies the default INI only
   if no runtime INI exists.
4. `python src/tools/theresa/build-theresa-garment-offline.py` compiles/runs the
   production offline weight/format checks with the workspace's installed VS
   Build Tools14.51.36231 and Windows SDK10.0.26100.0. It never launches the game.

Build the solution normally. Run `src/play-theresa.bat` for the one-client owner
preview, or `powershell -File src/tools/theresa/play-theresa.ps1 -Check` to check
required paths without starting anything. Source fixtures remain unchanged.
See `docs/theresa-native-weight-paint.md` for the current implementation.
