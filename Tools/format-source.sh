#!/bin/sh
black *.py
clang-format --verbose -i ../Source/*.cpp ../Source/*.h
