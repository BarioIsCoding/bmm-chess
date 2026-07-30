"""
Mint's Lab Chess Library - UCI Engine Interface
Communication with UCI-compatible chess engines.
"""

import subprocess
import threading
import queue
import time
from typing import Optional, Dict, List, Union, Callable
from dataclasses import dataclass, field

from .board import Board
from .move import Move


@dataclass
class EngineInfo:
    """Information from engine during analysis."""
    depth: int = 0
    seldepth: int = 0
    time: int = 0
    nodes: int = 0
    pv: List[Move] = field(default_factory=list)
    multipv: int = 1
    score_cp: Optional[int] = None  # Score in centipawns
    score_mate: Optional[int] = None  # Mate in N moves
    currmove: Optional[Move] = None
    currmovenumber: int = 0
    hashfull: int = 0
    nps: int = 0
    tbhits: int = 0
    string: str = ""

    @property
    def score(self) -> Optional[int]:
        """Get score in centipawns (mate scores as large values)."""
        if self.score_mate is not None:
            if self.score_mate > 0:
                return 100000 - self.score_mate * 100
            else:
                return -100000 - self.score_mate * 100
        return self.score_cp

    def pv_san(self, board: Board) -> str:
        """Get principal variation as SAN string."""
        return board.variation_san(self.pv)


@dataclass
class BestMove:
    """Result of engine analysis."""
    move: Move
    ponder: Optional[Move] = None
    info: Optional[EngineInfo] = None


@dataclass
class Limit:
    """Search limits for engine analysis."""
    time: Optional[float] = None  # Seconds
    depth: Optional[int] = None
    nodes: Optional[int] = None
    mate: Optional[int] = None
    movetime: Optional[int] = None  # Milliseconds
    white_clock: Optional[float] = None  # Seconds
    black_clock: Optional[float] = None
    white_inc: Optional[float] = None
    black_inc: Optional[float] = None
    moves_to_go: Optional[int] = None


class EngineError(Exception):
    """Exception raised for engine errors."""
    pass


class EngineTerminatedError(EngineError):
    """Exception raised when engine terminates unexpectedly."""
    pass


class SimpleEngine:
    """
    Simple UCI engine wrapper for analysis.

    Usage:
        engine = SimpleEngine.popen_uci("path/to/stockfish")
        result = engine.analyse(board, Limit(depth=20))
        print(result.score)
        engine.quit()
    """

    def __init__(self, process: subprocess.Popen):
        self._process = process
        self._stdin = process.stdin
        self._stdout = process.stdout
        self._id: Dict[str, str] = {}
        self._options: Dict[str, "Option"] = {}
        self._is_ready = False
        self._info: EngineInfo = EngineInfo()
        self._info_callback: Optional[Callable[[EngineInfo], None]] = None

        self._queue: queue.Queue = queue.Queue()
        self._reader_thread = threading.Thread(target=self._read_output, daemon=True)
        self._reader_thread.start()

    @classmethod
    def popen_uci(cls, command: Union[str, List[str]], **popen_args) -> "SimpleEngine":
        """Open a UCI engine as a subprocess; extra kwargs go to subprocess.Popen."""
        if isinstance(command, str):
            command = [command]

        popen_args.setdefault("stdin", subprocess.PIPE)
        popen_args.setdefault("stdout", subprocess.PIPE)
        popen_args.setdefault("stderr", subprocess.DEVNULL)
        popen_args.setdefault("bufsize", 1)
        popen_args.setdefault("universal_newlines", True)

        try:
            process = subprocess.Popen(command, **popen_args)
        except FileNotFoundError:
            raise EngineError(f"Engine not found: {command[0]}")
        except PermissionError:
            raise EngineError(f"Permission denied: {command[0]}")

        engine = cls(process)
        engine._initialize_uci()
        return engine

    def _initialize_uci(self) -> None:
        """Initialize UCI protocol."""
        self._send("uci")

        while True:
            line = self._recv()
            if line is None:
                raise EngineTerminatedError("Engine terminated during initialization")

            if line == "uciok":
                break
            elif line.startswith("id "):
                parts = line[3:].split(" ", 1)
                if len(parts) == 2:
                    self._id[parts[0]] = parts[1]
            elif line.startswith("option "):
                self._parse_option(line[7:])

        self._isready()

    def _isready(self) -> None:
        """Wait for engine to be ready."""
        self._send("isready")
        while True:
            line = self._recv()
            if line is None:
                raise EngineTerminatedError("Engine terminated during isready")
            if line == "readyok":
                self._is_ready = True
                return

    def _send(self, command: str) -> None:
        """Send command to engine."""
        if self._process.poll() is not None:
            raise EngineTerminatedError("Engine has terminated")
        try:
            self._stdin.write(command + "\n")
            self._stdin.flush()
        except BrokenPipeError:
            raise EngineTerminatedError("Engine pipe broken")

    def _recv(self, timeout: Optional[float] = None) -> Optional[str]:
        """Receive line from engine."""
        try:
            line = self._queue.get(timeout=timeout)
            if line is None:
                raise EngineTerminatedError("Engine terminated")
            return line.strip()
        except queue.Empty:
            return None

    def _read_output(self) -> None:
        """Background thread to read engine output."""
        try:
            for line in self._stdout:
                self._queue.put(line)
            self._queue.put(None)  # Signal termination
        except Exception:
            self._queue.put(None)

    def _parse_option(self, line: str) -> None:
        """Parse UCI option line."""
        # Simple parsing - full implementation would handle all option types
        parts = line.split()
        if len(parts) < 2 or parts[0] != "name":
            return

        name_parts = []
        i = 1
        while i < len(parts) and parts[i] != "type":
            name_parts.append(parts[i])
            i += 1

        name = " ".join(name_parts)
        self._options[name] = Option(name=name, type="unknown")

    def configure(self, options: Dict[str, Union[str, int, bool]]) -> None:
        """Configure engine options from a mapping of option name to value."""
        for name, value in options.items():
            if isinstance(value, bool):
                value = "true" if value else "false"
            self._send(f"setoption name {name} value {value}")
        self._isready()

    def analyse(
        self,
        board: Board,
        limit: Limit,
        multipv: int = 1,
        info: Optional[Callable[[EngineInfo], None]] = None
    ) -> EngineInfo:
        """
        Analyse a position.

        Args:
            multipv: Number of principal variations to request
            info: Callback invoked for each info line
        """
        self._info = EngineInfo()
        self._info_callback = info

        self._send(f"position fen {board.fen()}")

        if multipv > 1:
            self._send(f"setoption name MultiPV value {multipv}")

        go_cmd = self._build_go_command(limit, board)
        self._send(go_cmd)

        while True:
            line = self._recv()
            if line is None:
                raise EngineTerminatedError("Engine terminated during analysis")

            if line.startswith("info "):
                self._parse_info(line[5:], board)
                if self._info_callback:
                    self._info_callback(self._info)
            elif line.startswith("bestmove "):
                break

        return self._info

    def play(
        self,
        board: Board,
        limit: Limit,
        ponder: bool = False,
        info: Optional[Callable[[EngineInfo], None]] = None
    ) -> BestMove:
        """
        Let the engine play a move.

        Args:
            ponder: Whether to enable pondering
            info: Callback invoked for each info line
        """
        self._info = EngineInfo()
        self._info_callback = info

        moves_uci = " ".join(m.uci() for m in board.move_stack())
        if moves_uci:
            self._send(f"position fen {board.root().fen()} moves {moves_uci}")
        else:
            self._send(f"position fen {board.fen()}")

        go_cmd = self._build_go_command(limit, board)
        if ponder:
            go_cmd += " ponder"
        self._send(go_cmd)

        best_move = None
        ponder_move = None

        while True:
            line = self._recv()
            if line is None:
                raise EngineTerminatedError("Engine terminated during play")

            if line.startswith("info "):
                self._parse_info(line[5:], board)
                if self._info_callback:
                    self._info_callback(self._info)
            elif line.startswith("bestmove "):
                parts = line.split()
                if len(parts) >= 2:
                    best_move = Move.from_uci(parts[1])
                if len(parts) >= 4 and parts[2] == "ponder":
                    ponder_move = Move.from_uci(parts[3])
                break

        if best_move is None:
            raise EngineError("No best move received")

        return BestMove(move=best_move, ponder=ponder_move, info=self._info)

    def _build_go_command(self, limit: Limit, board: Board) -> str:
        """Build the go command string."""
        cmd = "go"

        if limit.time is not None:
            cmd += f" movetime {int(limit.time * 1000)}"
        if limit.depth is not None:
            cmd += f" depth {limit.depth}"
        if limit.nodes is not None:
            cmd += f" nodes {limit.nodes}"
        if limit.mate is not None:
            cmd += f" mate {limit.mate}"
        if limit.movetime is not None:
            cmd += f" movetime {limit.movetime}"
        if limit.white_clock is not None:
            cmd += f" wtime {int(limit.white_clock * 1000)}"
        if limit.black_clock is not None:
            cmd += f" btime {int(limit.black_clock * 1000)}"
        if limit.white_inc is not None:
            cmd += f" winc {int(limit.white_inc * 1000)}"
        if limit.black_inc is not None:
            cmd += f" binc {int(limit.black_inc * 1000)}"
        if limit.moves_to_go is not None:
            cmd += f" movestogo {limit.moves_to_go}"

        # If no limits specified, use infinite
        if cmd == "go":
            cmd += " infinite"

        return cmd

    def _parse_info(self, line: str, board: Board) -> None:
        """Parse info line from engine."""
        parts = line.split()
        i = 0

        while i < len(parts):
            token = parts[i]
            i += 1

            if token == "depth" and i < len(parts):
                self._info.depth = int(parts[i])
                i += 1
            elif token == "seldepth" and i < len(parts):
                self._info.seldepth = int(parts[i])
                i += 1
            elif token == "time" and i < len(parts):
                self._info.time = int(parts[i])
                i += 1
            elif token == "nodes" and i < len(parts):
                self._info.nodes = int(parts[i])
                i += 1
            elif token == "pv":
                # Rest of line is PV
                pv_moves = []
                temp_board = board.copy()
                while i < len(parts):
                    try:
                        move = Move.from_uci(parts[i])
                        if move in list(temp_board.legal_moves):
                            pv_moves.append(move)
                            temp_board.push(move)
                        else:
                            break
                    except ValueError:
                        break
                    i += 1
                self._info.pv = pv_moves
            elif token == "multipv" and i < len(parts):
                self._info.multipv = int(parts[i])
                i += 1
            elif token == "score":
                while i < len(parts):
                    if parts[i] == "cp" and i + 1 < len(parts):
                        self._info.score_cp = int(parts[i + 1])
                        self._info.score_mate = None
                        i += 2
                    elif parts[i] == "mate" and i + 1 < len(parts):
                        self._info.score_mate = int(parts[i + 1])
                        self._info.score_cp = None
                        i += 2
                    elif parts[i] in ("lowerbound", "upperbound"):
                        i += 1
                    else:
                        break
            elif token == "currmove" and i < len(parts):
                try:
                    self._info.currmove = Move.from_uci(parts[i])
                except ValueError:
                    pass
                i += 1
            elif token == "currmovenumber" and i < len(parts):
                self._info.currmovenumber = int(parts[i])
                i += 1
            elif token == "hashfull" and i < len(parts):
                self._info.hashfull = int(parts[i])
                i += 1
            elif token == "nps" and i < len(parts):
                self._info.nps = int(parts[i])
                i += 1
            elif token == "tbhits" and i < len(parts):
                self._info.tbhits = int(parts[i])
                i += 1
            elif token == "string":
                self._info.string = " ".join(parts[i:])
                break

    def stop(self) -> None:
        """Stop current search."""
        self._send("stop")

    def quit(self) -> None:
        """Quit the engine."""
        try:
            self._send("quit")
        except EngineTerminatedError:
            pass

        self._process.terminate()
        try:
            self._process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._process.kill()

    def close(self) -> None:
        """Alias for quit()."""
        self.quit()

    def __enter__(self) -> "SimpleEngine":
        return self

    def __exit__(self, *args) -> None:
        self.quit()

    @property
    def id(self) -> Dict[str, str]:
        """Engine identification."""
        return self._id

    @property
    def options(self) -> Dict[str, "Option"]:
        """Available engine options."""
        return self._options


@dataclass
class Option:
    """UCI engine option."""
    name: str
    type: str
    default: Optional[str] = None
    min: Optional[int] = None
    max: Optional[int] = None
    var: List[str] = field(default_factory=list)
