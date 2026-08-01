"""
Self-test and benchmark for bmm_chess.

  python -m bmm_chess.selftest              everything
  python -m bmm_chess.selftest perft [n]    node counts vs the published table
  python -m bmm_chess.selftest sig [depth]  behavioural signature of the game tree
  python -m bmm_chess.selftest invariants   push/pop, FEN, SAN, copy() invariants
  python -m bmm_chess.selftest features     status, pins, EPD, transforms, books
  python -m bmm_chess.selftest bench        micro-benchmarks of the hot primitives
  python -m bmm_chess.selftest compare      head-to-head vs python-chess
  python -m bmm_chess.selftest divide [n]   per-move node counts, for debugging

Run this after any change to _core.c.
"""

import hashlib
import os
import sys
import time

from . import (
    Board, Move, Piece, SquareSet, perft, polyglot,
    WHITE, BLACK, PAWN, QUEEN, KING,
    lsb, msb, popcount, scan_forward, scan_reversed, between, ray,
    square_manhattan_distance, square_knight_distance, flip_vertical,
    InvalidMoveError, IllegalMoveError, AmbiguousMoveError,
    STATUS_EMPTY, STATUS_NO_WHITE_KING, STATUS_NO_BLACK_KING,
    STATUS_BAD_CASTLING_RIGHTS, STATUS_INVALID_EP_SQUARE,
    STATUS_OPPOSITE_CHECK, STATUS_PAWNS_ON_BACKRANK,
)

# Standard perft suite, with published node counts.
PERFT_SUITE = [
    ("startpos",
     "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
     [20, 400, 8902, 197281, 4865609, 119060324]),
    ("kiwipete",
     "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
     [48, 2039, 97862, 4085603, 193690690]),
    ("position 3",
     "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
     [14, 191, 2812, 43238, 674624, 11030083]),
    ("position 4",
     "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
     [6, 264, 9467, 422333, 15833292]),
    ("position 5",
     "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
     [44, 1486, 62379, 2103487, 89941194]),
    ("position 6",
     "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
     [46, 2079, 89890, 3894594, 164075551]),
]

SIGNATURE_FEN = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
SIGNATURE_DEPTH = 3

# Reproduced byte-for-byte by python-chess 1.11.2 over the same tree.
EXPECTED_SIGNATURE = "6c80896c119548bfc4a75ab9727bc5d4"

BENCH_POSITIONS = {
    "start": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "kiwipete": SIGNATURE_FEN,
    "endgame": "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "promotion": "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
}


def _check(condition, label, failures):
    if not condition:
        failures.append(label)
        print("  FAIL  %s" % label)
    return condition


def run_perft(max_depth=None):
    failures = []
    total_nodes = 0
    started = time.perf_counter()

    for name, fen, expected in PERFT_SUITE:
        board = Board(fen)
        depths = expected if max_depth is None else expected[:max_depth]
        for depth, want in enumerate(depths, 1):
            t0 = time.perf_counter()
            got = perft(board, depth)
            elapsed = time.perf_counter() - t0
            total_nodes += got
            rate = ("%7.1f Mnps" % (got / elapsed / 1e6)) if elapsed > 0.02 else ""
            status = "ok" if got == want else "FAIL want %d" % want
            print("  %-11s depth %d  %12d  %-16s %7.3fs %s"
                  % (name, depth, got, status, elapsed, rate))
            if got != want:
                failures.append("perft %s depth %d" % (name, depth))

    elapsed = time.perf_counter() - started
    print("  %d nodes in %.2fs (%.1f Mnps overall)"
          % (total_nodes, elapsed, total_nodes / elapsed / 1e6))
    return failures


def divide(depth=1, fen=None):
    """Per-move node counts, for narrowing down a perft mismatch."""
    board = Board(fen or PERFT_SUITE[0][1])
    total = 0
    for move in sorted(board.legal_moves, key=lambda m: m.uci()):
        board.push(move)
        n = perft(board, depth - 1)
        board.pop()
        total += n
        print("  %-6s %d" % (move.uci(), n))
    print("  total  %d" % total)
    return []


def _signature_walk(board, depth, digest):
    digest.update(board.fen().encode())
    if depth == 0:
        return
    # Sorted, so an independent engine can reproduce the digest.
    for move in sorted(board.legal_moves, key=lambda m: m.uci()):
        digest.update(board.san(move).encode())
        digest.update(move.uci().encode())
        board.push(move)
        _signature_walk(board, depth - 1, digest)
        board.pop()


def signature(depth=SIGNATURE_DEPTH, fen=SIGNATURE_FEN):
    digest = hashlib.md5()
    _signature_walk(Board(fen), depth, digest)
    return digest.hexdigest()


def run_signature(depth=SIGNATURE_DEPTH):
    failures = []
    t0 = time.perf_counter()
    got = signature(depth)
    print("  depth %d signature %s  (%.2fs)" % (depth, got, time.perf_counter() - t0))
    if EXPECTED_SIGNATURE is None:
        print("  no recorded signature; record this value to gate future changes")
    else:
        _check(got == EXPECTED_SIGNATURE,
               "signature %s != %s" % (got, EXPECTED_SIGNATURE), failures)
    return failures


def run_invariants():
    failures = []

    # FEN round-trips.
    for name, fen, _ in PERFT_SUITE:
        board = Board(fen)
        _check(board.fen() == fen, "fen round-trip %s" % name, failures)
        other = Board()
        other.set_fen(board.fen())
        _check(other.fen() == fen, "set_fen round-trip %s" % name, failures)
        _check(board.board_fen() == fen.split()[0], "board_fen %s" % name, failures)

    # push/pop restores the position exactly.
    for name, fen, _ in PERFT_SUITE:
        board = Board(fen)
        before = board.fen()
        for move in list(board.legal_moves):
            board.push(move)
            popped = board.pop()
            _check(popped == move, "pop returns pushed move %s %s" % (name, move.uci()),
                   failures)
            _check(board.fen() == before, "push/pop restores %s %s" % (name, move.uci()),
                   failures)

    # SAN round-trips through parse_san for every legal move.
    for name, fen, _ in PERFT_SUITE:
        board = Board(fen)
        for move in list(board.legal_moves):
            san = board.san(move)
            _check(board.parse_san(san) == move,
                   "san round-trip %s %s (%s)" % (name, move.uci(), san), failures)

    # copy() is independent of its source, with and without the move stack.
    board = Board()
    board.push_san("e4")
    board.push_san("e5")
    clone = board.copy()
    deep = board.copy(stack=False)
    _check(len(clone.move_stack()) == 2, "copy keeps stack", failures)
    _check(len(deep.move_stack()) == 0, "copy(stack=False) drops stack", failures)
    clone.push_san("Nf3")
    _check(board.fen() != clone.fen(), "copy is independent", failures)
    _check(board.root().fen() == Board().fen(), "root returns start", failures)

    # Checkmate, stalemate, and the draw rules.
    mate = Board("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3")
    _check(mate.is_checkmate(), "fool's mate detected", failures)
    _check(mate.is_game_over(), "fool's mate is game over", failures)
    _check(mate.result() == "0-1", "fool's mate result", failures)
    _check(mate.outcome() == "checkmate", "fool's mate outcome", failures)

    stale = Board("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1")
    _check(stale.is_stalemate(), "stalemate detected", failures)
    _check(stale.result() == "1/2-1/2", "stalemate result", failures)

    _check(Board("8/8/8/4k3/8/8/8/4K3 w - - 0 1").is_insufficient_material(),
           "K vs K insufficient", failures)
    _check(Board("8/8/8/4k3/8/8/5N2/4K3 w - - 0 1").is_insufficient_material(),
           "K+N vs K insufficient", failures)
    _check(not Board("8/8/8/4k3/8/8/5R2/4K3 w - - 0 1").is_insufficient_material(),
           "K+R vs K sufficient", failures)

    fifty = Board("8/8/8/4k3/8/8/4R3/4K3 w - - 99 60")
    fifty.push_uci("e2e3")
    _check(fifty.is_fifty_moves(), "fifty-move rule", failures)
    _check(fifty.can_claim_fifty_moves(), "fifty-move claim", failures)
    _check(fifty.can_claim_draw(), "fifty-move draw claim", failures)

    # Repetition needs the move stack, so shuffle knights.
    rep = Board()
    for _ in range(2):
        for uci in ("g1f3", "g8f6", "f3g1", "f6g8"):
            rep.push_uci(uci)
    _check(rep.is_repetition(3), "threefold repetition", failures)
    _check(rep.can_claim_threefold_repetition(), "threefold claim", failures)
    _check(not rep.is_fivefold_repetition(), "not yet fivefold", failures)
    for uci in ("g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1", "f6g8"):
        rep.push_uci(uci)
    _check(rep.is_fivefold_repetition(), "fivefold repetition", failures)
    _check(rep.outcome() == "fivefold_repetition", "fivefold outcome", failures)

    # En passant: available, then gone after any other move.
    ep = Board("rnbqkbnr/pp1ppppp/8/8/2pP4/5N2/PPP1PPPP/RNBQKB1R b KQkq d3 0 3")
    _check(ep.ep_square == 19, "ep_square parsed", failures)
    ep_move = Move.from_uci("c4d3")
    _check(ep_move in ep.legal_moves, "ep capture is legal", failures)
    _check(ep.is_en_passant(ep_move), "ep capture recognised", failures)
    _check(ep.is_capture(ep_move), "ep counts as capture", failures)
    _check(ep.san(ep_move) == "cxd3", "ep SAN", failures)
    ep.push(ep_move)
    _check(ep.piece_at(27) is None, "ep removes the passed pawn", failures)
    _check(ep.ep_square is None, "ep square cleared", failures)

    # Castling, including the rook landing square and losing rights.
    cast = Board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1")
    _check(len(cast.legal_moves) == 26, "castling position move count", failures)
    kingside = Move.from_uci("e1g1")
    _check(cast.is_castling(kingside), "kingside is castling", failures)
    _check(cast.is_kingside_castling(kingside), "kingside flavour", failures)
    _check(cast.san(kingside) == "O-O", "kingside SAN", failures)
    cast.push(kingside)
    _check(cast.piece_at(5) is not None and cast.piece_at(5).piece_type == 4,
           "rook lands on f1", failures)
    _check(not cast.has_kingside_castling_rights(WHITE), "rights dropped", failures)

    # A right without its rook must not conjure one onto the board.
    phantom = Board("4k3/8/8/8/8/8/8/4K3 w K - 0 1")
    _check(Move.from_uci("e1g1") not in phantom.legal_moves,
           "no castling without a rook", failures)

    # Castling out of, through and into check is refused.
    through = Board("4k3/8/8/8/8/8/8/R3K2r w Q - 0 1")
    _check(Move.from_uci("e1c1") not in through.legal_moves,
           "no castling while in check", failures)

    # Promotion generates exactly four options per target square.
    promo = Board("8/4P3/8/8/8/8/8/K6k w - - 0 1")
    promos = [m for m in promo.legal_moves if m.promotion]
    _check(len(promos) == 4, "four promotion options", failures)
    _check(sorted(m.uci() for m in promos) == ["e7e8b", "e7e8n", "e7e8q", "e7e8r"],
           "promotion UCI set", failures)
    _check(promo.san(Move.from_uci("e7e8q")) == "e8=Q", "promotion SAN", failures)

    # A pinned knight is frozen; a pinned rook still slides along the pin.
    pin = Board("k3r3/8/8/8/8/4N3/8/4K3 w - - 0 1")
    _check(all(m.from_square != 20 for m in pin.legal_moves),
           "pinned knight is frozen", failures)
    slide = Board("k3r3/8/8/8/8/4R3/8/4K3 w - - 0 1")
    pinned_moves = sorted(m.uci() for m in slide.legal_moves if m.from_square == 20)
    _check(pinned_moves == ["e3e2", "e3e4", "e3e5", "e3e6", "e3e7", "e3e8"],
           "pinned rook slides along the pin", failures)

    # Null move flips the turn and clears the en passant square.
    null = Board()
    null.push(Move.null())
    _check(null.turn == BLACK, "null move flips turn", failures)
    _check(null.pop() == Move.null(), "null move pops", failures)

    # Move, Piece and SquareSet value semantics.
    _check(Move.from_uci("e2e4") == Move(12, 28), "Move equality", failures)
    _check(hash(Move.from_uci("e7e8q")) == hash(Move(52, 60, QUEEN)), "Move hash", failures)
    _check(not bool(Move.null()), "null move is falsy", failures)
    _check(bool(Move.from_uci("e2e4")), "real move is truthy", failures)
    _check(Move.from_uci("e7e8q").uci() == "e7e8q", "promotion UCI", failures)
    _check(Piece.from_symbol("N") == Piece(2, WHITE), "Piece equality", failures)
    _check(Piece.from_symbol("n").symbol() == "n", "Piece symbol", failures)
    _check(Piece.from_symbol("K").name() == "white king", "Piece name", failures)
    board = Board()
    squares = board.pieces(PAWN, WHITE)
    _check(len(squares) == 8, "eight white pawns", failures)
    _check(list(squares) == list(range(8, 16)), "pawn squares", failures)
    _check(8 in squares and 0 not in squares, "SquareSet containment", failures)
    _check(int(board.attackers(WHITE, 16)) == int(board.attackers_mask(WHITE, 16)),
           "attackers matches mask", failures)

    # Bad input is rejected rather than silently accepted.
    for bad_fen in ("", "8/8/8/8/8/8/8 w - - 0 1", "9/8/8/8/8/8/8/8 w - - 0 1",
                    "8/8/8/8/8/8/8/8 x - - 0 1", "8/8/8/8/8/8/8/8 w Z - 0 1",
                    "8/8/8/8/8/8/8/8 w - z9 0 1", "8/8/8/8/8/8/8/8 w - - x 1"):
        try:
            Board().set_fen(bad_fen)
            failures.append("set_fen accepted %r" % bad_fen)
            print("  FAIL  set_fen accepted %r" % bad_fen)
        except ValueError:
            pass

    guard = Board()
    for bad_san in ("", "Zx9", "e9", "Qd5", "e8=X"):
        try:
            guard.parse_san(bad_san)
            failures.append("parse_san accepted %r" % bad_san)
            print("  FAIL  parse_san accepted %r" % bad_san)
        except ValueError:
            pass

    try:
        Board().pop()
        failures.append("pop on empty stack")
        print("  FAIL  pop on empty stack")
    except IndexError:
        pass

    # A rejected push must not leave a frame on the stack.
    stack_guard = Board()
    try:
        stack_guard.push(Move.from_uci("e3e4"))
    except ValueError:
        pass
    _check(len(stack_guard.move_stack()) == 0, "failed push leaves stack clean", failures)

    if not failures:
        print("  all invariants hold")
    return failures


def run_features():
    """The API beyond move generation: status, pins, EPD, transforms, books."""
    failures = []

    # status() / is_valid()
    _check(Board().is_valid(), "start position is valid", failures)
    _check(Board().status() == 0, "start position status 0", failures)
    for fen, flag, label in [
        ("8/8/8/8/8/8/8/8 w - - 0 1", STATUS_EMPTY, "empty board"),
        ("4k3/8/8/8/8/8/8/8 w - - 0 1", STATUS_NO_WHITE_KING, "no white king"),
        ("8/8/8/8/8/8/8/4K3 w - - 0 1", STATUS_NO_BLACK_KING, "no black king"),
        ("4k3/8/8/8/8/8/8/4K3 w KQkq - 0 1", STATUS_BAD_CASTLING_RIGHTS,
         "castling without rooks"),
        ("4k3/8/8/8/8/8/8/4K3 w - e3 0 1", STATUS_INVALID_EP_SQUARE, "bogus ep"),
        ("4k3/8/8/8/8/8/8/r3K3 b - - 0 1", STATUS_OPPOSITE_CHECK, "opposite check"),
        ("pppppppp/8/8/8/8/8/8/4K1k1 w - - 0 1", STATUS_PAWNS_ON_BACKRANK,
         "pawns on the back rank"),
    ]:
        _check(Board(fen).status() & flag, "status flags %s" % label, failures)
        _check(not Board(fen).is_valid(), "is_valid rejects %s" % label, failures)

    # The nine test vectors published with the PolyGlot format.
    for fen, want in [
        ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
         0x463b96181691fc9c),
        ("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
         0x823c9b50fd114196),
        ("rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 2",
         0x0756b94461c50fb0),
        ("rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR b KQkq - 0 2",
         0x662fafb965db29d4),
        ("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
         0x22a48b5a8e47ff78),
        ("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPPKPPP/RNBQ1BNR b kq - 1 3",
         0x652a607ca3f242c1),
        ("rnbq1bnr/ppp1pkpp/8/3pPp2/8/8/PPPPKPPP/RNBQ1BNR w - - 2 4",
         0x00fdd303c946bdd9),
        ("rnbqkbnr/p1pppppp/8/8/PpP4P/8/1P1PPPP1/RNBQKBNR b KQkq c3 0 3",
         0x3c8123ea7b067637),
        ("rnbqkbnr/p1pppppp/8/8/P6P/R1p5/1P1PPPP1/1NBQKBNR b Kkq - 1 4",
         0x5c3f9b829b279560),
    ]:
        got = Board(fen).zobrist_hash()
        _check(got == want, "zobrist %s (got %#x want %#x)" % (fen, got, want), failures)

    # attacks / pins
    board = Board("k3r3/8/8/8/8/4N3/8/4K3 w - - 0 1")
    _check(board.is_pinned(WHITE, 20), "knight is pinned", failures)
    _check(not board.is_pinned(WHITE, 4), "king is not pinned", failures)
    _check(sorted(Board().attacks(1)) == [11, 16, 18], "knight attacks from b1", failures)
    _check(sorted(Board().attacks(12)) == [19, 21], "pawn attacks from e2", failures)

    # ply, zeroing, irreversible
    board = Board()
    _check(board.ply() == 0, "ply at start", failures)
    board.push_san("e4")
    _check(board.ply() == 1, "ply after one move", failures)
    _check(Board().is_zeroing(Move.from_uci("e2e4")), "pawn move is zeroing", failures)
    _check(not Board().is_zeroing(Move.from_uci("g1f3")), "knight move is not zeroing",
           failures)
    _check(Board("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1").is_irreversible(
        Move.from_uci("e1e2")), "king move is irreversible", failures)

    # en passant availability
    ep = Board("rnbqkbnr/pp1ppppp/8/8/2pP4/5N2/PPP1PPPP/RNBQKB1R b KQkq d3 0 3")
    _check(ep.has_legal_en_passant(), "legal ep available", failures)
    _check(ep.has_pseudo_legal_en_passant(), "pseudo-legal ep available", failures)
    _check(not Board().has_legal_en_passant(), "no ep at start", failures)

    # insufficient material per colour
    kb = Board("8/8/8/4k3/8/8/5B2/4K3 w - - 0 1")
    _check(kb.has_insufficient_material(WHITE), "K+B cannot force mate", failures)
    _check(kb.has_insufficient_material(BLACK), "lone K cannot force mate", failures)
    _check(not Board().has_insufficient_material(WHITE), "start has material", failures)

    # move parsing
    board = Board()
    _check(board.parse_uci("e2e4") == Move.from_uci("e2e4"), "parse_uci", failures)
    _check(board.find_move(12, 28) == Move.from_uci("e2e4"), "find_move", failures)
    promo = Board("8/4P3/8/8/8/8/8/K6k w - - 0 1")
    _check(promo.find_move(52, 60) == Move.from_uci("e7e8q"),
           "find_move defaults to a queen", failures)
    for bad in ("e2e5", "a1a8"):
        try:
            board.parse_uci(bad)
            failures.append("parse_uci accepted %r" % bad)
            print("  FAIL  parse_uci accepted %r" % bad)
        except IllegalMoveError:
            pass
    try:
        board.parse_uci("zz99")
        failures.append("parse_uci accepted 'zz99'")
        print("  FAIL  parse_uci accepted 'zz99'")
    except InvalidMoveError:
        pass
    # The move exceptions must stay ValueError subclasses.
    _check(issubclass(IllegalMoveError, ValueError), "IllegalMoveError is a ValueError",
           failures)
    _check(issubclass(InvalidMoveError, ValueError), "InvalidMoveError is a ValueError",
           failures)
    _check(issubclass(AmbiguousMoveError, ValueError),
           "AmbiguousMoveError is a ValueError", failures)

    # LAN
    _check(Board().lan(Move.from_uci("e2e4")) == "e2-e4", "pawn LAN", failures)
    _check(Board().lan(Move.from_uci("g1f3")) == "Ng1-f3", "knight LAN", failures)

    # EPD round trip
    board = Board()
    epd = board.epd(bm=Move.from_uci("e2e4"), id="start")
    _check(epd.endswith('bm e4; id "start";'), "epd operations: %s" % epd, failures)
    other = Board()
    ops = other.set_epd(epd)
    _check(ops["id"] == "start", "epd id round trip", failures)
    _check(ops["bm"] == [Move.from_uci("e2e4")], "epd bm round trip", failures)
    _check(other.fen() == Board().fen(), "epd position round trip", failures)

    # transforms
    board = Board("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4")
    _check(board.mirror().mirror().fen() == board.fen(), "mirror is its own inverse",
           failures)
    _check(board.mirror().turn != board.turn, "mirror flips the side to move", failures)
    _check(board.transform(flip_vertical).transform(flip_vertical).board_fen() ==
           board.board_fen(), "flip_vertical is its own inverse", failures)

    # board editing
    board = Board()
    board.clear()
    _check(board.fen() == "8/8/8/8/8/8/8/8 w - - 0 1", "clear()", failures)
    board.reset()
    _check(board.fen() == Board().fen(), "reset()", failures)
    _check(Board.empty().fen() == "8/8/8/8/8/8/8/8 w - - 0 1", "Board.empty()", failures)
    board = Board.empty()
    board.set_piece_map({4: Piece(KING, WHITE), 60: Piece(KING, BLACK)})
    _check(board.board_fen() == "4k3/8/8/8/8/8/8/4K3", "set_piece_map", failures)
    board.set_piece_at(28, Piece(QUEEN, WHITE))
    _check(board.piece_at(28) == Piece(QUEEN, WHITE), "set_piece_at", failures)
    _check(board.remove_piece_at(28) == Piece(QUEEN, WHITE), "remove_piece_at", failures)
    _check(board.piece_at(28) is None, "square emptied", failures)

    # filtered generation
    board = Board()
    from_e2 = [m.uci() for m in board.generate_legal_moves(from_mask=1 << 12)]
    _check(sorted(from_e2) == ["e2e3", "e2e4"], "generate_legal_moves(from_mask)",
           failures)
    _check(len(list(board.generate_legal_captures())) == 0, "no captures at start",
           failures)
    _check(len(list(ep.generate_legal_ep())) == 1, "one legal ep capture", failures)
    _check(len(board.pseudo_legal_moves) == 20, "pseudo_legal_moves at start", failures)

    # SquareSet
    squares = SquareSet([0, 1, 2])
    squares.add(63)
    squares.discard(0)
    _check(sorted(squares) == [1, 2, 63], "SquareSet add/discard", failures)
    _check(squares.pop() == 1, "SquareSet pop takes the lowest", failures)
    _check(SquareSet(0xFF).issuperset(SquareSet(0x0F)), "SquareSet issuperset", failures)
    _check(SquareSet(0xFF).isdisjoint(SquareSet(0xFF00)), "SquareSet isdisjoint",
           failures)
    _check(len(list(SquareSet(0b101).carry_rippler())) == 4, "carry_rippler subsets",
           failures)
    _check(SquareSet(0xFF).mirror() == SquareSet(0xFF00000000000000),
           "SquareSet mirror", failures)
    _check(list(reversed(SquareSet(0b1011))) == [3, 1, 0], "SquareSet reversed", failures)

    # bit helpers
    _check(lsb(0b1000) == 3 and msb(0b11000) == 4, "lsb/msb", failures)
    _check(popcount(0b1011) == 3, "popcount", failures)
    _check(list(scan_forward(0b1010)) == [1, 3], "scan_forward", failures)
    _check(list(scan_reversed(0b1010)) == [3, 1], "scan_reversed", failures)
    _check(sorted(between(0, 3)) == [1, 2], "between", failures)
    _check(sorted(ray(0, 3)) == [0, 1, 2, 3, 4, 5, 6, 7], "ray", failures)
    _check(square_manhattan_distance(0, 9) == 2, "manhattan distance", failures)
    _check(square_knight_distance(0, 9) == 4, "knight distance", failures)

    # PolyGlot books, when the repository ships one
    book_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "books", "chess", "Rodent.bin")
    if os.path.exists(book_path):
        with polyglot.open_reader(book_path) as book:
            entries = list(book.find_all(Board()))
            _check(len(entries) > 0, "book has entries for the start position", failures)
            _check(all(e.move in Board().legal_moves for e in entries),
                   "book moves are legal", failures)
            _check(book.find(Board()).weight ==
                   max(e.weight for e in entries), "find() takes the best entry",
                   failures)
    else:
        print("  (no PolyGlot book present; skipped book checks)")

    if not failures:
        print("  all feature checks pass")
    return failures


def run_compare():
    """Same operations, bmm_chess vs python-chess, reported as calls per second."""
    try:
        import chess
        import chess.polyglot
    except ImportError:
        print("  python-chess is not installed; `pip install chess` to run this")
        return []

    import platform

    # platform.platform() cannot tell Windows 11 from 10, so report only what
    # it gets right and let the reader supply the CPU.
    print("  %s %s, CPython %s, python-chess %s"
          % (platform.system(), platform.machine(),
             platform.python_version(), chess.__version__))
    print()
    print("  | operation | bmm_chess | python-chess | |")
    print("  |---|---|---|---|")

    def rate(fn, iterations):
        fn()
        best = None
        for _ in range(3):
            t0 = time.perf_counter()
            fn()
            elapsed = time.perf_counter() - t0
            best = elapsed if best is None else min(best, elapsed)
        return iterations / best

    def row(label, mine, theirs, iterations):
        a, b = rate(mine, iterations), rate(theirs, iterations)
        print("  | `%s` | %s | %s | **%.0fx** |"
              % (label, _ops(a), _ops(b), a / b))

    fen = SIGNATURE_FEN
    gb, rb = Board(fen), chess.Board(fen)
    gm, rm = list(gb.legal_moves), list(rb.legal_moves)
    n = len(gm)

    row("len(board.legal_moves)", lambda: [len(gb.legal_moves) for _ in range(3000)],
        lambda: [rb.legal_moves.count() for _ in range(3000)], 3000)
    row("board.fen()", lambda: [gb.fen() for _ in range(20000)],
        lambda: [rb.fen() for _ in range(20000)], 20000)
    row("zobrist_hash(board)", lambda: [gb.zobrist_hash() for _ in range(20000)],
        lambda: [chess.polyglot.zobrist_hash(rb) for _ in range(20000)], 20000)
    row("Board(fen)", lambda: [Board(fen) for _ in range(20000)],
        lambda: [chess.Board(fen) for _ in range(20000)], 20000)
    row("board.status()", lambda: [gb.status() for _ in range(20000)],
        lambda: [rb.status() for _ in range(20000)], 20000)
    row("push() + pop()",
        lambda: [(gb.push(m), gb.pop()) for _ in range(300) for m in gm],
        lambda: [(rb.push(m), rb.pop()) for _ in range(300) for m in rm], 300 * n)
    row("list(board.legal_moves)", lambda: [list(gb.legal_moves) for _ in range(3000)],
        lambda: [list(rb.legal_moves) for _ in range(3000)], 3000)
    row("board.san(move)", lambda: [gb.san(m) for _ in range(300) for m in gm],
        lambda: [rb.san(m) for _ in range(300) for m in rm], 300 * n)
    row("board.piece_map()", lambda: [gb.piece_map() for _ in range(20000)],
        lambda: [rb.piece_map() for _ in range(20000)], 20000)

    def py_perft(board, depth):
        if depth <= 1:
            return sum(1 for _ in board.legal_moves)
        total = 0
        for move in board.legal_moves:
            board.push(move)
            total += py_perft(board, depth - 1)
            board.pop()
        return total

    # Both Python-loop rows call into their library for every operation; only the
    # loop itself is interpreted. Nothing here has a pure-Python chess path.
    print()
    for label, factory in (("bmm_chess", Board), ("python-chess", chess.Board)):
        t0 = time.perf_counter()
        nodes = py_perft(factory(), 4)
        elapsed = time.perf_counter() - t0
        print("  perft, Python loop over %-13s %s nodes/sec"
              % (label, _ops(nodes / elapsed)))

    t0 = time.perf_counter()
    nodes = perft(Board(), 5)
    print("  perft, whole walk in C  %-13s %s nodes/sec"
          % ("bmm_chess", _ops(nodes / (time.perf_counter() - t0))))
    return []


def _ops(v):
    if v >= 1e6:
        return "%.1fM/s" % (v / 1e6)
    if v >= 1e3:
        return "%.0fk/s" % (v / 1e3)
    return "%.0f/s" % v


def run_bench():
    def timed(label, fn, iterations):
        fn()  # warm
        best = None
        for _ in range(3):
            t0 = time.perf_counter()
            fn()
            elapsed = time.perf_counter() - t0
            best = elapsed if best is None else min(best, elapsed)
        print("  %-34s %8.3f us/op" % (label, best / iterations * 1e6))

    boards = {name: Board(fen) for name, fen in BENCH_POSITIONS.items()}
    kiwi = boards["kiwipete"]
    moves = list(kiwi.legal_moves)

    timed("legal move generation", lambda: [list(b.legal_moves) for b in boards.values()],
          len(boards))
    timed("legal move count", lambda: [len(b.legal_moves) for b in boards.values()],
          len(boards))
    timed("push + pop", lambda: [(kiwi.push(m), kiwi.pop()) for m in moves], len(moves))
    timed("san()", lambda: [kiwi.san(m) for m in moves], len(moves))
    timed("parse_san()", lambda: [kiwi.parse_san(s) for s in [kiwi.san(m) for m in moves]],
          len(moves))
    timed("fen()", lambda: [b.fen() for b in boards.values()], len(boards))
    timed("set_fen()", lambda: [boards["start"].set_fen(f) for f in BENCH_POSITIONS.values()],
          len(BENCH_POSITIONS))
    timed("piece_map()", lambda: [b.piece_map() for b in boards.values()], len(boards))
    timed("is_attacked_by() x64", lambda: [kiwi.is_attacked_by(WHITE, s) for s in range(64)], 64)

    t0 = time.perf_counter()
    nodes = perft(Board(), 5)
    elapsed = time.perf_counter() - t0
    print("  %-34s %8.1f Mnps" % ("native perft", nodes / elapsed / 1e6))
    return []


def main(argv):
    command = argv[1] if len(argv) > 1 else "all"
    arg = argv[2] if len(argv) > 2 else None
    failures = []

    if command in ("all", "perft"):
        print("perft:")
        failures += run_perft(int(arg) if arg else None)
    if command in ("all", "sig"):
        print("signature:")
        failures += run_signature(int(arg) if arg else SIGNATURE_DEPTH)
    if command in ("all", "invariants"):
        print("invariants:")
        failures += run_invariants()
    if command in ("all", "features"):
        print("features:")
        failures += run_features()
    if command == "compare":
        print("comparison:")
        failures += run_compare()
    if command in ("all", "bench"):
        print("benchmark:")
        failures += run_bench()
    if command == "divide":
        print("divide:")
        failures += divide(int(arg) if arg else 1)
    if command not in ("all", "perft", "sig", "invariants", "features", "bench",
                       "compare", "divide"):
        print(__doc__)
        return 2

    print()
    if failures:
        print("FAILED: %d" % len(failures))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
