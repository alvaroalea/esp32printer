// paleta_ryb.h -- Paleta de 256 colores para imprimir en VARIAS PASADAS con una cinta
// de cuatro tintas Rojo / Amarillo / Azul / Negro (RYBK).
//
// El indice de paleta (1 byte por pixel en el BMP de 8 bits) es un MAPA DE BITS de tinta:
//
//        bit:   7    6    5    4  |  3    2    1    0
//        tinta: R    Y    B    K  |  R    Y    B    K
//               \_ 2a pasada _____/  \_ 1a pasada ____/
//                  (nibble alto)        (nibble bajo)
//
//   0b00000000 (0x00)  papel blanco, sin tinta
//   0b00000001         negro, 1 pasada          (80%)
//   0b00010001         negro, 2 pasadas         (100%)
//   0b00001000         rojo, 1 pasada           0b10001000  rojo, 2 pasadas
//   0b00001100         rojo + amarillo = naranja (1 pasada de cada)
//   0b01000110         verde claro: amarillo 2 pasadas + azul 1 pasada
//
// Cuanta tinta hay en cada color la dan los bits, NO su posicion: cada tinta vale
//   - 0 bits puestos (ni en el nibble bajo ni en el alto)  -> sin tinta
//   - 1 bit puesto   (en cualquiera de los dos nibbles)    -> 1 pasada  = RYBK_PASS1_PERCENT (80%)
//   - 2 bits puestos (los dos nibbles)                     -> 2 pasadas = RYBK_PASS2_PERCENT (100%)
// El nibble alto solo recuerda que esa tinta ya habia recibido una pasada: un rojo que
// esta solo en el nibble alto (0b10000000) sigue siendo una unica pasada, 80%.
//
// REGLA DE PINTADO (rybkPaint): al pintar un pixel con una mascara de tintas M (bits del
// nibble bajo; p.ej. naranja = INK_R | INK_Y), sobre un pixel que ya vale CUR:
//     si (CUR & M) == 0   -> CUR | M          (ninguna de esas tintas estaba usada: 1a pasada)
//     si no               -> CUR | (M << 4)   (alguna ya estaba: todo el color pasa al nibble alto)
// Es una comparacion con AND y una OR, sin tablas. Ejemplo "raro" del diseno: amarillo
// (0b00000100) y luego naranja (M = 0b1100): (CUR & M) != 0, asi que el naranja entero va al
// nibble alto -> 0b11000100: el amarillo llega al 100% (2 pasadas) y el rojo queda en 80%
// (una sola pasada, aunque su bit este en el nibble alto).
//
// El COLOR que se ve de cada indice (rybkColor) se calcula con un modelo RYB (mezcla de pintura de
// toda la vida): cada tinta R / Y / B tiene una cantidad (0%, RYBK_PASS1_PERCENT o RYBK_PASS2_PERCENT
// segun sus pasadas) y el color sale de interpolar entre los 8 colores de las esquinas del cubo RYB
// (RYBK_CORNER_RGB: papel, rojo, amarillo, naranja, azul, violeta, verde y las tres tintas juntas).
// Asi cada tinta sola al 100% es EXACTAMENTE su color (rojo = 255,0,0) y las mezclas son las que
// se esperan (rojo+amarillo = naranja, amarillo+azul = verde, rojo+azul = violeta), sin ensuciarse.
// El negro (K) va aparte: oscurece el resultado en proporcion a su cobertura (80% -> queda el 20%
// del brillo, 100% -> negro). Se puede retocar todo en este fichero: los colores de las esquinas,
// los porcentajes por pasada y a que tintas equivale cada ESC r.

#pragma once
#include <Arduino.h>

// --- Tintas: bit de cada una en el nibble bajo (1a pasada). En el nibble alto, el mismo bit +4.
#define INK_K 0x01   // negro
#define INK_B 0x02   // azul
#define INK_Y 0x04   // amarillo
#define INK_R 0x08   // rojo

#define RYBK_WHITE_INDEX 0x00   // papel sin tinta

// --- Cuanto color aporta cada pasada (porcentaje de cobertura de la tinta)
#define RYBK_PASS1_PERCENT  80  // una pasada
#define RYBK_PASS2_PERCENT 100  // dos pasadas

// --- Colores de las 8 esquinas del cubo RYB: indice = (hay rojo ? 1 : 0) | (hay amarillo ? 2 : 0) | (hay azul ? 4 : 0),
// con cada tinta presente al 100%. Los tres primeros colores puros (rojo, amarillo, azul) y el papel
// son los colores "de manual"; los secundarios y el de las tres tintas se pueden ajustar a gusto.
#define RYB_R 1
#define RYB_Y 2
#define RYB_B 4
static const uint8_t RYBK_CORNER_RGB[8][3] = {
  { 255, 255, 255 },   // 0            papel blanco
  { 255,   0,   0 },   // R            rojo
  { 255, 255,   0 },   // Y            amarillo
  { 255, 128,   0 },   // R+Y          naranja
  {   0,   0, 255 },   // B            azul
  { 128,   0, 192 },   // R+B          violeta
  {   0, 170,   0 },   // Y+B          verde
  {  64,  32,   0 },   // R+Y+B        las tres tintas: marron oscuro
};

// --- ESC r n (n = 0..6) -> tintas que se usan. Los 7 colores de ESC/P eran negro, magenta, cian,
// violeta, amarillo, naranja y verde; con una cinta R-Y-B-K se aproximan asi:
static const uint8_t RYBK_ESC_R_MASK[7] = {
  INK_K,            // 0 negro
  INK_R,            // 1 (magenta)  -> rojo
  INK_B,            // 2 (cian)     -> azul
  INK_R | INK_B,    // 3 violeta    -> rojo + azul
  INK_Y,            // 4 amarillo
  INK_R | INK_Y,    // 5 naranja    -> rojo + amarillo
  INK_Y | INK_B,    // 6 verde      -> amarillo + azul
};

// Pinta con la mascara de tintas 'inkMask' (bits del nibble bajo) un pixel cuyo indice actual es 'cur'
// y devuelve el indice nuevo. Ver REGLA DE PINTADO arriba.
static inline uint8_t rybkPaint(uint8_t cur, uint8_t inkMask) {
  inkMask &= 0x0F;
  return ((cur & inkMask) == 0) ? (uint8_t)(cur | inkMask) : (uint8_t)(cur | (inkMask << 4));
}

// Color RGB (0..255 por canal) del indice de paleta 'idx'. Aritmetica entera (sin float).
static inline void rybkColor(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b) {
  // Cantidad de cada tinta en % segun sus pasadas (bit i del nibble bajo + bit i del alto)
  uint32_t amt[4];
  for (int i = 0; i < 4; i++) {
    int passes = ((idx >> i) & 1) + ((idx >> (i + 4)) & 1);
    amt[i] = (passes == 0) ? 0 : (passes == 1) ? RYBK_PASS1_PERCENT : RYBK_PASS2_PERCENT;
  }
  const uint32_t aR = amt[3], aY = amt[2], aB = amt[1], aK = amt[0];
  // Interpolacion trilineal entre las 8 esquinas del cubo RYB (pesos en % x % x % = 0..1.000.000)
  uint32_t acc[3] = { 0, 0, 0 };
  for (int m = 0; m < 8; m++) {
    uint32_t w = ((m & RYB_R) ? aR : 100 - aR) * ((m & RYB_Y) ? aY : 100 - aY) * ((m & RYB_B) ? aB : 100 - aB);
    if (w == 0) continue;
    for (int c = 0; c < 3; c++) acc[c] += w * RYBK_CORNER_RGB[m][c];
  }
  uint8_t out[3];
  for (int c = 0; c < 3; c++) {
    uint32_t v = (acc[c] + 500000UL) / 1000000UL;   // color RYB, redondeado
    v = (v * (100 - aK) + 50) / 100;                // el negro oscurece en proporcion a su cobertura
    out[c] = (uint8_t)v;
  }
  *r = out[0]; *g = out[1]; *b = out[2];
}
