#/env/bin sh
set -e
mkdir -p $(pwd)/build
g++ -D DEBUG -lX11 scame.cpp -o $(pwd)/build/scame -Wall -Wextra -pedantic -std=c++20
