# Prebuilt Artifacts

If you just want to link against Socrates without building from source, grab the right binary for your platform.

## macOS (Apple Silicon — M1/M2/M3/M4)

```
dist/macos-arm64/
├── libsocrates.a          # static library (1.7 MB)
└── include/socrates/      # all headers
```

```bash
# Link against it
clang++ -std=c++20 -I dist/include your_app.cpp dist/macos-arm64/libsocrates.a \
    -framework Metal -framework Foundation -lssl -lcrypto -lllama -lsqlite3
```

## Android (ARM64 — Snapdragon / Exynos / Dimensity)

```
dist/android-arm64/
├── libsocrates.so         # shared library (6.1 MB, ELF aarch64)
└── include/socrates/      # all headers
```

```bash
# Copy into your Android project
cp dist/android-arm64/libsocrates.so app/src/main/jniLibs/arm64-v8a/

# CMakeLists.txt
add_library(socrates SHARED IMPORTED)
set_target_properties(socrates PROPERTIES IMPORTED_LOCATION
    ${CMAKE_SOURCE_DIR}/jniLibs/arm64-v8a/libsocrates.so)
target_include_directories(socrates INTERFACE dist/include)
target_link_libraries(your_app socrates)
```

## Windows (x64 — Snapdragon X Elite / x86-64)

Build from source for now. See [CONTRIBUTING.md](../CONTRIBUTING.md#windows).

## iOS (ARM64 — A-series / M-series)

Build from source (requires Xcode). See [CONTRIBUTING.md](../CONTRIBUTING.md#ios).

---

## C ABI (Language-agnostic)

For JNI (Android), ObjC++ (iOS/macOS), or C++/WinRT (Windows), use the C bindings:

```c
#include "socrates_c.h"

socrates_config_t cfg = { .version = 1, .cluster_id = "my-cluster" };
socrates_runtime_t rt;
socrates_init(cfg, &rt);
socrates_start(rt);

socrates_generate(rt, "req-1", "Hello world!", 100, 0.7f,
    [](const socrates_stream_event_t* ev, void* data) {
        printf("[%llu] %s\n", ev->sequence, ev->text_delta);
    }, NULL, NULL);

socrates_shutdown(rt);
```

Headers: `include/socrates_c.h`  
macOS: link `libsocrates.a`  
Android: link `libsocrates.so`

---

Built from commit `$(git rev-parse --short HEAD)`.  
See [README.md](../README.md) for the full project overview.
