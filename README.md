# bmm_chess

Fast chess library for Python. Bitboards, legal moves, SAN/FEN/EPD, PGN, PolyGlot
opening books, UCI engines.

The board layer is a C extension. Everything else is ordinary Python.

**16–179x faster than python-chess. MIT, not GPL. One build step, no dependencies.**

---

## The numbers

Calls per second, higher is better. Kiwipete position, best of three runs.

| operation | bmm_chess | python-chess | |
|---|---|---|---|
| `len(board.legal_moves)` | 3.4M/s | 19k/s | **179x** |
| `zobrist_hash(board)` | 9.7M/s | 59k/s | **164x** |
| `board.fen()` | 4.2M/s | 27k/s | **156x** |
| `Board(fen)` | 2.2M/s | 18k/s | **126x** |
| `board.status()` | 18.1M/s | 313k/s | **58x** |
| `push()` + `pop()` | 9.9M/s | 210k/s | **47x** |
| `list(board.legal_moves)` | 723k/s | 16k/s | **44x** |
| `board.san(move)` | 3.7M/s | 83k/s | **44x** |
| `board.piece_map()` | 580k/s | 35k/s | **16x** |

Perft, which exercises generation plus make/unmake together:

| | nodes/sec |
|---|---|
| `perft(board, n)`, whole tree walk in C | **170M** |
| your own Python loop over `legal_moves` / `push` / `pop` | 16.2M |
| the same Python loop, on python-chess | 589k |

Both bmm_chess rows run the identical C core. The difference is only whether Python is
driving the loop, and that costs you 10x in interpreter overhead. The middle row is what
ordinary code gets, so **27x is the honest apples-to-apples number**; the top row is a
bonus for the one case where you can hand the whole search down to C at once.

Intel i9-9900K, Windows 11, CPython 3.11.9, python-chess 1.11.2. Your absolute numbers
will differ; the ratios shouldn't move much. Reproduce the whole table with:

```bash
python -m bmm_chess.selftest compare
```

The gap is widest where python-chess has to build Python objects (FEN strings, piece
maps, whole boards) and narrowest where it was already doing bitboard work. `piece_map()`
is the floor at 16x, because both libraries end up allocating 32 objects and a dict, and
there's no way around that from C either.

**Those are primitives. Whole pipelines land lower.** Reading a PGN file end to end is
**7.8x** (410k moves/sec against 52k), because `pgn.py` is still Python and the parser
starts to dominate once the board work gets cheap. Budget somewhere between 8x and 30x
for real work, and the big numbers above for tight loops over the board itself.

## Should you use this?

**No, if** you need Chess960 or any other variant, Syzygy tablebases, or SVG board
rendering. python-chess has all three and this has none of them.

**Also no, if you lean on the engine client.** Basic `analyse` and `play` work, but
`multipv` silently gives you one line, reads have no timeout, and UCI option metadata
isn't parsed. python-chess's engine module is seven times the size and it earns it. See
[Rough edges](#rough-edges) before you commit to this for engine work.

**Yes, if:**

- **GPL is a blocker.** python-chess is GPL-3.0+, this is MIT. If you ship closed source
  that's the entire conversation, and no amount of benchmarking matters. (One asterisk,
  on the PolyGlot constants — see [License](#license).)
- **You're moving volume over the board itself.** Position mining, opening trees, tree
  search, anything hammering `legal_moves` / `push` / `pop` / `fen` / `zobrist_hash`.
  That's where the 27x-and-up numbers live.
- **You want a thin dependency surface.** Standard library only, plus a C compiler once
  at build time.

Note what's *not* on that list: bulk PGN parsing is only ~8x, so if that's your whole
workload the win is real but modest.

If none of it applies, `pip install chess` and get on with your life.

## Install

Drop the folder into your project and build the core once:

```bash
python bmm_chess/build_core.py
```

Run it as a **script**, not `python -m bmm_chess.build_core`. The `-m` form imports the
package first, and the package refuses to import without the extension the script is
about to build. Chicken, egg.

Needs Python 3.9+ and a C compiler (MSVC on Windows, gcc or clang elsewhere).

There is no pure-Python fallback and there won't be. If the extension is missing,
`import bmm_chess` raises and tells you how to fix it. A silent slow path is a slow path
you ship without noticing.

## The 60-second tour

```python
from bmm_chess import Board, Move

board = Board()                       # or Board(fen), or Board.empty()
board.push_san("e4")
board.push_uci("e7e5")

for move in board.legal_moves:        # len() and `in` work too
    print(board.san(move), move.uci())

board.is_checkmate()
board.outcome()                       # "checkmate", "stalemate", ... or None
board.fen()
board.pop()                           # undo
```

Squares are plain ints, a1 = 0 through h8 = 63. Colours are bools, `WHITE` is `True`.
If you know python-chess, you already know this API — that was deliberate.

## What's in the box

**Board and moves.** Legal generation with pins, checks, castling, en passant,
promotion. Make/unmake with a full undo stack. Repetition, fifty-move, insufficient
material. Roughly 105 methods on `Board`; `help(Board)` is the fastest way to browse
them.

**Notation, both directions.** SAN, LAN, UCI, FEN, EPD with operations, PGN with
comments, NAGs and nested variations that survive a round trip.

**Opening books.** Full PolyGlot `.bin` support, memory-mapped, with `find`, `find_all`,
`choice` and `weighted_choice`. Keys come from `board.zobrist_hash()`, which is the real
PolyGlot hash, so existing books just work:

```python
from bmm_chess import Board, polyglot

with polyglot.open_reader("books/chess/Rodent.bin") as book:
    entry = book.weighted_choice(Board())
    print(entry.move.uci(), entry.weight)
```

**Position validation.** `board.status()` returns a bitmask of what's wrong with a
position and `is_valid()` collapses it to a bool. Worth wiring up anywhere a FEN arrives
from a user, a browser extension or a socket. It catches missing kings, pawns on the back
rank, castling rights with no rook behind them, impossible check configurations, and a
dozen other things.

**Filtered move generation.** `generate_legal_moves(from_mask=...)`,
`generate_legal_captures()`, `generate_legal_ep()`. Useful for a drag-and-drop board:
ask what a single piece can do without generating and discarding everything else.

**Analysis primitives.** `attacks`, `attackers`, `pin`, `is_pinned`, `mirror`,
`transform`, plus the bitboard toolkit (`lsb`, `msb`, `popcount`, `between`, `ray`, the
`shift_*` family, `BB_*` constants) if you want to work at that level directly.

**Untrusted input.** `parse_uci` rejects well-formed-but-illegal moves. `find_move` fills
in a queen promotion so your UI doesn't have to ask. Both raise `IllegalMoveError` or
`InvalidMoveError`, which subclass `ValueError`, so existing handlers keep working.

## Gotchas

Things that will actually cost you an hour if nobody tells you:

- **`fen()` always writes the en passant square** when one exists. python-chess omits it
  unless the capture is legal. `epd()` follows python-chess, not `fen()`. Yes, that's
  inconsistent; it's inherited and changing it would break FEN round-trips people rely on.
- **`checkers_mask` is a property, `move_stack` is a method.** python-chess has these the
  other way round. Sorry.
- **`generate_legal_moves(to_mask=...)` matches castling on the king's destination**
  (c1/g1). python-chess matches it on the rook's square, because it encodes castling as
  king-takes-rook for Chess960. This library is standard chess only, so the king's square
  is the sane answer here.
- **`outcome()` returns a string or `None`**, not an object. `is_game_over()` only counts
  automatic endings; pass `claim_draw=True` for threefold.
- **Repetition detection reads the move stack.** It only sees moves you actually pushed.
  `set_fen()` wipes the history.
- **`status()` is a plain `int`**, not an `IntFlag`. Compare against the `STATUS_*`
  constants.
- There's no `BaseBoard`. `Board` is the only board class.

## How I know it's not lying to you

Move generators are easy to write and hard to trust. Three independent checks:

**Perft.** The full Chess Programming Wiki suite — startpos, Kiwipete, positions 3
through 6 — at every published depth. 610,195,852 nodes, exact match.

**A behavioural signature.** `selftest sig` walks a ~100k-node tree and hashes every FEN,
every SAN and every UCI it produces. python-chess walking the same tree produces the same
MD5, byte for byte. That pins behaviour to a second implementation instead of to whatever
this library happened to do the day the test was written.

**Differential testing.** ~50,000 positions compared against python-chess across legal
move sets, SAN both directions, attack and pin masks on all 64 squares for both colours,
FEN, EPD, status flags, filtered generation, and every move predicate. Zero divergences
that aren't documented above.

Zobrist hashing is checked against the nine test vectors published with the PolyGlot
format, and book lookups are compared entry-for-entry against `chess.polyglot` on real
books.

```bash
python -m bmm_chess.selftest              # everything, ~4s
python -m bmm_chess.selftest invariants   # quick, skips deep perft
python -m bmm_chess.selftest features     # status, pins, EPD, books
```

Run the last two before you commit anything. Run the first before you tag.

## Rough edges

Being honest about where this is weaker than python-chess:

**The engine client is the weak spot.** `engine.py` is 460 lines against python-chess's
3,143, and it shows:

- `analyse(multipv=N)` mutates one shared info object, so you get whatever the last
  `info` line was. The MultiPV data is thrown away.
- Reads have no timeout. A wedged engine hangs the caller, forever.
- Option parsing is a stub. Every option comes back as `type="unknown"` with no
  min/max/default, so you can't build a settings UI from it or validate what you send.
- No streaming analysis handle, no async API. If you're calling this from an event loop,
  it blocks.

Fixing that properly is the next real piece of work. It's pure Python and doesn't touch
the C core.

**No type stubs.** A C extension is opaque to tooling, so you currently get no
autocomplete, no mypy and no signature help for the entire `Board` API. A `_core.pyi`
would fix that in an afternoon. It's the cheapest big win left.

**PGN annotations aren't parsed.** `[%clk 0:03:00]` and `[%eval -1.2]` from Lichess and
chess.com sit in the raw comment string instead of becoming structured fields.

**No tablebases.** No Syzygy, no Gaviota. WDL-only Syzygy probing is a tractable subset
if someone wants it.

**No variants, no Chess960, no SVG.** Not planned. Standard chess only.

## Working on it

The interesting code is `_core.c`. Rebuild after any change:

```bash
python bmm_chess/build_core.py && python -m bmm_chess.selftest
```

`board.py`, `move.py` and `piece.py` are three-line re-export shims that keep the old
import paths alive. Don't put logic in them. `pgn.py`, `engine.py` and `polyglot.py` are
Python on purpose: they're I/O bound, and C would buy nothing.

## License

MIT. See [LICENSE](LICENSE).

One caveat worth stating out loud, since the licensing pitch above is half the point:
`_polyglot_keys.h` contains the 781 Zobrist constants that define the PolyGlot `.bin`
format. They're interoperability data (a reader using any other values finds nothing in
any book), but they did originate in GPL projects. If that matters for how you ship,
delete `polyglot.py`, the `_polyglot_keys.h` include and `zobrist_hash`. Those three are
the only things that touch them.
