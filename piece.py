"""
Mint's Lab Chess Library - Piece Class
Represents a chess piece with type and color.
"""

from .constants import (
    WHITE, BLACK, PIECE_SYMBOLS, PIECE_NAMES, SYMBOL_TO_PIECE_TYPE,
    PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING
)


class Piece:
    """
    Represents a chess piece.

    Attributes:
        piece_type: Type of piece (PAWN=1 through KING=6)
        color: Color of piece (WHITE=True, BLACK=False)
    """

    __slots__ = ("piece_type", "color")

    def __init__(self, piece_type: int, color: bool):
        self.piece_type = piece_type
        self.color = color

    @classmethod
    def from_symbol(cls, symbol: str) -> "Piece":
        """Create a piece from a symbol such as 'P', 'n' or 'K'; case sets colour."""
        lower = symbol.lower()
        if lower not in SYMBOL_TO_PIECE_TYPE:
            raise ValueError(f"Invalid piece symbol: {symbol}")

        piece_type = SYMBOL_TO_PIECE_TYPE[lower]
        color = symbol.isupper()
        return cls(piece_type, color)

    def symbol(self) -> str:
        """Get the piece symbol: uppercase for white, lowercase for black."""
        sym = PIECE_SYMBOLS[self.piece_type]
        return sym.upper() if self.color == WHITE else sym

    def unicode_symbol(self) -> str:
        """Get the Unicode chess piece symbol."""
        unicode_pieces = {
            (KING, WHITE): "♔",
            (QUEEN, WHITE): "♕",
            (ROOK, WHITE): "♖",
            (BISHOP, WHITE): "♗",
            (KNIGHT, WHITE): "♘",
            (PAWN, WHITE): "♙",
            (KING, BLACK): "♚",
            (QUEEN, BLACK): "♛",
            (ROOK, BLACK): "♜",
            (BISHOP, BLACK): "♝",
            (KNIGHT, BLACK): "♞",
            (PAWN, BLACK): "♟",
        }
        return unicode_pieces.get((self.piece_type, self.color), "?")

    def name(self) -> str:
        """Get the piece name (e.g., 'white knight')."""
        color_name = "white" if self.color == WHITE else "black"
        piece_name = PIECE_NAMES.get(self.piece_type, "unknown")
        return f"{color_name} {piece_name}"

    def __str__(self) -> str:
        return self.symbol()

    def __repr__(self) -> str:
        return f"Piece.from_symbol('{self.symbol()}')"

    def __eq__(self, other: object) -> bool:
        if isinstance(other, Piece):
            return self.piece_type == other.piece_type and self.color == other.color
        return False

    def __hash__(self) -> int:
        return hash((self.piece_type, self.color))

    def copy(self) -> "Piece":
        """Create a copy of this piece."""
        return Piece(self.piece_type, self.color)
