#!/bin/bash

# Check if homebrew LLVM is installed
HOMEBREW_LLVM_PATH="/opt/homebrew/opt/llvm/bin"

if [ -f "$HOMEBREW_LLVM_PATH/llvm-config" ] && [ -f "$HOMEBREW_LLVM_PATH/clang" ]; then
    echo "Using homebrew LLVM installation..."
    CLANG="$HOMEBREW_LLVM_PATH/clang"
    LLVM_CONFIG="$HOMEBREW_LLVM_PATH/llvm-config"
else
    echo "Using system LLVM installation..."
    CLANG="clang"
    LLVM_CONFIG="llvm-config"
fi

$CLANG -std=c11 -O3 -march=native -o doc_gen main.c -I$($LLVM_CONFIG --includedir) -L$($LLVM_CONFIG --libdir) -lclang

if [ $? -eq 0 ]; then
    echo "done"
else
    echo "Build failed!"
    exit 1
fi
