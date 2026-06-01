#!/bin/sh

# Generate compile_commands.json for clangd
pio run -t compiledb

# Install vim plugins
vim +PlugInstall +qall
