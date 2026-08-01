"""
Mint's Lab Chess Library - PGN Module
Portable Game Notation parsing and generation.
"""

from typing import Optional, Dict, List, Iterator, TextIO, Union
from io import StringIO
import logging
import re

from ._core import tokenize_movetext as _tokenize_movetext
from .board import Board
from .move import Move
from .constants import WHITE

logger = logging.getLogger(__name__)

RESULT_TOKENS = ("1-0", "0-1", "1/2-1/2", "*")

# Compiled once: _parse_movetext matches every token against these.
MOVE_NUMBER_PATTERN = re.compile(r'^\d+\.+$')
RESULT_PATTERN = re.compile(r'\s*(1-0|0-1|1/2-1/2|\*)\s*$')
NAG_MAP = {"!": 1, "?": 2, "!!": 3, "??": 4, "!?": 5, "?!": 6}


class Headers(dict):
    """
    PGN game headers as a dictionary.

    Standard headers: Event, Site, Date, Round, White, Black, Result
    """

    def __init__(self, **kwargs):
        super().__init__()
        self["Event"] = kwargs.get("Event", "?")
        self["Site"] = kwargs.get("Site", "?")
        self["Date"] = kwargs.get("Date", "????.??.??")
        self["Round"] = kwargs.get("Round", "?")
        self["White"] = kwargs.get("White", "?")
        self["Black"] = kwargs.get("Black", "?")
        self["Result"] = kwargs.get("Result", "*")

        for key, value in kwargs.items():
            if key not in self:
                self[key] = value


class GameNode:
    """
    A node in the game tree, representing a position after a move.
    Supports variations (alternative lines).
    """

    def __init__(self, parent: Optional["GameNode"] = None, move: Optional[Move] = None):
        self.parent = parent
        self.move = move
        self.variations: List["GameNode"] = []
        self.comment: str = ""
        self.nags: List[int] = []  # Numeric Annotation Glyphs
        self._board: Optional[Board] = None

    def add_variation(self, move: Move) -> "GameNode":
        """Add a variation (alternative move) from this position."""
        node = GameNode(parent=self, move=move)
        self.variations.append(node)
        return node

    def add_main_variation(self, move: Move) -> "GameNode":
        """Add a move as the main continuation."""
        if self.variations:
            node = GameNode(parent=self, move=move)
            self.variations.insert(0, node)
        else:
            node = self.add_variation(move)
        return node

    def is_main_variation(self) -> bool:
        """Check if this is the main line."""
        if self.parent is None:
            return True
        if not self.parent.variations:
            return True
        return self.parent.variations[0] is self

    def main_line(self) -> Iterator["GameNode"]:
        """Iterate through the main line from this node."""
        node = self
        while node.variations:
            node = node.variations[0]
            yield node

    def board(self) -> Board:
        """Get the board position at this node."""
        if self._board is not None:
            return self._board.copy()

        # Build position by walking back to root
        moves = []
        node = self
        while node.parent is not None:
            if node.move:
                moves.append(node.move)
            node = node.parent

        if hasattr(node, '_start_board') and node._start_board is not None:
            board = node._start_board.copy()
        else:
            board = Board()

        for move in reversed(moves):
            board.push(move)

        return board

    def san(self) -> str:
        """Get the SAN of the move leading to this node."""
        if self.move is None or self.parent is None:
            return ""
        parent_board = self.parent.board()
        return parent_board.san(self.move)

    def __iter__(self) -> Iterator["GameNode"]:
        """Iterate through variations."""
        return iter(self.variations)

    def __bool__(self) -> bool:
        return True


class Game(GameNode):
    """
    Represents a complete chess game with headers and moves.
    """

    def __init__(self, headers: Optional[Dict[str, str]] = None):
        super().__init__()
        self.headers = Headers(**(headers or {}))
        self._start_board: Board = Board()
        # Tokens that could not be parsed while reading this game. Empty for a
        # clean read; inspect it to find out what a lenient parse skipped.
        self.errors: List[str] = []

    @classmethod
    def from_board(cls, board: Board) -> "Game":
        """Create a game from a board with its move history."""
        game = cls()

        # Set up starting position if not standard
        root_board = board.root()
        if root_board.fen() != Board().fen():
            game.headers["FEN"] = root_board.fen()
            game.headers["SetUp"] = "1"
            game._start_board = root_board

        node = game
        for move in board.move_stack():
            node = node.add_main_variation(move)

        game.headers["Result"] = board.result()

        return game

    def setup(self, fen: str) -> None:
        """Set up a custom starting position."""
        self._start_board = Board(fen)
        self.headers["FEN"] = fen
        self.headers["SetUp"] = "1"

    def board(self) -> Board:
        """Get the starting position board."""
        return self._start_board.copy()

    def end(self) -> GameNode:
        """Get the final node of the main line."""
        node = self
        while node.variations:
            node = node.variations[0]
        return node

    def mainline_moves(self) -> Iterator[Move]:
        """Iterate through main line moves."""
        for node in self.main_line():
            if node.move:
                yield node.move

    def mainline(self) -> Iterator[GameNode]:
        """Iterate through main line nodes."""
        return self.main_line()

    def accept(self, visitor: "BaseVisitor") -> None:
        """Accept a visitor for traversing the game tree."""
        visitor.begin_game()
        visitor.visit_headers(self.headers)

        board = self.board()
        self._accept_node(self, visitor, board)

        visitor.visit_result(self.headers.get("Result", "*"))
        visitor.end_game()

    def _accept_node(self, node: GameNode, visitor: "BaseVisitor", board: Board) -> None:
        """
        Visit the continuations of `node`: its main line, with sidelines inline.

        Sidelines branch from the position *before* the main move, so they are
        written directly after it and before the main line continues -- which
        is both what PGN means by "(...)" and what makes the move numbers
        inside the parentheses come out right.
        """
        if not node.variations:
            return

        main = node.variations[0]
        self._visit_move_node(main, visitor, board)

        sidelines = node.variations[1:]
        if sidelines:
            if main.move:
                board.pop()
            for variation in sidelines:
                visitor.begin_variation()
                self._visit_move_node(variation, visitor, board)
                self._accept_node(variation, visitor, board)
                if variation.move:
                    board.pop()
                visitor.end_variation()
            if main.move:
                board.push(main.move)

        self._accept_node(main, visitor, board)
        if main.move:
            board.pop()

    def _visit_move_node(self, node: GameNode, visitor: "BaseVisitor", board: Board) -> None:
        """Emit a node's move, comment and NAGs, leaving its move on the board."""
        if node.move:
            visitor.visit_move(board, node.move)
            board.push(node.move)

        if node.comment:
            visitor.visit_comment(node.comment)

        for nag in node.nags:
            visitor.visit_nag(nag)

    def __str__(self) -> str:
        """Export game as PGN string."""
        exporter = StringExporter()
        self.accept(exporter)
        return str(exporter)


class BaseVisitor:
    """Base class for game tree visitors."""

    def begin_game(self) -> None:
        pass

    def end_game(self) -> None:
        pass

    def visit_headers(self, headers: Headers) -> None:
        pass

    def visit_move(self, board: Board, move: Move) -> None:
        pass

    def visit_comment(self, comment: str) -> None:
        pass

    def visit_nag(self, nag: int) -> None:
        pass

    def visit_result(self, result: str) -> None:
        pass

    def begin_variation(self) -> None:
        pass

    def end_variation(self) -> None:
        pass


class StringExporter(BaseVisitor):
    """Export a game to PGN string format."""

    NAG_SYMBOLS = {
        1: "!",    # Good move
        2: "?",    # Mistake
        3: "!!",   # Brilliant move
        4: "??",   # Blunder
        5: "!?",   # Interesting move
        6: "?!",   # Dubious move
    }

    def __init__(self, columns: int = 80, headers: bool = True,
                 variations: bool = True, comments: bool = True):
        self.columns = columns
        self.include_headers = headers
        self.include_variations = variations
        self.include_comments = comments
        self._lines: List[str] = []
        self._current_line = ""
        self._after_variation = False
        self._needs_move_number = True
        self._board: Optional[Board] = None
        self._in_variation = False
        self._variation_depth = 0
        self._suppress_space = False

    def begin_game(self) -> None:
        self._lines = []
        self._current_line = ""
        self._after_variation = False
        self._needs_move_number = True
        self._in_variation = False
        self._variation_depth = 0
        self._suppress_space = False

    def visit_headers(self, headers: Headers) -> None:
        if self.include_headers:
            # Standard seven-tag roster first
            standard_tags = ["Event", "Site", "Date", "Round", "White", "Black", "Result"]
            for tag in standard_tags:
                value = headers.get(tag, "?")
                self._lines.append(f'[{tag} "{value}"]')

            for tag, value in headers.items():
                if tag not in standard_tags:
                    self._lines.append(f'[{tag} "{value}"]')

            self._lines.append("")  # Blank line after headers

        if "FEN" in headers:
            self._board = Board(headers["FEN"])
        else:
            self._board = Board()

        # A game resumed from a FEN with Black to move must lead with "N...".
        self._needs_move_number = True

    def visit_move(self, board: Board, move: Move) -> None:
        if self._in_variation and not self.include_variations:
            return

        san = board.san(move)

        # Take the move number from the position rather than a running counter:
        # a sideline rewinds the board, and a counter would keep moving forward.
        number = board.fullmove_number

        if board.turn == WHITE:
            move_text = f"{number}. {san}"
        elif self._needs_move_number:
            move_text = f"{number}... {san}"
        else:
            move_text = san

        self._write_token(move_text)

        self._needs_move_number = False
        self._after_variation = False

    def visit_comment(self, comment: str) -> None:
        if self.include_comments:
            self._write_token("{" + comment + "}")

    def visit_nag(self, nag: int) -> None:
        if nag in self.NAG_SYMBOLS:
            # Append to previous token
            if self._current_line:
                self._current_line = self._current_line.rstrip() + self.NAG_SYMBOLS[nag] + " "
        else:
            self._write_token(f"${nag}")

    def visit_result(self, result: str) -> None:
        self._write_token(result)
        self._flush_line()

    def begin_variation(self) -> None:
        self._variation_depth += 1
        self._in_variation = True
        if self.include_variations:
            self._write_token("(")
            self._suppress_space = True
            self._needs_move_number = True

    def end_variation(self) -> None:
        if self.include_variations:
            self._write_token(")", separator="")
            self._after_variation = True
            self._needs_move_number = True
        self._variation_depth = max(0, self._variation_depth - 1)
        self._in_variation = self._variation_depth > 0

    def _write_token(self, token: str, separator: str = " ") -> None:
        """Write a token to the output."""
        # An opening paren binds tight to the move that follows it.
        if self._suppress_space:
            separator = ""
            self._suppress_space = False

        if (self._current_line and
                len(self._current_line) + len(token) + len(separator) > self.columns):
            self._flush_line()

        if self._current_line:
            self._current_line += separator + token
        else:
            self._current_line = token

    def _flush_line(self) -> None:
        """Flush current line to output."""
        if self._current_line:
            self._lines.append(self._current_line)
            self._current_line = ""

    def __str__(self) -> str:
        lines = self._lines.copy()
        if self._current_line:
            lines.append(self._current_line)
        return "\n".join(lines)


HEADER_PATTERN = re.compile(r'\[(\w+)\s+"([^"]*)"\]')


def read_game(handle: Union[TextIO, str], strict: bool = False) -> Optional[Game]:
    """
    Read a single game from a PGN file or string.

    Args:
        handle: File handle or PGN string
        strict: Raise ValueError on the first unparseable token instead of
            skipping it. When False (the default), skipped tokens are recorded
            on ``game.errors`` and logged as warnings.

    Returns None when the handle holds no further game.
    """
    if isinstance(handle, str):
        handle = StringIO(handle)

    # Skip leading whitespace and empty lines
    while True:
        line = handle.readline()
        if not line:
            return None
        line = line.strip()
        if line:
            break

    headers = {}
    while line.startswith("["):
        match = HEADER_PATTERN.match(line)
        if match:
            headers[match.group(1)] = match.group(2)
        line = handle.readline()
        if not line:
            break
        line = line.strip()

    game = Game(headers)

    if "FEN" in headers:
        game.setup(headers["FEN"])

    # Parse moves - continue reading until EOF or the next game's header block.
    # The header loop above stops on the first line that is not a tag pair, so
    # `line` is already the start of the movetext.
    movetext = " " + line if line else ""
    while True:
        position = _tell(handle)
        line = handle.readline()
        if not line:
            break
        stripped = line.strip()
        # A tag pair on its own line starts the next game. Hand it back so the
        # following read_game() call still sees its first header. Matching the
        # full tag syntax (not just "[") keeps wrapped {[%clk ...]} comments
        # from being mistaken for a game boundary.
        if HEADER_PATTERN.match(stripped):
            _unread(handle, line, position)
            break
        if stripped:
            movetext += " " + stripped

    _parse_movetext(game, movetext, strict=strict)

    return game


def _tell(handle: TextIO) -> Optional[int]:
    """Current offset, or None for streams that cannot be seeked."""
    try:
        return handle.tell()
    except (OSError, ValueError, AttributeError):
        return None


def _unread(handle: TextIO, line: str, position: Optional[int] = None) -> None:
    """
    Push a line back so the next read sees it.

    Without this the first tag pair of every game after the first is consumed
    while detecting the game boundary, and silently lost.
    """
    if position is not None:
        try:
            handle.seek(position)
            return
        except (OSError, ValueError, AttributeError):
            pass
    logger.debug("Cannot rewind PGN stream; header %r may be dropped", line.strip())


def _parse_movetext(game: Game, movetext: str, strict: bool = False) -> None:
    """Parse movetext and add moves to game."""
    # Take the result off the end, and record it. The movetext is the
    # authoritative source when the tag pair is missing or still a placeholder.
    result_match = RESULT_PATTERN.search(movetext)
    if result_match:
        movetext = movetext[:result_match.start()]
        result = result_match.group(1)
        if game.headers.get("Result", "*") == "*":
            game.headers["Result"] = result

    tokens = _tokenize_movetext(movetext)

    board = game.board()
    node = game
    variation_stack = []

    i = 0
    while i < len(tokens):
        token = tokens[i]
        i += 1

        # Skip move numbers
        if MOVE_NUMBER_PATTERN.match(token):
            continue

        if token.startswith("{"):
            comment = token[1:]
            while not comment.endswith("}") and i < len(tokens):
                comment += " " + tokens[i]
                i += 1
            node.comment = comment.rstrip("}")
            continue

        if token.startswith("$"):
            try:
                nag = int(token[1:])
                node.nags.append(nag)
            except ValueError:
                pass
            continue

        if token == "(":
            variation_stack.append((node, board.copy()))
            # Go back one move
            if node.parent:
                board.pop()
                node = node.parent
            continue

        if token == ")":
            if variation_stack:
                node, board = variation_stack.pop()
            continue

        if token in NAG_MAP:
            node.nags.append(NAG_MAP[token])
            continue

        # A stray result token inside the movetext ends this game's moves.
        if token in RESULT_TOKENS:
            continue

        try:
            move = board.parse_san(token)
        except ValueError as exc:
            # Skipping a move silently desynchronises the board from the file
            # and every later move is then parsed against the wrong position,
            # so make the failure visible rather than swallowing it.
            message = f"could not parse {token!r}: {exc}"
            if strict:
                raise ValueError(f"Invalid PGN movetext: {message}") from exc
            game.errors.append(message)
            logger.warning("PGN: %s", message)
            continue

        # Append rather than insert: inside parentheses this is a sideline and
        # must not displace the move already recorded as the main line.
        node = node.add_variation(move)
        board.push(move)


def scan_headers(handle: TextIO) -> Iterator[Headers]:
    """
    Scan a PGN file and yield game headers without parsing moves.
    Much faster for indexing large PGN files.
    """
    headers = {}

    for line in handle:
        line = line.strip()

        if line.startswith("["):
            match = HEADER_PATTERN.match(line)
            if match:
                headers[match.group(1)] = match.group(2)
        elif headers:
            # End of headers section
            yield Headers(**headers)
            headers = {}

    # Yield last game if file doesn't end with blank line
    if headers:
        yield Headers(**headers)
