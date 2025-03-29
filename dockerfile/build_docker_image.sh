#!/bin/bash

set -e

cd /

# We have a simple patch to workaround a bug (or feature?) in LLVM's PreserveMost calling convention, 
# and to extend LLVM's GHC calling convention to allow passing more arguments.
#
# This unfortunately means that we have to build Clang+LLVM from source (we need to build
# Clang from source as well, since we have C++ code that uses PreserveMost calling convention)
#
# Install clang-14, which will be used to build Clang+LLVM from source
# Note that thanks to clang-14 is not the default Clang version on Ubuntu, all the executable files are suffixed
# (e.g., the Clang executable is clang-14, not clang). This is a good thing for us since it happens to also
# prevent name collision between the system version and our build-from-source version.
#

# Checkout LLVM 15.0.3
#
LLVM_SRC_DIR=/llvm-src
mkdir $LLVM_SRC_DIR
cd $LLVM_SRC_DIR
git clone -b llvmorg-15.0.3 --depth 1 https://github.com/llvm/llvm-project.git

# Apply our patch
#
cd $LLVM_SRC_DIR/llvm-project
mv /llvm.patch llvm.patch
git apply llvm.patch

# Build and install Clang+LLVM
# Since we are already building Clang+LLVM by ourselves, take this chance to enable RTTI. 
# (otherwise we would need to either disable RTTI for our own LLVM code, or risk random link failures..)
# Clang/LLVM's performance isn't a problem, since we are only using them for the build step.
#
# We do not enable debug info for LLVM library. While seemingly good, it turns out to be a terrible idea: 
# gdb becomes extraordinarily slow due to the extra debug info, and it turns out that even LLVM's 
# stack trace printer fails due to OOM while parsing the debug info...
#
mkdir build
cd $LLVM_SRC_DIR/llvm-project/build
CC=clang-14 CXX=clang++-14 cmake -GNinja -DLLVM_ENABLE_DUMP=ON -DLLVM_ENABLE_RTTI=ON -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS="clang;compiler-rt" -DLLVM_TARGETS_TO_BUILD="X86;AArch64" ../llvm

# Leave two CPUs idle so the system won't be irresponsible during the build
#
REQUIRES_RTTI=1 ninja -j$((`nproc`-2))
REQUIRES_RTTI=1 ninja install

# Having built Clang+LLVM, we can now uninstall the system Clang compiler
#
apt remove -y clang-14
apt autoremove -y

# It seems like after uninstalling the system Clang, the ld link is broken.. fix it
#
#update-alternatives --install /usr/bin/ld ld /usr/local/bin/mold 100
update-alternatives --install /usr/bin/ld ld /usr/bin/lld 120

# Remove the Clang/LLVM build directory
#
cd /
rm -rf $LLVM_SRC_DIR

# set user
#
userdel -r ubuntu
useradd -ms /bin/bash u
usermod -aG sudo u
echo 'u ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers
