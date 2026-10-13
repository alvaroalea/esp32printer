// paleta_cmyk.h -- Paleta de 256 colores para imprimir en VARIAS PASADAS con una cinta
// de cuatro tintas Cian / Magenta / Amarillo / Negro (CMYK).
//
// NOMBRES: el manual de la impresora llama "rojo" a lo que en realidad es MAGENTA y "naranja" a lo
// que en realidad es ROJO (magenta + amarillo). En este fichero y en el programa se usan siempre
// los nombres reales de las tintas.
//
// El indice de paleta (1 byte por pixel en el BMP de 8 bits) es un MAPA DE BITS de tinta:
//
//        bit:   7    6    5    4  |  3    2    1    0
//        tinta: M    Y    C    K  |  M    Y    C    K
//               \_ 2a pasada _____/  \_ 1a pasada ____/
//                  (nibble alto)        (nibble bajo)
//
//   0b00000000 (0x00)  papel blanco, sin tinta
//   0b00000001         negro, 1 pasada          (80%)
//   0b00010001         negro, 2 pasadas         (100%)
//   0b00001000         magenta, 1 pasada        0b10001000  magenta, 2 pasadas
//   0b00001100         magenta + amarillo = rojo (1 pasada de cada)
//   0b01000110         verde claro: amarillo 2 pasadas + cian 1 pasada
//
// Cuanta tinta hay en cada color la dan los bits, NO su posicion: cada tinta vale
//   - 0 bits puestos (ni en el nibble bajo ni en el alto)  -> sin tinta
//   - 1 bit puesto   (en cualquiera de los dos nibbles)    -> 1 pasada  = CMYK_PASS1_PERCENT (80%)
//   - 2 bits puestos (los dos nibbles)                     -> 2 pasadas = CMYK_PASS2_PERCENT (100%)
// El nibble alto solo recuerda que esa tinta ya habia recibido una pasada: un magenta que
// esta solo en el nibble alto (0b10000000) sigue siendo una unica pasada, 80%.
//
// REGLA DE PINTADO (cmykPaint): al pintar un pixel con una mascara de tintas M (bits del
// nibble bajo; p.ej. rojo = INK_M | INK_Y), sobre un pixel que ya vale CUR:
//     si (CUR & M) == 0   -> CUR | M          (ninguna de esas tintas estaba usada: 1a pasada)
//     si no               -> CUR | (M << 4)   (alguna ya estaba: todo el color pasa al nibble alto)
// Es una comparacion con AND y una OR, sin tablas. Ejemplo "raro" del diseno: amarillo
// (0b00000100) y luego rojo (M = 0b1100): (CUR & M) != 0, asi que el rojo entero va al
// nibble alto -> 0b11000100: el amarillo llega al 100% (2 pasadas) y el magenta queda en 80%
// (una sola pasada, aunque su bit este en el nibble alto).
//
// El COLOR que se ve de cada indice (cmykColor) se calcula con un modelo sustractivo, que es como se
// comporta de verdad una cinta CMY: cada tinta es un filtro sobre el papel blanco con cobertura =
// 80% / 100% segun sus pasadas, y los filtros se multiplican canal a canal. Asi, con tintas cian,
// magenta y amarillo, salen solos los colores secundarios: magenta + amarillo = rojo, amarillo +
// cian = verde, magenta + cian = violeta/azul, y las tres juntas = negro. Se puede retocar todo en
// este fichero: los colores de las tintas, los porcentajes por pasada y a que tintas equivale cada ESC r.

#pragma once
#include <Arduino.h>

// --- Tintas: bit de cada una en el nibble bajo (1a pasada). En el nibble alto, el mismo bit +4.
#define INK_K 0x01   // negro
#define INK_C 0x02   // cian
#define INK_Y 0x04   // amarillo
#define INK_M 0x08   // magenta

#define CMYK_WHITE_INDEX 0x00   // papel sin tinta

// --- Cuanto color aporta cada pasada (porcentaje de cobertura de la tinta)
#define CMYK_PASS1_PERCENT  80  // una pasada
#define CMYK_PASS2_PERCENT 100  // dos pasadas

// --- Color de cada tinta al 100% sobre papel blanco, ordenado por NUMERO DE BIT: 0=K, 1=C, 2=Y, 3=M.
// Son filtros: lo que NO aparece en un canal es lo que esa tinta absorbe (el cian absorbe el rojo,
// el magenta el verde, el amarillo el azul). Valores vivos, sin "ensuciar" con otros canales.
static const uint8_t CMYK_INK_RGB[4][3] = {
  {   0,   0,   0 },   // bit 0  K  negro
  {   0, 190, 255 },   // bit 1  C  cian
  { 255, 240,   0 },   // bit 2  Y  amarillo
  { 255,   0, 170 },   // bit 3  M  magenta
};

// --- ESC r n (n = 0..6) -> tintas que se usan. Los 7 colores de ESC/P son negro, magenta, cian,
// violeta, amarillo, rojo y verde (el manual llama "rojo" al magenta y "naranja" al rojo):
static const uint8_t CMYK_ESC_R_MASK[7] = {
  INK_K,            // 0 negro
  INK_M,            // 1 magenta
  INK_C,            // 2 cian
  INK_M | INK_C,    // 3 violeta  -> magenta + cian
  INK_Y,            // 4 amarillo
  INK_M | INK_Y,    // 5 rojo     -> magenta + amarillo
  INK_Y | INK_C,    // 6 verde    -> amarillo + cian
};

// Pinta con la mascara de tintas 'inkMask' (bits del nibble bajo) un pixel cuyo indice actual es 'cur'
// y devuelve el indice nuevo. Ver REGLA DE PINTADO arriba.
static inline uint8_t cmykPaint(uint8_t cur, uint8_t inkMask) {
  inkMask &= 0x0F;
  return ((cur & inkMask) == 0) ? (uint8_t)(cur | inkMask) : (uint8_t)(cur | (inkMask << 4));
}

// Color RGB (0..255 por canal) del indice de paleta 'idx'. Aritmetica entera (sin float).
static inline void cmykColor(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b) {
  uint8_t out[3];
  for (int c = 0; c < 3; c++) {
    uint32_t v = 255; // papel blanco
    for (int i = 0; i < 4; i++) {
      int passes = ((idx >> i) & 1) + ((idx >> (i + 4)) & 1);
      if (passes == 0) continue;
      uint32_t cov = (passes == 1) ? CMYK_PASS1_PERCENT : CMYK_PASS2_PERCENT;       // % de cobertura
      // v = v * (1 - cov * (1 - tinta/255)), con redondeo; 25500 = 100 * 255
      v = (v * (25500UL - cov * (255UL - CMYK_INK_RGB[i][c])) + 12750UL) / 25500UL;
    }
    out[c] = (uint8_t)v;
  }
  *r = out[0]; *g = out[1]; *b = out[2];
}
