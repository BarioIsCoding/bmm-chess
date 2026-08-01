"""Core constants. The square and piece helpers come from `_core`."""

from typing import Dict

from ._core import (
    square,
    square_file,
    square_rank,
    square_name,
    parse_square,
    square_distance,
    square_mirror,
    piece_symbol,
    piece_name,
)

WHITE: bool = True
BLACK: bool = False

COLOR_NAMES: Dict[bool, str] = {WHITE: "white", BLACK: "black"}

# Piece types are 1-6; 0 means no piece.
PAWN: int = 1
KNIGHT: int = 2
BISHOP: int = 3
ROOK: int = 4
QUEEN: int = 5
KING: int = 6

PIECE_TYPES = (PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING)

PIECE_SYMBOLS: Dict[int, str] = {
    PAWN: "p",
    KNIGHT: "n",
    BISHOP: "b",
    ROOK: "r",
    QUEEN: "q",
    KING: "k",
}

PIECE_NAMES: Dict[int, str] = {
    PAWN: "pawn",
    KNIGHT: "knight",
    BISHOP: "bishop",
    ROOK: "rook",
    QUEEN: "queen",
    KING: "king",
}

# Reverse lookup: symbol to piece type
SYMBOL_TO_PIECE_TYPE: Dict[str, int] = {v: k for k, v in PIECE_SYMBOLS.items()}

# Squares are indexed a1=0 .. h8=63.
A1, B1, C1, D1, E1, F1, G1, H1 = range(8)
A2, B2, C2, D2, E2, F2, G2, H2 = range(8, 16)
A3, B3, C3, D3, E3, F3, G3, H3 = range(16, 24)
A4, B4, C4, D4, E4, F4, G4, H4 = range(24, 32)
A5, B5, C5, D5, E5, F5, G5, H5 = range(32, 40)
A6, B6, C6, D6, E6, F6, G6, H6 = range(40, 48)
A7, B7, C7, D7, E7, F7, G7, H7 = range(48, 56)
A8, B8, C8, D8, E8, F8, G8, H8 = range(56, 64)

# All squares as a tuple
SQUARES = tuple(range(64))

# File and rank indices
FILES = tuple(range(8))  # 0=a, 7=h
RANKS = tuple(range(8))  # 0=1, 7=8

FILE_NAMES = "abcdefgh"
RANK_NAMES = "12345678"

# Square names mapping
SQUARE_NAMES = [f + r for r in RANK_NAMES for f in FILE_NAMES]

# Knight move offsets
KNIGHT_MOVES = (-17, -15, -10, -6, 6, 10, 15, 17)

# King move offsets
KING_MOVES = (-9, -8, -7, -1, 1, 7, 8, 9)

# Sliding piece directions
ROOK_DIRECTIONS = (-8, -1, 1, 8)
BISHOP_DIRECTIONS = (-9, -7, 7, 9)
QUEEN_DIRECTIONS = ROOK_DIRECTIONS + BISHOP_DIRECTIONS

# Pawn push directions
PAWN_PUSH = {WHITE: 8, BLACK: -8}
PAWN_DOUBLE_PUSH = {WHITE: 16, BLACK: -16}
PAWN_ATTACK_LEFT = {WHITE: 7, BLACK: -9}
PAWN_ATTACK_RIGHT = {WHITE: 9, BLACK: -7}

# Starting ranks for pawns and promotion ranks
PAWN_START_RANK = {WHITE: 1, BLACK: 6}
PAWN_PROMOTION_RANK = {WHITE: 7, BLACK: 0}

# Castling right bits
CASTLING_WHITE_KINGSIDE = 1
CASTLING_WHITE_QUEENSIDE = 2
CASTLING_BLACK_KINGSIDE = 4
CASTLING_BLACK_QUEENSIDE = 8

# All castling rights
CASTLING_ALL = 15

# Castling squares
CASTLING_KING_FROM = {WHITE: E1, BLACK: E8}
CASTLING_KINGSIDE_KING_TO = {WHITE: G1, BLACK: G8}
CASTLING_QUEENSIDE_KING_TO = {WHITE: C1, BLACK: C8}
CASTLING_KINGSIDE_ROOK_FROM = {WHITE: H1, BLACK: H8}
CASTLING_QUEENSIDE_ROOK_FROM = {WHITE: A1, BLACK: A8}
CASTLING_KINGSIDE_ROOK_TO = {WHITE: F1, BLACK: F8}
CASTLING_QUEENSIDE_ROOK_TO = {WHITE: D1, BLACK: D8}

# Squares that must be empty for castling
CASTLING_EMPTY_SQUARES = {
    (WHITE, True): (F1, G1),      # White kingside
    (WHITE, False): (B1, C1, D1), # White queenside
    (BLACK, True): (F8, G8),      # Black kingside
    (BLACK, False): (B8, C8, D8), # Black queenside
}

# Squares that must not be attacked for castling
CASTLING_SAFE_SQUARES = {
    (WHITE, True): (E1, F1, G1),  # White kingside
    (WHITE, False): (C1, D1, E1), # White queenside
    (BLACK, True): (E8, F8, G8),  # Black kingside
    (BLACK, False): (C8, D8, E8), # Black queenside
}

STARTING_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
STARTING_BOARD_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR"

PIECE_VALUES: Dict[int, int] = {
    PAWN: 100,
    KNIGHT: 320,
    BISHOP: 330,
    ROOK: 500,
    QUEEN: 900,
    KING: 20000,
}
