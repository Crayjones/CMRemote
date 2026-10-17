FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS dev
LABEL org.remotemanager.project="remotemanager"
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential perl git curl ca-certificates python3 python3-venv pkg-config libglib2.0-0t64 \
    libgl1 libegl1 libopengl0 libfontconfig1 libfreetype6 libdbus-1-3 libxkbcommon0 libxkbcommon-x11-0 \
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-xinerama0 \
    libxcb-randr0 libxcb-shape0 libxcb-xfixes0 libxcb-sync1 libxcb-xkb1 libsm6 libice6 libx11-6 \
    libwayland-client0 libwayland-cursor0 libwayland-egl1 fonts-dejavu-core xvfb \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/remotemanager
COPY dependencies.lock.json ./
COPY scripts/bootstrap.py scripts/verify-dependencies.py ./scripts/
COPY scripts/bootstrap-requirements.txt ./scripts/
COPY app/shaders/ ./app/shaders/
COPY cmake/libvterm/ ./cmake/libvterm/
RUN python3 -m venv .dev/linux/python && \
    .dev/linux/python/bin/pip --cache-dir .dev/linux/cache/pip install -r scripts/bootstrap-requirements.txt && \
    .dev/linux/python/bin/python scripts/bootstrap.py --platform linux && \
    dpkg-query -W > .dev/linux/native-packages.txt && \
    chmod -R a+rX .dev/linux && rm -rf build .dev/linux/src .dev/linux/downloads .dev/linux/cache
RUN apt-get update && apt-get install -y --no-install-recommends libgl-dev libegl-dev libopengl-dev libxkbcommon-dev libx11-dev libxext-dev libwayland-dev patchelf weston libgl1-mesa-dri libegl-mesa0 openssh-client && \
    rm -rf /var/lib/apt/lists/* && dpkg-query -W > .dev/linux/native-packages.txt
ARG LOCAL_UID=1001
ARG LOCAL_GID=1001
RUN if ! getent group "$LOCAL_GID" >/dev/null; then groupadd --gid "$LOCAL_GID" dev; fi && \
    if ! getent passwd "$LOCAL_UID" >/dev/null; then useradd --create-home --uid "$LOCAL_UID" --gid "$LOCAL_GID" --shell /bin/bash dev; fi
WORKDIR /workspace
ENV RM_SDK_ROOT=/opt/remotemanager/.dev/linux
USER ${LOCAL_UID}:${LOCAL_GID}
CMD ["sleep", "infinity"]

# The 24.04 server sends bitmap updates before resize reactivation completes.
# Keep the development SDK unchanged; use the corrected server only in this fixture.
FROM ubuntu:26.04@sha256:f144425ff09be612d6d9ad965196e9cdc23dae1f42110a8a11a3e9a8198759f7 AS rdp-fixture
LABEL org.remotemanager.project="remotemanager"
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends xrdp xorgxrdp xserver-xorg-core python3 python3-tk fonts-dejavu-core && \
    dpkg-query -W > /fixture-packages.txt && \
    rm -rf /var/lib/apt/lists/* && useradd --create-home --shell /bin/bash rdpfixture && \
    usermod -a -G ssl-cert xrdp
COPY tests/rdp-fixture/ /fixture/
RUN printf 'exec python3 /fixture/server.py\n' > /home/rdpfixture/.xsession && \
    chown rdpfixture:rdpfixture /home/rdpfixture/.xsession
EXPOSE 3389
CMD ["python3", "/fixture/start.py"]

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS ssh-fixture
LABEL org.remotemanager.project="remotemanager"
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends openssh-server vim tmux python3 ca-certificates locales && \
    dpkg-query -W > /fixture-packages.txt && rm -rf /var/lib/apt/lists/* && \
    useradd --create-home --shell /bin/bash sshfixture && mkdir -p /run/sshd
COPY tests/ssh-fixture/ /fixture/
EXPOSE 22 8022
CMD ["python3", "/fixture/start.py"]
