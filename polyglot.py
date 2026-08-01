"""
PolyGlot opening book support.

A book is a file of 16-byte big-endian records sorted by position key, so a
lookup is a binary search over the memory-mapped file. Keys come from
`board.zobrist_hash()`.

    with polyglot.open_reader("book.bin") as book:
        for entry in book.find_all(board):
            print(entry.move.uci(), entry.weight)
"""

import mmap
import os
import random as _random
import struct
from typing import Container, Iterator, List, NamedTuple, Optional, Union

from ._core import Board, Move, zobrist_hash

ENTRY_STRUCT = struct.Struct(">QHHI")
ENTRY_SIZE = ENTRY_STRUCT.size

# PolyGlot stores promotions as 1..4; our piece types run knight..queen as 2..5.
_PROMOTIONS = [None, 2, 3, 4, 5]


class Entry(NamedTuple):
    """One book record."""

    key: int
    raw_move: int
    weight: int
    learn: int
    move: Move


def _decode_move(raw_move: int) -> Move:
    to_square = raw_move & 0x3F
    from_square = (raw_move >> 6) & 0x3F
    promotion = _PROMOTIONS[(raw_move >> 12) & 0x7]
    return Move(from_square, to_square, promotion)


def _uncastle(board: Board, move: Move) -> Move:
    """PolyGlot writes castling as king-takes-rook, so e1h1 must become e1g1."""
    if move.promotion is None:
        if move.from_square == 4 and board.piece_type_at(4) == 6:
            if move.to_square == 7:
                return Move(4, 6)
            if move.to_square == 0:
                return Move(4, 2)
        elif move.from_square == 60 and board.piece_type_at(60) == 6:
            if move.to_square == 63:
                return Move(60, 62)
            if move.to_square == 56:
                return Move(60, 58)
    return move


class MemoryMappedReader:
    """A PolyGlot book mapped into memory."""

    def __init__(self, filename: Union[str, bytes, os.PathLike]):
        self.filename = filename
        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0)
        self._fd = os.open(filename, flags)
        try:
            size = os.fstat(self._fd).st_size
            if size == 0:
                self._mmap: Union[mmap.mmap, bytes] = b""
            else:
                self._mmap = mmap.mmap(self._fd, 0, access=mmap.ACCESS_READ)
        except Exception:
            os.close(self._fd)
            raise

        if len(self._mmap) % ENTRY_SIZE:
            raise ValueError(
                f"{filename!r} is {len(self._mmap)} bytes, not a whole number of "
                f"{ENTRY_SIZE}-byte PolyGlot entries")

    def __enter__(self) -> "MemoryMappedReader":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        if isinstance(self._mmap, mmap.mmap):
            self._mmap.close()
            self._mmap = b""
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def __len__(self) -> int:
        return len(self._mmap) // ENTRY_SIZE

    def __getitem__(self, index: int) -> Entry:
        if index < 0:
            index += len(self)
        if index < 0 or index >= len(self):
            raise IndexError("book entry index out of range")
        key, raw_move, weight, learn = ENTRY_STRUCT.unpack_from(
            self._mmap, index * ENTRY_SIZE)
        return Entry(key, raw_move, weight, learn, _decode_move(raw_move))

    def __iter__(self) -> Iterator[Entry]:
        for i in range(len(self)):
            yield self[i]

    def bisect_key_left(self, key: int) -> int:
        """Index of the first entry with a key >= `key`."""
        lo, hi = 0, len(self)
        while lo < hi:
            mid = (lo + hi) // 2
            mid_key, = struct.unpack_from(">Q", self._mmap, mid * ENTRY_SIZE)
            if mid_key < key:
                lo = mid + 1
            else:
                hi = mid
        return lo

    def find_all(self, board: Union[Board, int], *, minimum_weight: int = 1,
                 exclude_moves: Container[Move] = ()) -> Iterator[Entry]:
        """Yield the entries for a position, in the order the book stores them."""
        if isinstance(board, int):
            key: int = board
            context: Optional[Board] = None
        else:
            context = board
            key = zobrist_hash(board)

        i = self.bisect_key_left(key)
        size = len(self)

        while i < size:
            entry = self[i]
            i += 1

            if entry.key != key:
                break
            if entry.weight < minimum_weight:
                continue

            if context is not None:
                move = _uncastle(context, entry.move)
                entry = Entry(entry.key, entry.raw_move, entry.weight, entry.learn, move)
                if not context.is_legal(entry.move):
                    continue
            if exclude_moves and entry.move in exclude_moves:
                continue

            yield entry

    def find(self, board: Union[Board, int], *, minimum_weight: int = 1,
             exclude_moves: Container[Move] = ()) -> Entry:
        """The highest-weighted entry for a position."""
        best: Optional[Entry] = None
        for entry in self.find_all(board, minimum_weight=minimum_weight,
                                   exclude_moves=exclude_moves):
            if best is None or entry.weight > best.weight:
                best = entry
        if best is None:
            raise IndexError("no matching entry found")
        return best

    def choice(self, board: Union[Board, int], *, minimum_weight: int = 1,
               exclude_moves: Container[Move] = (),
               random: Optional[_random.Random] = None) -> Entry:
        """A uniformly random entry for a position."""
        chosen: Optional[Entry] = None
        seen = 0
        for entry in self.find_all(board, minimum_weight=minimum_weight,
                                   exclude_moves=exclude_moves):
            seen += 1
            if chosen is None or _randint(random, 0, seen - 1) == 0:
                chosen = entry
        if chosen is None:
            raise IndexError("no matching entry found")
        return chosen

    def weighted_choice(self, board: Union[Board, int], *,
                        exclude_moves: Container[Move] = (),
                        random: Optional[_random.Random] = None) -> Entry:
        """A random entry, with each entry weighted by its book weight."""
        total = 0
        entries: List[Entry] = []
        for entry in self.find_all(board, exclude_moves=exclude_moves):
            total += entry.weight
            entries.append(entry)
        if not entries:
            raise IndexError("no matching entry found")

        choice = _randint(random, 0, total - 1)
        running = 0
        for entry in entries:
            if running + entry.weight > choice:
                return entry
            running += entry.weight
        return entries[-1]

    def __repr__(self) -> str:
        return f"<MemoryMappedReader({self.filename!r}, {len(self)} entries)>"


def _randint(rng: Optional[_random.Random], a: int, b: int) -> int:
    return _random.randint(a, b) if rng is None else rng.randint(a, b)


def open_reader(path: Union[str, bytes, os.PathLike]) -> MemoryMappedReader:
    """Open a PolyGlot book for reading."""
    return MemoryMappedReader(path)
