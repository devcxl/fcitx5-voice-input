FROM archlinux:base-devel

RUN pacman -Syu --noconfirm && \
    pacman -S --noconfirm --needed \
        cmake \
        pkg-config \
        fcitx5 \
        pipewire \
        libpulse \
        jsoncpp \
        curl \
        onnxruntime-cpu \
        sudo && \
    pacman -Scc --noconfirm

RUN useradd -m builder && \
    echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

WORKDIR /build
CMD chown -R builder:builder /build && su builder -c "makepkg -f --noconfirm"
