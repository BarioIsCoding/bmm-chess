# bmm_chess

A small, permissively licensed chess library for Python. Boards, legal moves, SAN, FEN,
PGN, and a UCI engine client — in about 3,300 lines of dependency-free Python you can
read in an afternoon.

```python
from bmm_chess import Board

board = Board()
board.push_san("e4")
board.push_san("e5")

print(board)
print(board.fen())
print(len(board.legal_moves))
```

## Why this instead of python-chess

python-chess is excellent and better in almost every dimension — it is faster, older,
far more featureful, and battle-tested. Use it unless one of these matters to you:

**Licensing.** python-chess is GPL-3.0+. That is a genuine obstacle if you ship a closed
or differently-licensed product. `bmm_chess` is MIT: use it, embed it, sell it, no
copyleft obligations.

**No dependencies, no install.** Standard library only. Copy the `bmm_chess` folder into
your project and import it. Nothing to pin, nothing to audit, no wheels, no C extension.

**Small enough to own.** Seven files. If it does something you don't like, you can read
the function and change it, rather than filing an issue and waiting.

If none of that applies, `pip install chess` and move on. This library exists because the
license and the dependency footprint mattered for the project it was built in.

## Installing

Copy the directory into your project:

```
your_project/
    bmm_chess/
    your_code.py
```

```python
from bmm_chess import Board, Move
```

No `pip`, no build step. Requires Python 3.8 or later.

## Doing things

```python
from bmm_chess import Board, Move

board = Board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")

# Moves
board.push_san("Nf3")             # or push_uci("g1f3"), or push(Move)
board.pop()                       # undo
for move in board.legal_moves:    # supports len() and `in` too
    print(board.san(move), move.uci())

# Position
board.piece_at(28)                # squares are ints, a1 = 0, h8 = 63
board.is_check()
board.is_checkmate()
board.is_game_over()
board.outcome()                   # "checkmate", "stalemate", ... or None
board.fen()
```

Reading and writing PGN:

```python
from bmm_chess import read_game

game = read_game(open("games.pgn"))     # or pass a string
print(game.headers["White"])

for move in game.mainline_moves():
    ...

print(str(game))                        # export back to PGN
```

Talking to Stockfish or any UCI engine:

```python
from bmm_chess import SimpleEngine, Limit

with SimpleEngine.popen_uci("stockfish") as engine:
    info = engine.analyse(board, Limit(depth=20))
    print(info.score_cp, info.pv)

    result = engine.play(board, Limit(time=1.0))
    print(result.move.uci())
```

## What works

Everything you need for normal chess: legal move generation with pins, checks, castling,
en passant and promotion; SAN and FEN in both directions; PGN with comments, NAGs and
variations that survive a read/write round trip; threefold and fifty-move draws;
insufficient material.

Move generation is verified against the standard perft suite — the starting position,
Kiwipete, and positions 3–5 from the Chess Programming Wiki all match published node
counts exactly. It runs at roughly 170k nodes/sec, which is pure-Python territory: fine
for analysis, tooling and game logic, not for writing an engine.

## What doesn't

- Standard chess only. No Chess960.
- `analyse(multipv=N)` returns just the last line, not N of them.
- Engine reads have no timeout, so a hung engine blocks the caller.
- A handful of python-chess conveniences are missing: `attacks()`, `ply()`, `status()`,
  `is_valid()`, `mirror()`, `epd()`.

## Coming from python-chess

Most code ports by changing the import. The differences that will actually bite:

- `outcome()` returns a string like `"checkmate"` or `None`, not an `Outcome` object.
- `is_game_over()` covers automatic endings only; pass `claim_draw=True` to include
  threefold repetition.
- Repetition detection reads the move stack, so it only sees moves you pushed onto that
  board. `set_fen()` clears the history.

## License

MIT. See [LICENSE](LICENSE).
