#!/usr/bin/env python3

import sys

NUM_CHARS = 96
CHAR_SIZE = 8
EXPECTED_SIZE = NUM_CHARS * CHAR_SIZE


def transpose_char(data):
    """Trasponer un carácter 8x8.

    Entrada:
        8 bytes, uno por cada fila.
        Bit 7 = píxel izquierdo
        Bit 0 = píxel derecho

    Salida:
        8 bytes, uno por cada columna.
        Bit 0 = píxel superior
        Bit 7 = píxel inferior
    """

    result = bytearray(8)

    for x in range(8):
        value = 0

        for y in range(8):
            if data[y] & (0x80 >> x):
                value |= (1 << y)

        result[x] = value

    return result


def ascii_comment(code):
    """Genera el comentario con código y carácter ASCII."""

    if code == 32:
        char = " "

    elif 32 <= code <= 126:
        char = chr(code)

    else:
        char = "?"

    return f"// {code} '{char}'"


def generate_c(data):
    """Genera el array C unidimensional."""

    output = []

    output.append("static const uint8_t charset[768] = {")

    for char_num in range(NUM_CHARS):
        offset = char_num * CHAR_SIZE
        char_data = data[offset:offset + CHAR_SIZE]

        transposed = transpose_char(char_data)

        values = ", ".join(
            f"0x{b:02X}" for b in transposed
        )

        ascii_code = 32 + char_num
        comment = ascii_comment(ascii_code)

        output.append(
            f"    {values},  {comment}"
        )

    output.append("};")

    return "\n".join(output)


def main():
    if len(sys.argv) != 2:
        print(
            f"Uso: {sys.argv[0]} archivo.ch8",
            file=sys.stderr
        )
        sys.exit(1)

    filename = sys.argv[1]

    try:
        with open(filename, "rb") as f:
            data = f.read()

    except OSError as e:
        print(
            f"Error leyendo '{filename}': {e}",
            file=sys.stderr
        )
        sys.exit(1)

    if len(data) != EXPECTED_SIZE:
        print(
            f"Error: el archivo tiene {len(data)} bytes, "
            f"pero se esperaban {EXPECTED_SIZE} bytes "
            f"(96 caracteres x 8 bytes).",
            file=sys.stderr
        )
        sys.exit(1)

    print(generate_c(data))


if __name__ == "__main__":
    main()
