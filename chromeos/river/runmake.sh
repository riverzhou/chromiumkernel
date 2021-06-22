#!/usr/bin/env bash
# Run this code on kernel source top dir

make CC=clang LLVM=1 O=build -j8 
if [ $? -ne 0 ]; then
echo Build Failed ..
exit
fi

rm ../install -rf 
mkdir ../install
cp build/arch/x86/boot/bzImage ../install/kernel-river 
make CC=clang LLVM=1 O=build -j8 modules_install INSTALL_MOD_PATH=../../install/

