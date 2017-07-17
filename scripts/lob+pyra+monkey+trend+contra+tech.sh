#!/bin/zsh -i

src/lobot XAU |& stdbuf -o L tee /tmp/lobot &
src/pyrabot XAU -N 10 -Q 200+2800 --max 100000 -F 0.5 -d
sleep 0.2
src/monkey -F 0.6 XAU -Q 50 -N 7 -d --max 25000
src/trendbot -F 1.0 -Q 100 -d --max 25000
src/trendbot -F 2.0 -Q 500 -d --max 100000
src/trendbot -F 2.0 -Q 1000 -d --max 100000
src/trendbot -F 3.0 -Q 2500 -d --max 250000 --contrarian
src/techbot XAU -F 1.0 -d
fg
