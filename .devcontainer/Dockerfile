FROM ubuntu:24.04

ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000
# Emscripten SDK version ("latest" or a pinned release such as "4.0.10").
ARG EMSDK_VERSION=latest
# Node.js major version, for the TS / tRPC side and the headless WASM test runner.
ARG NODE_MAJOR=24

ENV DEBIAN_FRONTEND=noninteractive

# Toolchains (GCC 13 + Clang 18), build tools, and OpenSSL for the asio::ssl adapter.
# Header-only or source dependencies (asio, llhttp, Catch2...) are fetched by CPM at configure time.
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++-13 \
      clang-18 clang-format-18 clang-tidy-18 clangd-18 lld-18 \
      cmake ninja-build ccache gdb \
      git ca-certificates curl gnupg sudo xz-utils \
      libssl-dev zlib1g-dev pkg-config \
      python3 \
 && rm -rf /var/lib/apt/lists/* \
 && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100 \
 && update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-18 100 \
 && update-alternatives --install /usr/bin/clangd clangd /usr/bin/clangd-18 100

# Node.js from NodeSource (Ubuntu's package is too old for current TS tooling).
RUN curl -fsSL https://deb.nodesource.com/setup_${NODE_MAJOR}.x | bash - \
 && apt-get install -y --no-install-recommends nodejs \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu images ship an "ubuntu" user with uid 1000: replace it so files written in the bind-mounted
# workspace keep the host user's ownership.
RUN userdel -r ubuntu 2>/dev/null || true \
 && groupadd --gid ${USER_GID} ${USERNAME} \
 && useradd --uid ${USER_UID} --gid ${USER_GID} -m -s /bin/bash ${USERNAME} \
 && echo "${USERNAME} ALL=(root) NOPASSWD:ALL" > /etc/sudoers.d/${USERNAME} \
 && chmod 0440 /etc/sudoers.d/${USERNAME}

# Emscripten, owned by the dev user: emcc writes its system library cache inside the SDK.
RUN git clone --depth 1 https://github.com/emscripten-core/emsdk.git /opt/emsdk \
 && /opt/emsdk/emsdk install ${EMSDK_VERSION} \
 && /opt/emsdk/emsdk activate ${EMSDK_VERSION} \
 && chown -R ${USER_UID}:${USER_GID} /opt/emsdk \
 && echo 'EMSDK_QUIET=1 . /opt/emsdk/emsdk_env.sh' >> /etc/bash.bashrc

ENV CC=gcc-13 \
    CXX=g++-13 \
    CMAKE_GENERATOR=Ninja \
    CMAKE_C_COMPILER_LAUNCHER=ccache \
    CMAKE_CXX_COMPILER_LAUNCHER=ccache \
    CCACHE_DIR=/home/${USERNAME}/.cache/ccache \
    CPM_SOURCE_CACHE=/home/${USERNAME}/.cache/CPM

USER ${USERNAME}
