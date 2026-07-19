"""
Low-level ctypes wrapper around the stable Socrates C ABI (libsocrates).
"""

import ctypes
import os
import platform
from ctypes import (
    POINTER,
    Structure,
    byref,
    c_bool,
    c_char_p,
    c_float,
    c_int32,
    c_uint32,
    c_uint64,
    c_void_p,
)

# ── C type definitions ──────────────────────────────────────────────────────

SOCRATES_API_VERSION = 1


class SocratesError(Structure):
    _fields_ = [
        ("code", c_int32),
        ("message", c_char_p),
    ]


class SocratesStreamEvent(Structure):
    _fields_ = [
        ("request_id", c_char_p),
        ("kind", c_int32),
        ("sequence", c_uint64),
        ("text_delta", c_char_p),
        ("token_id", c_int32),
    ]


class SocratesConfig(Structure):
    _fields_ = [
        ("version", c_uint32),
        ("state_directory", c_char_p),
        ("model_root", c_char_p),
        ("cluster_id", c_char_p),
        ("trust_mode", c_int32),
        ("recovery_token_retention", c_uint32),
        ("tracing_enabled", c_bool),
    ]


# Callback type
StreamCallback = ctypes.CFUNCTYPE(
    None, POINTER(SocratesStreamEvent), c_void_p
)

# ── Library loading ─────────────────────────────────────────────────────────


def _find_library() -> str:
    """Locate the libsocrates shared library for the current platform."""
    system = platform.system()
    machine = platform.machine().lower()

    # Search paths in priority order
    candidates = []

    if system == "Darwin":
        candidates = [
            "libsocrates.dylib",
            os.path.join(os.path.dirname(__file__), "..", "..", "build",
                         "macos-debug", "libsocrates.dylib"),
            "/usr/local/lib/libsocrates.dylib",
        ]
    elif system == "Linux":
        candidates = [
            "libsocrates.so",
            os.path.join(os.path.dirname(__file__), "..", "..", "build",
                         "linux-debug", "libsocrates.so"),
            "/usr/local/lib/libsocrates.so",
        ]
    elif system == "Windows":
        candidates = [
            "socrates.dll",
            os.path.join(os.path.dirname(__file__), "..", "..", "build",
                         "windows-debug", "socrates.dll"),
        ]

    for cand in candidates:
        if os.path.exists(cand) or not os.path.isabs(cand):
            return cand

    raise RuntimeError(
        "Cannot find libsocrates. Build the project first with:\n"
        "  cmake --build --preset macos-debug\n"
        "Then set SOCRATES_LIB_PATH to the .dylib/.so/.dll location."
    )


_lib = None


def _get_lib():
    global _lib
    if _lib is None:
        lib_path = os.environ.get("SOCRATES_LIB_PATH", _find_library())
        _lib = ctypes.cdll.LoadLibrary(lib_path)

        # Set function signatures
        _lib.socrates_init.argtypes = [SocratesConfig, POINTER(c_void_p)]
        _lib.socrates_init.restype = SocratesError

        _lib.socrates_start.argtypes = [c_void_p]
        _lib.socrates_start.restype = SocratesError

        _lib.socrates_generate.argtypes = [
            c_void_p, c_char_p, c_char_p, c_uint32, c_float,
            StreamCallback, c_void_p, POINTER(c_void_p),
        ]
        _lib.socrates_generate.restype = SocratesError

        _lib.socrates_cancel.argtypes = [c_void_p]
        _lib.socrates_cancel.restype = SocratesError

        _lib.socrates_stop.argtypes = [c_void_p]
        _lib.socrates_stop.restype = SocratesError

        _lib.socrates_shutdown.argtypes = [c_void_p]
        _lib.socrates_shutdown.restype = SocratesError

        _lib.socrates_version.argtypes = []
        _lib.socrates_version.restype = c_char_p
    return _lib


# ── High-level Python class ─────────────────────────────────────────────────


class Runtime:
    """High-level Python wrapper around the Socrates Edge AI Runtime.

    Usage::

        rt = Runtime(
            state_directory="/tmp/socrates",
            model_root="./models",
            cluster_id="my-cluster",
        )
        rt.start()

        def on_token(text: str, token_id: int, sequence: int):
            print(text, end="", flush=True)

        handle = rt.generate("Hello, world!", max_tokens=50, callback=on_token)
        handle.wait()
        rt.stop()
    """

    def __init__(
        self,
        state_directory: str = "/tmp/socrates-state",
        model_root: str = "/tmp/socrates-models",
        cluster_id: str = "default-cluster",
        tracing_enabled: bool = False,
    ):
        self._lib = _get_lib()
        self._runtime = c_void_p()
        self._state_dir = state_directory
        self._model_root = model_root
        self._cluster_id = cluster_id
        self._tracing = tracing_enabled
        self._started = False
        self._active_handles: dict = {}

        cfg = SocratesConfig()
        cfg.version = SOCRATES_API_VERSION
        cfg.state_directory = state_directory.encode("utf-8")
        cfg.model_root = model_root.encode("utf-8")
        cfg.cluster_id = cluster_id.encode("utf-8")
        cfg.trust_mode = 0  # ephemeral
        cfg.recovery_token_retention = 0
        cfg.tracing_enabled = tracing_enabled

        err = self._lib.socrates_init(cfg, byref(self._runtime))
        if err.code != 0:
            msg = err.message.decode() if err.message else "unknown error"
            raise RuntimeError(f"socrates_init failed: {msg}")

    def start(self) -> None:
        """Start the runtime: begin discovery, profiling, and cluster join."""
        err = self._lib.socrates_start(self._runtime)
        if err.code != 0:
            msg = err.message.decode() if err.message else "unknown error"
            raise RuntimeError(f"socrates_start failed: {msg}")
        self._started = True

    def generate(
        self,
        prompt: str,
        *,
        max_tokens: int = 256,
        temperature: float = 0.7,
        request_id: str | None = None,
    ) -> "GenerationHandle":
        """Submit a generation request. Returns a handle for streaming/cancellation."""
        import uuid

        rid = request_id or str(uuid.uuid4())[:8]

        handle_ptr = c_void_p()

        # We use a simple callback wrapper (no Python-callback from C thread in
        # production — JSI/trampoline needed). For now, this is async fire-and-forget.
        @StreamCallback
        def _stream_cb(event, _user_data):
            pass  # Tokens are logged by the native runtime

        err = self._lib.socrates_generate(
            self._runtime,
            rid.encode("utf-8"),
            prompt.encode("utf-8"),
            max_tokens,
            c_float(temperature),
            _stream_cb,
            None,
            byref(handle_ptr),
        )
        if err.code != 0:
            msg = err.message.decode() if err.message else "unknown error"
            raise RuntimeError(f"socrates_generate failed: {msg}")

        handle = GenerationHandle(self._lib, handle_ptr, rid)
        self._active_handles[rid] = handle
        return handle

    def cancel(self, request_id: str) -> None:
        """Cancel an active generation by request ID."""
        if request_id in self._active_handles:
            self._active_handles[request_id].cancel()
            del self._active_handles[request_id]

    def stop(self) -> None:
        """Gracefully stop the runtime: leave cluster, drain work, release resources."""
        if self._started:
            self._lib.socrates_stop(self._runtime)
            self._started = False

    def shutdown(self) -> None:
        """Stop and destroy the runtime, releasing all native resources."""
        self.stop()
        self._lib.socrates_shutdown(self._runtime)
        self._active_handles.clear()

    @property
    def version(self) -> str:
        """Socrates engine version string."""
        v = self._lib.socrates_version()
        return v.decode() if v else "unknown"

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *args):
        self.shutdown()


class GenerationHandle:
    """Handle to an active generation. Supports cancellation."""

    def __init__(self, lib, handle_ptr, request_id: str):
        self._lib = lib
        self._handle = handle_ptr
        self.request_id = request_id
        self._cancelled = False

    def cancel(self) -> None:
        """Request cooperative cancellation of the generation."""
        if not self._cancelled and self._handle:
            self._lib.socrates_cancel(self._handle)
            self._cancelled = True

    def wait(self, timeout_seconds: float = 30.0) -> None:
        """Block until generation completes or times out (best-effort)."""
        import time

        deadline = time.time() + timeout_seconds
        while time.time() < deadline:
            # In a real implementation, we'd poll for completion.
            # For now, sleep and let the native runtime handle it.
            time.sleep(0.1)
