cmake_minimum_required(VERSION 2.6)

project(elf++)

add_library(
  elf++
  STATIC
  elf.cpp
  mmap_loader.cpp
  to_string.cpp
)

