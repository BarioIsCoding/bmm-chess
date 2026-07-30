"""
Mint's Lab Chess Library
A custom chess library tailored for the Mint's Lab NodeBased project.

This library provides:
- Board representation with bitboards
- Legal move generation
- Check/checkmate detection
- FEN and SAN notation support
- PGN parsing
- UCI engine communication

Usage:
    from bmm_chess import Board, Move

    board = Board()
    board.push_san("e4")
    board.push_san("e5")
    print(board)

    for move in board.legal_moves:
        print(move.uci())
"""

__version__ = "1.0.0"
__author__ = "Mint's Lab Project"

from .constants import (
    WHITE,
    BLACK,
    COLOR_NAMES,

    PAWN,
    KNIGHT,
    BISHOP,
    ROOK,
    QUEEN,
    KING,
    PIECE_TYPES,
    PIECE_SYMBOLS,
    PIECE_NAMES,
    PIECE_VALUES,
    SYMBOL_TO_PIECE_TYPE,

    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    SQUARES,
    FILES,
    RANKS,
    FILE_NAMES,
    RANK_NAMES,
    SQUARE_NAMES,

    square,
    square_file,
    square_rank,
    square_name,
    parse_square,
    square_distance,
    square_mirror,

    piece_symbol,
    piece_name,

    STARTING_FEN,
    STARTING_BOARD_FEN,

    CASTLING_WHITE_KINGSIDE,
    CASTLING_WHITE_QUEENSIDE,
    CASTLING_BLACK_KINGSIDE,
    CASTLING_BLACK_QUEENSIDE,
    CASTLING_ALL,
)

from .piece import Piece
from .move import Move
from .board import Board, SquareSet, LegalMoveGenerator

from . import pgn
from .pgn import (
    Game,
    GameNode,
    Headers,
    read_game,
    scan_headers,
    BaseVisitor,
    StringExporter,
)

from . import engine
from .engine import (
    SimpleEngine,
    EngineInfo,
    BestMove,
    Limit,
    Option,
    EngineError,
    EngineTerminatedError,
)

__all__ = [
    "__version__",

    "WHITE",
    "BLACK",
    "COLOR_NAMES",

    "PAWN",
    "KNIGHT",
    "BISHOP",
    "ROOK",
    "QUEEN",
    "KING",
    "PIECE_TYPES",
    "PIECE_SYMBOLS",
    "PIECE_NAMES",
    "PIECE_VALUES",
    "SYMBOL_TO_PIECE_TYPE",

    "A1", "B1", "C1", "D1", "E1", "F1", "G1", "H1",
    "A2", "B2", "C2", "D2", "E2", "F2", "G2", "H2",
    "A3", "B3", "C3", "D3", "E3", "F3", "G3", "H3",
    "A4", "B4", "C4", "D4", "E4", "F4", "G4", "H4",
    "A5", "B5", "C5", "D5", "E5", "F5", "G5", "H5",
    "A6", "B6", "C6", "D6", "E6", "F6", "G6", "H6",
    "A7", "B7", "C7", "D7", "E7", "F7", "G7", "H7",
    "A8", "B8", "C8", "D8", "E8", "F8", "G8", "H8",
    "SQUARES",
    "FILES",
    "RANKS",
    "FILE_NAMES",
    "RANK_NAMES",
    "SQUARE_NAMES",

    "square",
    "square_file",
    "square_rank",
    "square_name",
    "parse_square",
    "square_distance",
    "square_mirror",

    "piece_symbol",
    "piece_name",

    "STARTING_FEN",
    "STARTING_BOARD_FEN",

    "CASTLING_WHITE_KINGSIDE",
    "CASTLING_WHITE_QUEENSIDE",
    "CASTLING_BLACK_KINGSIDE",
    "CASTLING_BLACK_QUEENSIDE",
    "CASTLING_ALL",

    "Piece",
    "Move",
    "Board",
    "SquareSet",
    "LegalMoveGenerator",

    "pgn",
    "Game",
    "GameNode",
    "Headers",
    "read_game",
    "scan_headers",
    "BaseVisitor",
    "StringExporter",

    "engine",
    "SimpleEngine",
    "EngineInfo",
    "BestMove",
    "Limit",
    "Option",
    "EngineError",
    "EngineTerminatedError",
]
