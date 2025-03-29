FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

WORKDIR /

COPY llvm.patch /llvm.patch
COPY ccache.conf /etc/ccache.conf

# install misc dependency
#
RUN apt update
RUN apt-get install -y wget tar xz-utils sudo ninja-build ccache libtinfo-dev libz-dev lsb-release \
                       software-properties-common gnupg libstdc++-14-dev binutils-gold software-properties-common \
                       lld cmake binutils build-essential git python-is-python3 clang-14 libxml2 libxml2-dev libasan6 \
                       gcc-12-aarch64-linux-gnu g++-12-aarch64-linux-gnu debootstrap qemu-user-static binfmt-support

RUN update-alternatives --install /usr/bin/ld ld /usr/bin/lld 120

COPY build_docker_image.sh /build_docker_image.sh
RUN bash build_docker_image.sh
RUN rm /build_docker_image.sh

USER u
WORKDIR /home/u

