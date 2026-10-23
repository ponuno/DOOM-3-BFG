# cmake -DIN=<binary> -DOUT=<file.c> -DNAME=<symbol> -P bin2c.cmake
# Emits: const uint8_t NAME[] (4-byte aligned) and const uint32_t NAME_size.
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR size "${hexlen} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "/* generated from ${IN} by bin2c.cmake */\n#include <stdint.h>\n__attribute__((aligned(4))) const uint8_t ${NAME}[] = {\n${bytes}\n};\nconst uint32_t ${NAME}_size = ${size};\n")
