# python/socrates_engine/sensitivity.py
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from .sharder import LayerRange


@dataclass(frozen=True)
class SensitivityScore:
    """Quality loss and confidence for one range/quantization pair."""

    layer_range: LayerRange
    quantization_id: str
    normalized_score: float
    evaluated_samples: int
    confidence: float


class SensitivityProfiler(ABC):
    """Defines offline layer quantization-sensitivity measurement."""

    @abstractmethod
    def profile(
        self,
        model_path: Path,
        calibration_dataset: Path,
        ranges: Sequence[LayerRange],
        quantization_ids: Sequence[str],
    ) -> Sequence[SensitivityScore]:
        """Measure requested combinations without mutating the source model.

        Preconditions:
            Paths are readable, ranges do not overlap and are in bounds, and all
            quantization IDs are supported.
        Postconditions:
            Returns exactly one finite score for every requested combination.
        Raises:
            ValueError for invalid inputs, FileNotFoundError for absent paths,
            RuntimeError for model/calibration failures.
        Thread safety:
            Exclusive per profiler instance.
        Side effects:
            Loads local model/data and consumes bounded compute and memory.
        """
        raise NotImplementedError
