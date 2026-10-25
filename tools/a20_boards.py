"""Architecture-owned board directory discovery; no global platform fallback."""
from pathlib import Path


def board_directories(repo_root: Path) -> list[Path]:
    return sorted((repo_root / 'kernel' / 'arch').glob('*/boards/*'), key=lambda p: (p.name, p.parent.parent.name))


def board_directory(repo_root: Path, board: str, arch: str | None = None) -> Path | None:
    if not board or Path(board).name != board:
        return None
    candidates = [p for p in board_directories(repo_root)
                  if p.is_dir() and p.name == board and (arch is None or p.parent.parent.name == arch)]
    return candidates[0] if len(candidates) == 1 else None
