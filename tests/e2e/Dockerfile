FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build libssl-dev \
    libspdlog-dev nlohmann-json3-dev libsqlite3-dev \
    python3 python3-pip ca-certificates \
    && pip3 install conan \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY include/ ./include/
COPY src/ ./src/
COPY cmake/ ./cmake/
COPY CMakeLists.txt conanfile.py LICENSE ./
COPY tests/e2e/multi_node_test.cpp ./main.cpp

RUN conan profile detect --force && \
    sed -i 's/cppstd=gnu17/cppstd=20/' /root/.conan2/profiles/default && \
    conan install . --output-folder=build --build=missing -s build_type=Release

RUN cd build && \
    cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake \
        -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON \
        -DEDGEAI_ENABLE_TESTS=OFF -DEDGEAI_ENABLE_TOOLS=OFF && \
    cmake --build . --parallel -j2

RUN g++ -std=c++20 -O2 \
        -I./include \
        -I/root/.conan2/p/spdlog*/p/include \
        -I/root/.conan2/p/fmt*/p/include \
        -I/root/.conan2/p/nlohmann*/p/include \
        -I/root/.conan2/p/tl-ex*/p/include \
        main.cpp -o /usr/local/bin/multi-node-test \
        build/libedgeai.a -lspdlog -lfmt -lsqlite3 -lssl -lcrypto -lpthread -ldl

CMD ["multi-node-test"]
