"""
Mint's Lab Chess Library - Board Class
Core chess board implementation with bitboard representation.
"""

import re
from typing import Optional, Dict, Iterator, List, Tuple
from .constants import (
    WHITE, BLACK, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING,
    FILE_NAMES, RANK_NAMES,
    square_file, square_rank, square_name, parse_square,
    STARTING_FEN,
    A1, C1, D1, F1, G1, H1,
    A8, C8, D8, F8, G8, H8,
    CASTLING_WHITE_KINGSIDE, CASTLING_WHITE_QUEENSIDE,
    CASTLING_BLACK_KINGSIDE, CASTLING_BLACK_QUEENSIDE,
    CASTLING_ALL, CASTLING_EMPTY_SQUARES, CASTLING_SAFE_SQUARES,
    CASTLING_KING_FROM, CASTLING_KINGSIDE_KING_TO, CASTLING_QUEENSIDE_KING_TO,
    CASTLING_KINGSIDE_ROOK_FROM, CASTLING_QUEENSIDE_ROOK_FROM,
    PIECE_SYMBOLS, SYMBOL_TO_PIECE_TYPE
)
from .piece import Piece
from .move import Move

def _init_knight_attacks() -> List[int]:
    """Precompute knight attack bitboards for each square."""
    attacks = []
    for sq in range(64):
        bb = 0
        rank, file = square_rank(sq), square_file(sq)
        for dr, df in [(-2, -1), (-2, 1), (-1, -2), (-1, 2),
                       (1, -2), (1, 2), (2, -1), (2, 1)]:
            nr, nf = rank + dr, file + df
            if 0 <= nr <= 7 and 0 <= nf <= 7:
                bb |= 1 << (nr * 8 + nf)
        attacks.append(bb)
    return attacks


def _init_king_attacks() -> List[int]:
    """Precompute king attack bitboards for each square."""
    attacks = []
    for sq in range(64):
        bb = 0
        rank, file = square_rank(sq), square_file(sq)
        for dr in [-1, 0, 1]:
            for df in [-1, 0, 1]:
                if dr == 0 and df == 0:
                    continue
                nr, nf = rank + dr, file + df
                if 0 <= nr <= 7 and 0 <= nf <= 7:
                    bb |= 1 << (nr * 8 + nf)
        attacks.append(bb)
    return attacks


def _init_pawn_attacks() -> Tuple[List[int], List[int]]:
    """Precompute pawn attack bitboards for each square and color."""
    white_attacks = []
    black_attacks = []
    for sq in range(64):
        rank, file = square_rank(sq), square_file(sq)
        w_bb, b_bb = 0, 0
        # White pawns attack up-left and up-right
        if rank < 7:
            if file > 0:
                w_bb |= 1 << (sq + 7)
            if file < 7:
                w_bb |= 1 << (sq + 9)
        # Black pawns attack down-left and down-right
        if rank > 0:
            if file > 0:
                b_bb |= 1 << (sq - 9)
            if file < 7:
                b_bb |= 1 << (sq - 7)
        white_attacks.append(w_bb)
        black_attacks.append(b_bb)
    return white_attacks, black_attacks

KNIGHT_ATTACKS = _init_knight_attacks()
KING_ATTACKS = _init_king_attacks()
WHITE_PAWN_ATTACKS, BLACK_PAWN_ATTACKS = _init_pawn_attacks()


def _init_between() -> List[List[int]]:
    """Precompute, for every aligned square pair, the squares strictly between them."""
    table = [[0] * 64 for _ in range(64)]
    for a in range(64):
        ra, fa = a >> 3, a & 7
        for b in range(64):
            rb, fb = b >> 3, b & 7
            if a == b:
                continue
            if not (ra == rb or fa == fb or abs(ra - rb) == abs(fa - fb)):
                continue
            dr = (rb > ra) - (rb < ra)
            df = (fb > fa) - (fb < fa)
            bb = 0
            r, f = ra + dr, fa + df
            while (r, f) != (rb, fb):
                bb |= 1 << (r * 8 + f)
                r += dr
                f += df
            table[a][b] = bb
    return table


BETWEEN = _init_between()


def _rook_attacks(sq: int, occupied: int) -> int:
    """Calculate rook attacks from a square given occupied squares."""
    attacks = 0
    rank, file = square_rank(sq), square_file(sq)

    # Up
    for r in range(rank + 1, 8):
        target = r * 8 + file
        attacks |= 1 << target
        if occupied & (1 << target):
            break
    # Down
    for r in range(rank - 1, -1, -1):
        target = r * 8 + file
        attacks |= 1 << target
        if occupied & (1 << target):
            break
    # Right
    for f in range(file + 1, 8):
        target = rank * 8 + f
        attacks |= 1 << target
        if occupied & (1 << target):
            break
    # Left
    for f in range(file - 1, -1, -1):
        target = rank * 8 + f
        attacks |= 1 << target
        if occupied & (1 << target):
            break
    return attacks


def _bishop_attacks(sq: int, occupied: int) -> int:
    """Calculate bishop attacks from a square given occupied squares."""
    attacks = 0
    rank, file = square_rank(sq), square_file(sq)

    # Up-Right
    r, f = rank + 1, file + 1
    while r <= 7 and f <= 7:
        target = r * 8 + f
        attacks |= 1 << target
        if occupied & (1 << target):
            break
        r += 1
        f += 1
    # Up-Left
    r, f = rank + 1, file - 1
    while r <= 7 and f >= 0:
        target = r * 8 + f
        attacks |= 1 << target
        if occupied & (1 << target):
            break
        r += 1
        f -= 1
    # Down-Right
    r, f = rank - 1, file + 1
    while r >= 0 and f <= 7:
        target = r * 8 + f
        attacks |= 1 << target
        if occupied & (1 << target):
            break
        r -= 1
        f += 1
    # Down-Left
    r, f = rank - 1, file - 1
    while r >= 0 and f >= 0:
        target = r * 8 + f
        attacks |= 1 << target
        if occupied & (1 << target):
            break
        r -= 1
        f -= 1
    return attacks


# Sliding attacks on an otherwise empty board, used to find pin candidates.
ROOK_RAYS = [_rook_attacks(sq, 0) for sq in range(64)]
BISHOP_RAYS = [_bishop_attacks(sq, 0) for sq in range(64)]


class SquareSet:
    """
    Set of squares represented as a bitboard.
    Supports iteration, len(), and containment checks.
    """

    __slots__ = ("_mask",)

    def __init__(self, mask: int = 0):
        self._mask = mask

    def __iter__(self):
        bb = self._mask
        while bb:
            sq = (bb & -bb).bit_length() - 1
            yield sq
            bb &= bb - 1

    def __len__(self) -> int:
        return bin(self._mask).count('1')

    def __bool__(self) -> bool:
        return self._mask != 0

    def __contains__(self, sq: int) -> bool:
        return bool(self._mask & (1 << sq))

    @property
    def mask(self) -> int:
        """The underlying bitboard."""
        return self._mask

    def __int__(self) -> int:
        return self._mask

    @staticmethod
    def _coerce(other: object) -> Optional[int]:
        if isinstance(other, SquareSet):
            return other._mask
        if isinstance(other, int):
            return other
        return None

    def __or__(self, other: object) -> "SquareSet":
        mask = self._coerce(other)
        return NotImplemented if mask is None else SquareSet(self._mask | mask)

    def __and__(self, other: object) -> "SquareSet":
        mask = self._coerce(other)
        return NotImplemented if mask is None else SquareSet(self._mask & mask)

    def __xor__(self, other: object) -> "SquareSet":
        mask = self._coerce(other)
        return NotImplemented if mask is None else SquareSet(self._mask ^ mask)

    def __sub__(self, other: object) -> "SquareSet":
        mask = self._coerce(other)
        return NotImplemented if mask is None else SquareSet(self._mask & ~mask)

    def __eq__(self, other: object) -> bool:
        mask = self._coerce(other)
        return NotImplemented if mask is None else self._mask == mask

    def __hash__(self) -> int:
        return hash(self._mask)

    def tolist(self) -> List[int]:
        """The squares in this set, as a sorted list of square indices."""
        return list(self)

    def __repr__(self) -> str:
        return f"SquareSet({self._mask:#018x})"


class _BoardState:
    """Snapshot of board state for undo functionality."""
    __slots__ = ("pawns", "knights", "bishops", "rooks", "queens", "kings",
                 "white_pieces", "black_pieces", "turn", "castling_rights",
                 "ep_square", "halfmove_clock", "fullmove_number", "move")

    def __init__(self, board: "Board", move: "Move"):
        self.pawns = board._pawns
        self.knights = board._knights
        self.bishops = board._bishops
        self.rooks = board._rooks
        self.queens = board._queens
        self.kings = board._kings
        self.white_pieces = board._white
        self.black_pieces = board._black
        self.turn = board.turn
        self.castling_rights = board.castling_rights
        self.ep_square = board.ep_square
        self.halfmove_clock = board.halfmove_clock
        self.fullmove_number = board.fullmove_number
        self.move = move


class Board:
    """
    Chess board with bitboard representation.

    Provides full chess game state including:
    - Piece positions using bitboards
    - Turn tracking
    - Castling rights
    - En passant square
    - Move counters
    """

    def __init__(self, fen: Optional[str] = STARTING_FEN):
        self._pawns: int = 0
        self._knights: int = 0
        self._bishops: int = 0
        self._rooks: int = 0
        self._queens: int = 0
        self._kings: int = 0

        self._white: int = 0
        self._black: int = 0

        self.turn: bool = WHITE
        self.castling_rights: int = CASTLING_ALL
        self.ep_square: Optional[int] = None
        self.halfmove_clock: int = 0
        self.fullmove_number: int = 1

        # Move stack for undo
        self._stack: List[_BoardState] = []

        if fen:
            self.set_fen(fen)

    @property
    def occupied(self) -> int:
        """Bitboard of all occupied squares."""
        return self._white | self._black

    def occupied_co(self, color: bool) -> int:
        """Bitboard of squares occupied by a color."""
        return self._white if color == WHITE else self._black

    def pieces_mask(self, piece_type: int, color: bool) -> int:
        """Get bitboard of specific piece type and color."""
        type_bb = self._get_piece_bb(piece_type)
        color_bb = self._white if color == WHITE else self._black
        return type_bb & color_bb

    def _get_piece_bb(self, piece_type: int) -> int:
        """Get the bitboard for a piece type."""
        if piece_type == PAWN:
            return self._pawns
        elif piece_type == KNIGHT:
            return self._knights
        elif piece_type == BISHOP:
            return self._bishops
        elif piece_type == ROOK:
            return self._rooks
        elif piece_type == QUEEN:
            return self._queens
        elif piece_type == KING:
            return self._kings
        return 0

    def _set_piece_bb(self, piece_type: int, bb: int) -> None:
        """Set the bitboard for a piece type."""
        if piece_type == PAWN:
            self._pawns = bb
        elif piece_type == KNIGHT:
            self._knights = bb
        elif piece_type == BISHOP:
            self._bishops = bb
        elif piece_type == ROOK:
            self._rooks = bb
        elif piece_type == QUEEN:
            self._queens = bb
        elif piece_type == KING:
            self._kings = bb

    def pieces(self, piece_type: int, color: bool) -> SquareSet:
        """Get set of squares with specific piece type and color."""
        return SquareSet(self.pieces_mask(piece_type, color))

    def piece_at(self, sq: int) -> Optional[Piece]:
        """Get the piece at a square, or None if empty."""
        mask = 1 << sq
        if not (self.occupied & mask):
            return None

        color = WHITE if self._white & mask else BLACK

        if self._pawns & mask:
            return Piece(PAWN, color)
        elif self._knights & mask:
            return Piece(KNIGHT, color)
        elif self._bishops & mask:
            return Piece(BISHOP, color)
        elif self._rooks & mask:
            return Piece(ROOK, color)
        elif self._queens & mask:
            return Piece(QUEEN, color)
        elif self._kings & mask:
            return Piece(KING, color)
        return None

    def piece_type_at(self, sq: int) -> Optional[int]:
        """Get the piece type at a square, or None if empty."""
        mask = 1 << sq
        if not (self.occupied & mask):
            return None
        if self._pawns & mask:
            return PAWN
        elif self._knights & mask:
            return KNIGHT
        elif self._bishops & mask:
            return BISHOP
        elif self._rooks & mask:
            return ROOK
        elif self._queens & mask:
            return QUEEN
        elif self._kings & mask:
            return KING
        return None

    def color_at(self, sq: int) -> Optional[bool]:
        """Get the color of piece at a square, or None if empty."""
        mask = 1 << sq
        if self._white & mask:
            return WHITE
        elif self._black & mask:
            return BLACK
        return None

    def piece_map(self) -> Dict[int, Piece]:
        """Get a dictionary mapping squares to pieces."""
        result = {}
        for sq in range(64):
            piece = self.piece_at(sq)
            if piece:
                result[sq] = piece
        return result

    def _clear_square(self, sq: int) -> None:
        """Empty a square without building a Piece for the occupant."""
        inv_mask = ~(1 << sq)
        self._pawns &= inv_mask
        self._knights &= inv_mask
        self._bishops &= inv_mask
        self._rooks &= inv_mask
        self._queens &= inv_mask
        self._kings &= inv_mask
        self._white &= inv_mask
        self._black &= inv_mask

    def _set_piece_at(self, sq: int, piece_type: int, color: bool) -> None:
        """Place a piece on a square (internal use)."""
        mask = 1 << sq
        self._clear_square(sq)

        if piece_type == PAWN:
            self._pawns |= mask
        elif piece_type == KNIGHT:
            self._knights |= mask
        elif piece_type == BISHOP:
            self._bishops |= mask
        elif piece_type == ROOK:
            self._rooks |= mask
        elif piece_type == QUEEN:
            self._queens |= mask
        elif piece_type == KING:
            self._kings |= mask

        if color == WHITE:
            self._white |= mask
        else:
            self._black |= mask

    def _remove_piece_at(self, sq: int) -> Optional[Piece]:
        """Remove piece from a square and return it."""
        piece = self.piece_at(sq)
        if piece is None:
            return None
        self._clear_square(sq)
        return piece

    def king(self, color: bool) -> Optional[int]:
        """Get the square of the king for a color."""
        king_bb = self._kings & (self._white if color == WHITE else self._black)
        if king_bb:
            return (king_bb & -king_bb).bit_length() - 1
        return None

    def attackers_mask(self, color: bool, sq: int) -> int:
        """Get bitboard of pieces of given color attacking a square."""
        occupied = self._white | self._black
        queens = self._queens

        # Collect attackers of either colour, then mask down to `color` once.
        attackers = (
            (BLACK_PAWN_ATTACKS[sq] if color == WHITE else WHITE_PAWN_ATTACKS[sq]) & self._pawns
            | KNIGHT_ATTACKS[sq] & self._knights
            | KING_ATTACKS[sq] & self._kings
            | _rook_attacks(sq, occupied) & (self._rooks | queens)
            | _bishop_attacks(sq, occupied) & (self._bishops | queens)
        )
        return attackers & (self._white if color == WHITE else self._black)

    def attackers(self, color: bool, sq: int) -> SquareSet:
        """Get set of squares with pieces of given color attacking a square."""
        return SquareSet(self.attackers_mask(color, sq))

    def is_attacked_by(self, color: bool, sq: int) -> bool:
        """Check if a square is attacked by the given color."""
        return self.attackers_mask(color, sq) != 0

    def attacks_mask(self, sq: int) -> int:
        """Get bitboard of squares attacked by the piece at sq."""
        piece = self.piece_at(sq)
        if piece is None:
            return 0

        occupied = self.occupied

        if piece.piece_type == PAWN:
            if piece.color == WHITE:
                return WHITE_PAWN_ATTACKS[sq]
            else:
                return BLACK_PAWN_ATTACKS[sq]
        elif piece.piece_type == KNIGHT:
            return KNIGHT_ATTACKS[sq]
        elif piece.piece_type == BISHOP:
            return _bishop_attacks(sq, occupied)
        elif piece.piece_type == ROOK:
            return _rook_attacks(sq, occupied)
        elif piece.piece_type == QUEEN:
            return _bishop_attacks(sq, occupied) | _rook_attacks(sq, occupied)
        elif piece.piece_type == KING:
            return KING_ATTACKS[sq]
        return 0

    def is_check(self) -> bool:
        """Check if the side to move is in check."""
        king_sq = self.king(self.turn)
        if king_sq is None:
            # A side with no king has already lost decisively; treat that as
            # being in check so downstream end-state logic resolves to a win
            # for the opposing side rather than stalemate.
            return True
        return self.is_attacked_by(not self.turn, king_sq)

    def _king_attacked_after(self, move: Move, king_sq: int) -> bool:
        """
        Test whether our king would be attacked after `move`.

        Applies the move directly to the bitboards, queries, then restores.
        This skips the bookkeeping `push`/`pop` do (state snapshot, castling
        rights, clocks, move stack), which legality testing does not need.
        """
        us = self.turn
        from_mask = 1 << move.from_square
        to_mask = 1 << move.to_square

        piece_type = self.piece_type_at(move.from_square)
        if piece_type is None:
            return True

        # An en passant capture removes a pawn that is not on the target square.
        ep_mask = 0
        if (piece_type == PAWN and self.ep_square is not None
                and move.to_square == self.ep_square
                and square_file(move.from_square) != square_file(move.to_square)):
            ep_mask = 1 << (move.to_square - 8 if us == WHITE else move.to_square + 8)

        saved = (self._pawns, self._knights, self._bishops, self._rooks,
                 self._queens, self._kings, self._white, self._black)

        clear = ~(from_mask | to_mask | ep_mask)
        self._pawns &= clear
        self._knights &= clear
        self._bishops &= clear
        self._rooks &= clear
        self._queens &= clear
        self._kings &= clear
        self._white &= clear
        self._black &= clear

        landed = move.promotion if move.promotion else piece_type
        if landed == PAWN:
            self._pawns |= to_mask
        elif landed == KNIGHT:
            self._knights |= to_mask
        elif landed == BISHOP:
            self._bishops |= to_mask
        elif landed == ROOK:
            self._rooks |= to_mask
        elif landed == QUEEN:
            self._queens |= to_mask
        elif landed == KING:
            self._kings |= to_mask

        if us == WHITE:
            self._white |= to_mask
        else:
            self._black |= to_mask

        attacked = self.attackers_mask(
            not us, move.to_square if piece_type == KING else king_sq) != 0

        (self._pawns, self._knights, self._bishops, self._rooks,
         self._queens, self._kings, self._white, self._black) = saved
        return attacked

    def _pinned_mask(self, king_sq: int) -> int:
        """Bitboard of our pieces pinned against our king by an enemy slider."""
        us = self.turn
        our = self._white if us == WHITE else self._black
        their = self._black if us == WHITE else self._white
        occupied = our | their

        # Sliders that would attack the king on an otherwise empty board.
        queens = self._queens
        snipers = ((ROOK_RAYS[king_sq] & (self._rooks | queens)) |
                   (BISHOP_RAYS[king_sq] & (self._bishops | queens))) & their

        pinned = 0
        while snipers:
            sniper_sq = (snipers & -snipers).bit_length() - 1
            snipers &= snipers - 1
            blockers = BETWEEN[king_sq][sniper_sq] & occupied
            # Exactly one piece in the way, and it is ours -> pinned.
            if blockers and not (blockers & (blockers - 1)):
                pinned |= blockers & our
        return pinned

    def is_into_check(self, move: Move) -> bool:
        """Check if making a move would leave the king in check."""
        king_sq = self.king(self.turn)
        if king_sq is None:
            return True
        return self._king_attacked_after(move, king_sq)

    def gives_check(self, move: Move) -> bool:
        """Check if a move gives check to the opponent."""
        self.push(move)
        result = self.is_check()
        self.pop()
        return result

    @property
    def checkers_mask(self) -> int:
        """Bitboard of pieces giving check to the side to move."""
        king_sq = self.king(self.turn)
        if king_sq is None:
            return 0
        return self.attackers_mask(not self.turn, king_sq)

    def checkers(self) -> SquareSet:
        """Set of squares with pieces giving check."""
        return SquareSet(self.checkers_mask)

    def is_checkmate(self) -> bool:
        """Check if the position is checkmate."""
        if not self.is_check():
            return False
        return not self.legal_moves

    def is_stalemate(self) -> bool:
        """Check if the position is stalemate."""
        if self.king(self.turn) is None:
            return False
        if self.is_check():
            return False
        return not self.legal_moves

    def is_insufficient_material(self) -> bool:
        """Check for insufficient material draw."""
        if self.king(WHITE) is None or self.king(BLACK) is None:
            return False
        # K vs K
        if self.occupied == self._kings:
            return True

        # K+minor vs K
        knights = bin(self._knights).count('1')
        bishops = bin(self._bishops).count('1')

        if self._pawns == 0 and self._rooks == 0 and self._queens == 0:
            if knights + bishops <= 1:
                return True
            # K+B vs K+B with same color bishops
            if knights == 0 and bishops == 2:
                white_bishops = self._bishops & self._white
                black_bishops = self._bishops & self._black
                if bin(white_bishops).count('1') == 1 and bin(black_bishops).count('1') == 1:
                    # Check if bishops are on same color squares
                    w_sq = (white_bishops & -white_bishops).bit_length() - 1
                    b_sq = (black_bishops & -black_bishops).bit_length() - 1
                    if (square_rank(w_sq) + square_file(w_sq)) % 2 == (square_rank(b_sq) + square_file(b_sq)) % 2:
                        return True

        return False

    def is_fifty_moves(self) -> bool:
        """Check for 50-move rule draw."""
        return self.halfmove_clock >= 100

    def is_game_over(self, claim_draw: bool = False) -> bool:
        """
        Check if the game is over.

        Args:
            claim_draw: Also report positions where a draw is claimable but
                not automatic (threefold repetition).
        """
        return self.outcome(claim_draw=claim_draw) is not None

    def outcome(self, claim_draw: bool = False) -> Optional[str]:
        """
        Get game outcome if game is over, otherwise None.

        Args:
            claim_draw: Also consider claimable draws (threefold repetition).
        """
        if self.is_checkmate():
            return "checkmate"
        if self.is_stalemate():
            return "stalemate"
        if self.is_insufficient_material():
            return "insufficient_material"
        if self.is_fifty_moves():
            return "fifty_moves"
        if self.is_fivefold_repetition():
            return "fivefold_repetition"
        if claim_draw and self.is_repetition(3):
            return "threefold_repetition"
        return None

    def has_kingside_castling_rights(self, color: bool) -> bool:
        """Check if color has kingside castling rights."""
        if color == WHITE:
            return bool(self.castling_rights & CASTLING_WHITE_KINGSIDE)
        return bool(self.castling_rights & CASTLING_BLACK_KINGSIDE)

    def has_queenside_castling_rights(self, color: bool) -> bool:
        """Check if color has queenside castling rights."""
        if color == WHITE:
            return bool(self.castling_rights & CASTLING_WHITE_QUEENSIDE)
        return bool(self.castling_rights & CASTLING_BLACK_QUEENSIDE)

    def has_castling_rights(self, color: bool) -> bool:
        """Check if color has any castling rights."""
        return self.has_kingside_castling_rights(color) or self.has_queenside_castling_rights(color)

    def _has_castling_partners(self, color: bool, rook_square: int) -> bool:
        """
        Check that the king and rook castling would move are actually present.

        Castling rights parsed from a FEN are not self-validating, so a position
        such as "4k3/8/8/8/8/8/8/4K3 w K - 0 1" claims a right whose rook does
        not exist. Without this guard the rook would be conjured onto its
        destination square when the move is made.
        """
        king_mask = 1 << CASTLING_KING_FROM[color]
        rook_mask = 1 << rook_square
        ours = self._white if color == WHITE else self._black
        return bool(self._kings & ours & king_mask) and bool(self._rooks & ours & rook_mask)

    def _can_castle_kingside(self, color: bool) -> bool:
        """Check if kingside castling is currently legal."""
        if not self.has_kingside_castling_rights(color):
            return False

        if not self._has_castling_partners(color, CASTLING_KINGSIDE_ROOK_FROM[color]):
            return False

        for sq in CASTLING_EMPTY_SQUARES[(color, True)]:
            if self.occupied & (1 << sq):
                return False

        for sq in CASTLING_SAFE_SQUARES[(color, True)]:
            if self.is_attacked_by(not color, sq):
                return False

        return True

    def _can_castle_queenside(self, color: bool) -> bool:
        """Check if queenside castling is currently legal."""
        if not self.has_queenside_castling_rights(color):
            return False

        if not self._has_castling_partners(color, CASTLING_QUEENSIDE_ROOK_FROM[color]):
            return False

        for sq in CASTLING_EMPTY_SQUARES[(color, False)]:
            if self.occupied & (1 << sq):
                return False

        for sq in CASTLING_SAFE_SQUARES[(color, False)]:
            if self.is_attacked_by(not color, sq):
                return False

        return True

    def _generate_pseudo_legal_moves(self) -> Iterator[Move]:
        """Generate all pseudo-legal moves (may leave king in check)."""
        our_pieces = self._white if self.turn == WHITE else self._black
        occupied = self._white | self._black
        not_ours = ~our_pieces

        yield from self._generate_pawn_moves()

        pieces = self._knights & our_pieces
        while pieces:
            from_sq = (pieces & -pieces).bit_length() - 1
            pieces &= pieces - 1
            attacks = KNIGHT_ATTACKS[from_sq] & not_ours
            while attacks:
                to_sq = (attacks & -attacks).bit_length() - 1
                yield Move(from_sq, to_sq)
                attacks &= attacks - 1

        pieces = self._bishops & our_pieces
        while pieces:
            from_sq = (pieces & -pieces).bit_length() - 1
            pieces &= pieces - 1
            attacks = _bishop_attacks(from_sq, occupied) & not_ours
            while attacks:
                to_sq = (attacks & -attacks).bit_length() - 1
                yield Move(from_sq, to_sq)
                attacks &= attacks - 1

        pieces = self._rooks & our_pieces
        while pieces:
            from_sq = (pieces & -pieces).bit_length() - 1
            pieces &= pieces - 1
            attacks = _rook_attacks(from_sq, occupied) & not_ours
            while attacks:
                to_sq = (attacks & -attacks).bit_length() - 1
                yield Move(from_sq, to_sq)
                attacks &= attacks - 1

        pieces = self._queens & our_pieces
        while pieces:
            from_sq = (pieces & -pieces).bit_length() - 1
            pieces &= pieces - 1
            attacks = (_bishop_attacks(from_sq, occupied) |
                       _rook_attacks(from_sq, occupied)) & not_ours
            while attacks:
                to_sq = (attacks & -attacks).bit_length() - 1
                yield Move(from_sq, to_sq)
                attacks &= attacks - 1

        king_bb = self._kings & our_pieces
        if king_bb:
            king_sq = (king_bb & -king_bb).bit_length() - 1
            attacks = KING_ATTACKS[king_sq] & not_ours
            while attacks:
                to_sq = (attacks & -attacks).bit_length() - 1
                yield Move(king_sq, to_sq)
                attacks &= attacks - 1

        if self._can_castle_kingside(self.turn):
            yield Move(CASTLING_KING_FROM[self.turn], CASTLING_KINGSIDE_KING_TO[self.turn])
        if self._can_castle_queenside(self.turn):
            yield Move(CASTLING_KING_FROM[self.turn], CASTLING_QUEENSIDE_KING_TO[self.turn])

    def _generate_pawn_moves(self) -> Iterator[Move]:
        """Generate all pawn moves."""
        our_pawns = self.pieces_mask(PAWN, self.turn)
        their_pieces = self._black if self.turn == WHITE else self._white
        occupied = self.occupied

        # The capture masks clear the edge file before shifting, so a diagonal
        # step off the a-file (0xFE..) or h-file (0x7F..) cannot wrap onto the
        # opposite side of the board. The double-push mask selects the rank a
        # pawn may have just single-pushed to from its starting square.
        if self.turn == WHITE:
            single_push = (our_pawns << 8) & ~occupied
            double_push = ((single_push & 0x0000000000FF0000) << 8) & ~occupied
            left_captures = ((our_pawns & 0xFEFEFEFEFEFEFEFE) << 7) & their_pieces
            right_captures = ((our_pawns & 0x7F7F7F7F7F7F7F7F) << 9) & their_pieces
            promo_rank_mask = 0xFF00000000000000
        else:
            single_push = (our_pawns >> 8) & ~occupied
            double_push = ((single_push & 0x0000FF0000000000) >> 8) & ~occupied
            left_captures = ((our_pawns & 0xFEFEFEFEFEFEFEFE) >> 9) & their_pieces
            right_captures = ((our_pawns & 0x7F7F7F7F7F7F7F7F) >> 7) & their_pieces
            promo_rank_mask = 0x00000000000000FF

        moves = single_push
        while moves:
            to_sq = (moves & -moves).bit_length() - 1
            from_sq = to_sq - 8 if self.turn == WHITE else to_sq + 8
            if (1 << to_sq) & promo_rank_mask:
                for promo in [QUEEN, ROOK, BISHOP, KNIGHT]:
                    yield Move(from_sq, to_sq, promo)
            else:
                yield Move(from_sq, to_sq)
            moves &= moves - 1

        moves = double_push
        while moves:
            to_sq = (moves & -moves).bit_length() - 1
            from_sq = to_sq - 16 if self.turn == WHITE else to_sq + 16
            yield Move(from_sq, to_sq)
            moves &= moves - 1

        for captures, delta in [(left_captures, 7 if self.turn == WHITE else -9),
                                 (right_captures, 9 if self.turn == WHITE else -7)]:
            moves = captures
            while moves:
                to_sq = (moves & -moves).bit_length() - 1
                from_sq = to_sq - delta
                if (1 << to_sq) & promo_rank_mask:
                    for promo in [QUEEN, ROOK, BISHOP, KNIGHT]:
                        yield Move(from_sq, to_sq, promo)
                else:
                    yield Move(from_sq, to_sq)
                moves &= moves - 1

        if self.ep_square is not None:
            ep_mask = 1 << self.ep_square
            if self.turn == WHITE:
                left_ep = ((our_pawns & 0xFEFEFEFEFEFEFEFE) << 7) & ep_mask
                right_ep = ((our_pawns & 0x7F7F7F7F7F7F7F7F) << 9) & ep_mask
            else:
                left_ep = ((our_pawns & 0xFEFEFEFEFEFEFEFE) >> 9) & ep_mask
                right_ep = ((our_pawns & 0x7F7F7F7F7F7F7F7F) >> 7) & ep_mask

            if left_ep:
                from_sq = self.ep_square - 7 if self.turn == WHITE else self.ep_square + 9
                yield Move(from_sq, self.ep_square)
            if right_ep:
                from_sq = self.ep_square - 9 if self.turn == WHITE else self.ep_square + 7
                yield Move(from_sq, self.ep_square)

    def _generate_legal_moves(self) -> Iterator[Move]:
        """Generate all legal moves."""
        king_sq = self.king(self.turn)
        if king_sq is None:
            return

        in_check = self.attackers_mask(not self.turn, king_sq) != 0
        pinned = self._pinned_mask(king_sq)
        ep_square = self.ep_square

        for move in self._generate_pseudo_legal_moves():
            # A pseudo-legal move only needs verifying when it could expose the
            # king or fail to answer an existing attack: king moves, moves by a
            # pinned piece, any move made while in check, and en passant (which
            # clears two squares on one rank and can uncover a horizontal pin).
            # Everything else is legal by construction.
            if (in_check
                    or move.from_square == king_sq
                    or (1 << move.from_square) & pinned
                    or move.to_square == ep_square):
                if self._king_attacked_after(move, king_sq):
                    continue
            yield move

    @property
    def legal_moves(self) -> "LegalMoveGenerator":
        """
        The legal moves in this position.

        Lazily evaluated, but supports ``len()``, ``in`` and iteration.
        """
        return LegalMoveGenerator(self)

    def is_legal(self, move: Move) -> bool:
        """Check if a move is legal."""
        for pseudo_move in self._generate_pseudo_legal_moves():
            if move == pseudo_move:
                return not self.is_into_check(move)
        return False

    def is_capture(self, move: Move) -> bool:
        """Check if a move is a capture."""
        return self.piece_at(move.to_square) is not None or self.is_en_passant(move)

    def is_en_passant(self, move: Move) -> bool:
        """Check if a move is an en passant capture."""
        if self.ep_square is None or move.to_square != self.ep_square:
            return False
        if self.piece_type_at(move.from_square) != PAWN:
            return False
        # A real en passant capture is diagonal and lands on an empty square;
        # a straight pawn push onto the en passant square is neither.
        return (square_file(move.from_square) != square_file(move.to_square)
                and not (self.occupied & (1 << move.to_square)))

    def is_castling(self, move: Move) -> bool:
        """Check if a move is a castling move."""
        if self.piece_type_at(move.from_square) != KING:
            return False
        return abs(square_file(move.to_square) - square_file(move.from_square)) > 1

    def is_kingside_castling(self, move: Move) -> bool:
        """Check if a move is kingside (short) castling."""
        return (self.is_castling(move) and
                square_file(move.to_square) > square_file(move.from_square))

    def is_queenside_castling(self, move: Move) -> bool:
        """Check if a move is queenside (long) castling."""
        return (self.is_castling(move) and
                square_file(move.to_square) < square_file(move.from_square))

    def push(self, move: Move) -> None:
        """Make a move on the board."""
        # Handle null move (just flips turn)
        if move.from_square == move.to_square and move.promotion is None:
            self._stack.append(_BoardState(self, move))
            self.ep_square = None
            self.halfmove_clock += 1
            if self.turn == BLACK:
                self.fullmove_number += 1
            self.turn = not self.turn
            return

        captured = bool(self.occupied & (1 << move.to_square))
        self._stack.append(_BoardState(self, move))

        piece_type = self.piece_type_at(move.from_square)
        if piece_type is None:
            raise ValueError(f"No piece at {square_name(move.from_square)}")
        color = self.color_at(move.from_square)

        # Handle en passant capture
        ep_captured_sq = None
        if (piece_type == PAWN and move.to_square == self.ep_square
                and square_file(move.from_square) != square_file(move.to_square)):
            ep_captured_sq = self.ep_square - 8 if color == WHITE else self.ep_square + 8
            self._clear_square(ep_captured_sq)

        # Remove piece from source square. Any piece captured on the
        # destination is cleared by _set_piece_at below.
        self._clear_square(move.from_square)

        final_piece_type = move.promotion if move.promotion else piece_type
        self._set_piece_at(move.to_square, final_piece_type, color)

        # Handle castling: the king has moved two files, so shift the rook too.
        if piece_type == KING:
            file_delta = square_file(move.to_square) - square_file(move.from_square)
            if file_delta == 2:
                self._clear_square(CASTLING_KINGSIDE_ROOK_FROM[color])
                self._set_piece_at(F1 if color == WHITE else F8, ROOK, color)
            elif file_delta == -2:
                self._clear_square(CASTLING_QUEENSIDE_ROOK_FROM[color])
                self._set_piece_at(D1 if color == WHITE else D8, ROOK, color)

        if piece_type == KING:
            if color == WHITE:
                self.castling_rights &= ~(CASTLING_WHITE_KINGSIDE | CASTLING_WHITE_QUEENSIDE)
            else:
                self.castling_rights &= ~(CASTLING_BLACK_KINGSIDE | CASTLING_BLACK_QUEENSIDE)

        if piece_type == ROOK:
            if move.from_square == A1:
                self.castling_rights &= ~CASTLING_WHITE_QUEENSIDE
            elif move.from_square == H1:
                self.castling_rights &= ~CASTLING_WHITE_KINGSIDE
            elif move.from_square == A8:
                self.castling_rights &= ~CASTLING_BLACK_QUEENSIDE
            elif move.from_square == H8:
                self.castling_rights &= ~CASTLING_BLACK_KINGSIDE

        # Update castling rights if rook is captured
        if move.to_square == A1:
            self.castling_rights &= ~CASTLING_WHITE_QUEENSIDE
        elif move.to_square == H1:
            self.castling_rights &= ~CASTLING_WHITE_KINGSIDE
        elif move.to_square == A8:
            self.castling_rights &= ~CASTLING_BLACK_QUEENSIDE
        elif move.to_square == H8:
            self.castling_rights &= ~CASTLING_BLACK_KINGSIDE

        self.ep_square = None
        if piece_type == PAWN:
            diff = move.to_square - move.from_square
            if abs(diff) == 16:  # Double pawn push
                self.ep_square = (move.from_square + move.to_square) // 2

        if piece_type == PAWN or captured or ep_captured_sq:
            self.halfmove_clock = 0
        else:
            self.halfmove_clock += 1

        if color == BLACK:
            self.fullmove_number += 1

        self.turn = not self.turn

    def push_uci(self, uci: str) -> Move:
        """Make a move from UCI string."""
        move = Move.from_uci(uci)
        self.push(move)
        return move

    def pop(self) -> Move:
        """Undo the last move and return it."""
        if not self._stack:
            raise IndexError("Move stack is empty")

        state = self._stack.pop()

        self._pawns = state.pawns
        self._knights = state.knights
        self._bishops = state.bishops
        self._rooks = state.rooks
        self._queens = state.queens
        self._kings = state.kings
        self._white = state.white_pieces
        self._black = state.black_pieces

        self.turn = state.turn
        self.castling_rights = state.castling_rights
        self.ep_square = state.ep_square
        self.halfmove_clock = state.halfmove_clock
        self.fullmove_number = state.fullmove_number

        return state.move

    def peek(self) -> Optional[Move]:
        """Peek at the last move without undoing it."""
        if self._stack:
            return self._stack[-1].move
        return None

    def set_fen(self, fen: str) -> None:
        """
        Set board position from FEN string.

        Raises:
            ValueError: If the FEN is malformed. Fields after the piece
                placement are optional and fall back to sensible defaults.
        """
        parts = fen.split()
        if not parts:
            raise ValueError("Invalid FEN: empty string")

        # Validate piece placement before touching board state, so a bad FEN
        # leaves the board untouched rather than half-written.
        ranks = parts[0].split('/')
        if len(ranks) != 8:
            raise ValueError(f"Invalid FEN: expected 8 ranks, got {len(ranks)}")

        placements: List[Tuple[int, Piece]] = []
        for rank_index, rank_str in enumerate(ranks):
            file_index = 0
            for char in rank_str:
                if char.isdigit():
                    if char == '0':
                        raise ValueError(f"Invalid FEN: '0' is not a valid skip in rank {8 - rank_index}")
                    file_index += int(char)
                else:
                    if file_index > 7:
                        raise ValueError(
                            f"Invalid FEN: rank {8 - rank_index} ('{rank_str}') overflows the board")
                    placements.append(((7 - rank_index) * 8 + file_index, Piece.from_symbol(char)))
                    file_index += 1
            if file_index != 8:
                raise ValueError(
                    f"Invalid FEN: rank {8 - rank_index} ('{rank_str}') describes "
                    f"{file_index} squares, expected 8")

        # Parse the remaining fields before mutating, for the same reason.
        if len(parts) >= 2 and parts[1] not in ('w', 'b'):
            raise ValueError(f"Invalid FEN: side to move must be 'w' or 'b', got '{parts[1]}'")
        turn = WHITE if len(parts) < 2 or parts[1] == 'w' else BLACK

        castling_rights = 0
        if len(parts) >= 3 and parts[2] != '-':
            for char in parts[2]:
                if char == 'K':
                    castling_rights |= CASTLING_WHITE_KINGSIDE
                elif char == 'Q':
                    castling_rights |= CASTLING_WHITE_QUEENSIDE
                elif char == 'k':
                    castling_rights |= CASTLING_BLACK_KINGSIDE
                elif char == 'q':
                    castling_rights |= CASTLING_BLACK_QUEENSIDE
                else:
                    raise ValueError(f"Invalid FEN: bad castling field '{parts[2]}'")

        ep_square = None
        if len(parts) >= 4 and parts[3] != '-':
            name = parts[3]
            if len(name) != 2 or name[0] not in FILE_NAMES or name[1] not in RANK_NAMES:
                raise ValueError(f"Invalid FEN: bad en passant square '{name}'")
            ep_square = parse_square(name)

        try:
            halfmove_clock = int(parts[4]) if len(parts) >= 5 else 0
            fullmove_number = int(parts[5]) if len(parts) >= 6 else 1
        except ValueError:
            raise ValueError(f"Invalid FEN: move counters must be integers, got {parts[4:6]}") from None
        if halfmove_clock < 0:
            raise ValueError(f"Invalid FEN: negative halfmove clock {halfmove_clock}")
        if fullmove_number < 1:
            raise ValueError(f"Invalid FEN: fullmove number must be >= 1, got {fullmove_number}")

        self._pawns = 0
        self._knights = 0
        self._bishops = 0
        self._rooks = 0
        self._queens = 0
        self._kings = 0
        self._white = 0
        self._black = 0
        self._stack.clear()

        for sq, piece in placements:
            self._set_piece_at(sq, piece.piece_type, piece.color)

        self.turn = turn
        self.castling_rights = castling_rights
        self.ep_square = ep_square
        self.halfmove_clock = halfmove_clock
        self.fullmove_number = fullmove_number

    def fen(self) -> str:
        """Generate FEN string for current position."""
        fen_parts = []
        for rank in range(7, -1, -1):
            empty = 0
            rank_str = ""
            for file in range(8):
                sq = rank * 8 + file
                piece = self.piece_at(sq)
                if piece:
                    if empty > 0:
                        rank_str += str(empty)
                        empty = 0
                    rank_str += piece.symbol()
                else:
                    empty += 1
            if empty > 0:
                rank_str += str(empty)
            fen_parts.append(rank_str)

        board_fen = '/'.join(fen_parts)

        color = 'w' if self.turn == WHITE else 'b'

        castling = ""
        if self.castling_rights & CASTLING_WHITE_KINGSIDE:
            castling += 'K'
        if self.castling_rights & CASTLING_WHITE_QUEENSIDE:
            castling += 'Q'
        if self.castling_rights & CASTLING_BLACK_KINGSIDE:
            castling += 'k'
        if self.castling_rights & CASTLING_BLACK_QUEENSIDE:
            castling += 'q'
        if not castling:
            castling = '-'

        ep = square_name(self.ep_square) if self.ep_square is not None else '-'

        return f"{board_fen} {color} {castling} {ep} {self.halfmove_clock} {self.fullmove_number}"

    def board_fen(self) -> str:
        """Get just the board portion of the FEN."""
        return self.fen().split()[0]

    def copy(self, stack: bool = True) -> "Board":
        """Create a copy of the board."""
        board = Board.__new__(Board)
        board._pawns = self._pawns
        board._knights = self._knights
        board._bishops = self._bishops
        board._rooks = self._rooks
        board._queens = self._queens
        board._kings = self._kings
        board._white = self._white
        board._black = self._black
        board.turn = self.turn
        board.castling_rights = self.castling_rights
        board.ep_square = self.ep_square
        board.halfmove_clock = self.halfmove_clock
        board.fullmove_number = self.fullmove_number
        board._stack = list(self._stack) if stack else []
        return board

    def __copy__(self) -> "Board":
        return self.copy()

    def __str__(self) -> str:
        """ASCII board representation."""
        lines = []
        for rank in range(7, -1, -1):
            line = ""
            for file in range(8):
                sq = rank * 8 + file
                piece = self.piece_at(sq)
                if piece:
                    line += piece.symbol()
                else:
                    line += '.'
                line += ' '
            lines.append(f"{rank + 1} {line.strip()}")
        lines.append("  a b c d e f g h")
        return '\n'.join(lines)

    def __repr__(self) -> str:
        return f"Board('{self.fen()}')"

    def unicode(self, invert_color: bool = False, empty_square: str = "⬜") -> str:
        """Unicode board representation."""
        lines = []
        for rank in range(7, -1, -1):
            line = ""
            for file in range(8):
                sq = rank * 8 + file
                piece = self.piece_at(sq)
                if piece:
                    line += piece.unicode_symbol()
                else:
                    if (rank + file) % 2 == 0:
                        line += "⬛" if not invert_color else "⬜"
                    else:
                        line += "⬜" if not invert_color else "⬛"
            lines.append(f"{rank + 1} {line}")
        lines.append("  ａｂｃｄｅｆｇｈ")
        return '\n'.join(lines)

    def san(self, move: Move) -> str:
        """Convert a move to Standard Algebraic Notation, e.g. 'Nxf3+', 'O-O', 'e8=Q'."""
        piece = self.piece_at(move.from_square)
        if piece is None:
            raise ValueError(f"No piece at {square_name(move.from_square)}")

        # Castling is written as O-O / O-O-O, but still takes a +/# suffix,
        # so fall through to the check test below rather than returning here.
        if self.is_castling(move):
            san_str = "O-O" if self.is_kingside_castling(move) else "O-O-O"
            return san_str + self._san_check_suffix(move)

        san_str = ""
        is_capture = self.is_capture(move)

        if piece.piece_type == PAWN:
            if is_capture:
                san_str = FILE_NAMES[square_file(move.from_square)] + "x"
            san_str += square_name(move.to_square)
            if move.promotion:
                san_str += "=" + PIECE_SYMBOLS[move.promotion].upper()
        else:
            san_str = PIECE_SYMBOLS[piece.piece_type].upper()

            disambig = self._san_disambiguation(move, piece.piece_type)
            san_str += disambig

            if is_capture:
                san_str += "x"

            san_str += square_name(move.to_square)

        return san_str + self._san_check_suffix(move)

    def _san_check_suffix(self, move: Move) -> str:
        """Return '#', '+' or '' for the position arising after `move`."""
        self.push(move)
        try:
            if self.is_checkmate():
                return "#"
            return "+" if self.is_check() else ""
        finally:
            self.pop()

    def _san_disambiguation(self, move: Move, piece_type: int) -> str:
        """Get disambiguation string for SAN notation."""
        dominated_file = False
        dominated_rank = False

        from_file = square_file(move.from_square)
        from_rank = square_rank(move.from_square)

        # Find other pieces of same type that can move to the same square
        for other_move in self._generate_pseudo_legal_moves():
            if other_move.from_square == move.from_square:
                continue
            if other_move.to_square != move.to_square:
                continue

            other_piece = self.piece_at(other_move.from_square)
            if other_piece is None or other_piece.piece_type != piece_type:
                continue

            if self.is_into_check(other_move):
                continue

            if square_file(other_move.from_square) == from_file:
                dominated_file = True
            if square_rank(other_move.from_square) == from_rank:
                dominated_rank = True

        if not dominated_file and not dominated_rank:
            # Check if there's any ambiguity at all
            for other_move in self._generate_pseudo_legal_moves():
                if other_move.from_square == move.from_square:
                    continue
                if other_move.to_square != move.to_square:
                    continue
                other_piece = self.piece_at(other_move.from_square)
                if other_piece and other_piece.piece_type == piece_type:
                    if not self.is_into_check(other_move):
                        return FILE_NAMES[from_file]
            return ""

        if dominated_file and dominated_rank:
            return FILE_NAMES[from_file] + RANK_NAMES[from_rank]
        elif dominated_rank:
            return FILE_NAMES[from_file]
        else:
            return RANK_NAMES[from_rank]

    def parse_san(self, san: str) -> Move:
        """
        Parse a SAN move string such as 'Nf3', 'exd5' or 'O-O'.

        Raises:
            ValueError: If the SAN is invalid, illegal here, or ambiguous.
        """
        original = san
        san = san.strip()

        # Strip annotations that PGN allows to ride along with a move:
        # NAG references ($1), traditional glyphs (!, ?, !?, ...), the optional
        # "e.p." suffix, and finally the check/checkmate marks.
        san = re.sub(r'\$\d+', '', san)
        san = re.sub(r'\s*e\.?p\.?\s*$', '', san, flags=re.IGNORECASE)
        san = san.replace('!', '').replace('?', '')
        san = san.rstrip('+#').strip()

        if not san:
            raise ValueError(f"Invalid SAN: {original!r} has no move text")

        if san in ("O-O", "0-0"):
            king_sq = self.king(self.turn)
            if king_sq is None:
                raise ValueError("No king on board")
            to_sq = G1 if self.turn == WHITE else G8
            return Move(king_sq, to_sq)

        if san in ("O-O-O", "0-0-0"):
            king_sq = self.king(self.turn)
            if king_sq is None:
                raise ValueError("No king on board")
            to_sq = C1 if self.turn == WHITE else C8
            return Move(king_sq, to_sq)

        promotion = None
        if "=" in san:
            san, _, promo_char = san.partition("=")
            promo_char = promo_char.strip().lower()
            if promo_char in SYMBOL_TO_PIECE_TYPE:
                promotion = SYMBOL_TO_PIECE_TYPE[promo_char]
            else:
                raise ValueError(f"Invalid promotion piece in SAN {original!r}: {promo_char!r}")

        san = san.replace("x", "")

        if not san:
            raise ValueError(f"Invalid SAN: {original!r} has no move text")

        if san[0].isupper():
            piece_char = san[0].lower()
            if piece_char not in SYMBOL_TO_PIECE_TYPE:
                raise ValueError(f"Invalid piece in SAN {original!r}: {san[0]!r}")
            piece_type = SYMBOL_TO_PIECE_TYPE[piece_char]
            san = san[1:]
        else:
            piece_type = PAWN

        # The last two characters should be the target square
        if len(san) < 2:
            raise ValueError(f"Invalid SAN {original!r}: missing target square")

        target = san[-2:]
        if target[0] not in FILE_NAMES or target[1] not in RANK_NAMES:
            raise ValueError(f"Invalid SAN {original!r}: bad target square {target!r}")
        to_square = parse_square(target)
        san = san[:-2]

        from_file = None
        from_rank = None
        for char in san:
            if char in FILE_NAMES:
                from_file = FILE_NAMES.index(char)
            elif char in RANK_NAMES:
                from_rank = RANK_NAMES.index(char)

        matching_moves = []
        for move in self.legal_moves:
            if move.to_square != to_square:
                continue
            if move.promotion != promotion:
                continue

            piece = self.piece_at(move.from_square)
            if piece is None or piece.piece_type != piece_type:
                continue

            if from_file is not None and square_file(move.from_square) != from_file:
                continue
            if from_rank is not None and square_rank(move.from_square) != from_rank:
                continue

            matching_moves.append(move)

        if len(matching_moves) == 0:
            raise ValueError(f"Illegal or unparseable SAN {original!r} in position {self.fen()!r}")
        if len(matching_moves) > 1:
            raise ValueError(
                f"Ambiguous SAN {original!r}: matches "
                f"{', '.join(m.uci() for m in matching_moves)}")

        return matching_moves[0]

    def push_san(self, san: str) -> Move:
        """Parse and make a SAN move."""
        move = self.parse_san(san)
        self.push(move)
        return move

    def variation_san(self, moves: List[Move]) -> str:
        """Convert a variation (list of moves) to SAN string."""
        board = self.copy(stack=False)
        san_moves = []
        for move in moves:
            san_moves.append(board.san(move))
            board.push(move)
        return " ".join(san_moves)

    def _position_hash(self) -> int:
        """
        Compute a hash of the current position for repetition detection.
        Uses Zobrist-like hashing based on Python's built-in hash.
        """
        return hash((
            self._pawns, self._knights, self._bishops,
            self._rooks, self._queens, self._kings,
            self._white, self._black,
            self.turn, self.castling_rights, self.ep_square
        ))

    def is_repetition(self, count: int = 3) -> bool:
        """Check whether the current position has occurred `count` times (3 = threefold)."""
        current_hash = self._position_hash()

        # Count occurrences including current position
        occurrences = 1

        board = self.copy()
        while board._stack:
            board.pop()
            if board._position_hash() == current_hash:
                occurrences += 1
                if occurrences >= count:
                    return True

        return occurrences >= count

    def can_claim_draw(self) -> bool:
        """Check if the current player can claim a draw."""
        return self.is_fifty_moves() or self.is_repetition(3)

    def can_claim_threefold_repetition(self) -> bool:
        """Check for threefold repetition draw claim."""
        return self.is_repetition(3)

    def is_fivefold_repetition(self) -> bool:
        """Check for automatic fivefold repetition draw."""
        return self.is_repetition(5)

    def is_seventyfive_moves(self) -> bool:
        """Check for 75-move rule automatic draw."""
        return self.halfmove_clock >= 150

    def result(self, claim_draw: bool = False) -> str:
        """
        Get the game result: "1-0", "0-1", "1/2-1/2", or "*" while ongoing.

        Set `claim_draw` to also treat claimable draws as decided.
        """
        if self.is_checkmate():
            return "0-1" if self.turn == WHITE else "1-0"

        if self.is_stalemate():
            return "1/2-1/2"

        if self.is_insufficient_material():
            return "1/2-1/2"

        if self.is_fivefold_repetition():
            return "1/2-1/2"

        if self.is_seventyfive_moves():
            return "1/2-1/2"

        if claim_draw:
            if self.can_claim_draw():
                return "1/2-1/2"

        return "*"

    def move_stack(self) -> List[Move]:
        """Get a list of all moves made from the starting position."""
        return [state.move for state in self._stack]

    def clear_stack(self) -> None:
        """Clear the move stack without changing the position."""
        self._stack.clear()

    def root(self) -> "Board":
        """Get a copy of the board at the root position (before any moves)."""
        board = self.copy()
        while board._stack:
            board.pop()
        return board


class LegalMoveGenerator:
    """
    The legal moves of a position, generated on demand.

    Iterating is lazy, so ``is_checkmate`` and friends can stop at the first
    move, while ``len()`` and ``in`` still work for callers that want them.
    """

    __slots__ = ("_board",)

    def __init__(self, board: Board):
        self._board = board

    def __iter__(self) -> Iterator[Move]:
        return self._board._generate_legal_moves()

    def __len__(self) -> int:
        return sum(1 for _ in self._board._generate_legal_moves())

    def __bool__(self) -> bool:
        for _ in self._board._generate_legal_moves():
            return True
        return False

    def __contains__(self, move: Move) -> bool:
        return any(move == candidate for candidate in self._board._generate_legal_moves())

    def count(self) -> int:
        """Number of legal moves."""
        return len(self)

    def __repr__(self) -> str:
        moves = ", ".join(move.uci() for move in self)
        return f"<LegalMoveGenerator ({moves})>"
