#!/bin/bash
set -e

mkdir -p .vscode
make clean
bear --output .vscode/compile_commands.json -- make all
sed -i 's|'$PWD'|${workspaceFolder}|g' .vscode/compile_commands.json