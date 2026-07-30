"""
Mint's Lab Chess Library - Move Class
Represents a chess move with UCI support.
"""

from typing import Optional
from .constants import (
    square_name, parse_square, PIECE_SYMBOLS, SYMBOL_TO_PIECE_TYPE
)


class Move:
    """
    Represents a chess move.

    Attributes:
        from_square: Starting square index (0-63)
        to_square: Target square index (0-63)
        promotion: Piece type to promote to (QUEEN, ROOK, BISHOP, KNIGHT) or None
    """

    __slots__ = ("from_square", "to_square", "promotion")

    def __init__(self, from_square: int, to_square: int, promotion: Optional[int] = None):
        self.from_square = from_square
        self.to_square = to_square
        self.promotion = promotion

    @classmethod
    def from_uci(cls, uci: str) -> "Move":
        """Parse a UCI move string such as 'e2e4' or 'e7e8q'."""
        if len(uci) < 4 or len(uci) > 5:
            raise ValueError(f"Invalid UCI move: {uci}")

        try:
            from_square = parse_square(uci[0:2])
            to_square = parse_square(uci[2:4])
        except (ValueError, IndexError) as e:
            raise ValueError(f"Invalid UCI move: {uci}") from e

        promotion = None
        if len(uci) == 5:
            promo_char = uci[4].lower()
            if promo_char in SYMBOL_TO_PIECE_TYPE:
                promotion = SYMBOL_TO_PIECE_TYPE[promo_char]
            else:
                raise ValueError(f"Invalid promotion piece in UCI: {uci}")

        return cls(from_square, to_square, promotion)

    @classmethod
    def null(cls) -> "Move":
        """Create a null move (no movement)."""
        return cls(0, 0)

    def uci(self) -> str:
        """Convert the move to its UCI string, e.g. 'e2e4' or 'e7e8q'."""
        result = square_name(self.from_square) + square_name(self.to_square)
        if self.promotion:
            result += PIECE_SYMBOLS[self.promotion]
        return result

    def __str__(self) -> str:
        return self.uci()

    def __repr__(self) -> str:
        return f"Move.from_uci('{self.uci()}')"

    def __eq__(self, other: object) -> bool:
        if isinstance(other, Move):
            return (self.from_square == other.from_square and
                    self.to_square == other.to_square and
                    self.promotion == other.promotion)
        return False

    def __hash__(self) -> int:
        return hash((self.from_square, self.to_square, self.promotion))

    def __bool__(self) -> bool:
        """A move is truthy if it's not a null move."""
        return self.from_square != self.to_square or self.promotion is not None

    def copy(self) -> "Move":
        """Create a copy of this move."""
        return Move(self.from_square, self.to_square, self.promotion)
