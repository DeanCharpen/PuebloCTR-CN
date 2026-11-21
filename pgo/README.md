# The build by profile (PGO)

`re4.profdata` tells the compiler how often each function of the game ran, and which way each of its branches went,
in replays of recorded play on Linux (fights, the village path, rain, the castle, cutscenes, a round of every room).
With it the 3DS build lays the often-run code out for speed (inlined, kept close together in memory) and the code
that never ran out for size. The game does the same work: only how its code is compiled changes.

- `tools/build3ds.py` uses it by itself; `PUEBLO_PGO=off` builds without it.
- The file holds function names of the game and counts, no code.
- After changes of the game's code it gets old bit by bit: a changed function no longer matches its entry and is
  compiled as without the profile. Nothing breaks; the gain only shrinks. It is made again before a release.
