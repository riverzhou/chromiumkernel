#!/usr/bin/env bash
# Run this code on kernel source top dir

rm ../install -rf 
mkdir ../install
make CC=clang LLVM=1 O=build -j8 
cp build/arch/x86/boot/bzImage ../install/kernel-river 
make CC=clang LLVM=1 O=build -j8 modules_install INSTALL_MOD_PATH=../install/

