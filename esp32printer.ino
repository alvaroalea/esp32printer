/*
 * ==========================================================================
 *  Emulador de impresora Epson LQ (24 agujas, color) para ESP32
 * ==========================================================================
 *
 * Escucha el puerto serie (RS-232 a traves de un MAX232) esperando el
 * flujo de comandos ESC/P que enviaria un ordenador a una impresora matricial
 * Epson LQ de 24 agujas con cinta de color (Black/Cyan/Magenta/Yellow).
 * Interpreta texto y graficos de puntos, los "pinta" en un lienzo virtual
 * y, cada vez que llega un salto de pagina (Form Feed) o pasan unos segundos
 * sin actividad, vuelca la pagina completa a un fichero BMP (24 bits, sin
 * comprimir, formato BI_RGB) en la tarjeta SD.
 *
 * ---------------------------------------------------------------------
 * HARDWARE ESPERADO (ajustar en la seccion CONFIGURACION):
 *   - ESP32 (cualquier variante con SPI libre para SD y un UART libre)
 *   - Lector de tarjeta SD por SPI
 *   - MAX232 (u otro transceptor RS-232) en un UART hardware (Serial2)
 *
 * ---------------------------------------------------------------------
 * LIMITACIONES CONOCIDAS (documentadas para no llevar a confusion):
 *   - Resolucion interna fija a 180 dpi tanto en horizontal como en
 *     vertical. Los modos de 360 dpi (ESC * m=40/72) se aceptan pero se
 *     "diezman" a 180 dpi (se descarta una columna de cada dos).
 *   - No se implementa avance de papel hacia atras (no existe en ESC/P
 *     estandar salvo microjustificacion, que tampoco se soporta), por lo
 *     que la imagen se genera en una unica pasada, de arriba a abajo.
 *   - Solo se soporta un subconjunto practico de ESC/P: inicializacion,
 *     avance de linea/pagina, interlineado (ESC 2 / ESC 0 / ESC 3 n),
 *     graficos de puntos (ESC K/L/Y/Z y ESC *), negrita (ESC E/F),
 *     subrayado (ESC - n) y color de cinta (ESC r n). Los caracteres
 *     definidos por el usuario, tabulaciones verticales, microavances
 *     (ESC J), formato de pagina, etc. no estan implementados; los bytes
 *     de parametro de secuencias no reconocidas se descartan de forma
 *     conservadora para no desincronizar el interprete.
 *   - La paleta de color es la tipica de cinta de 4 bandas de estas
 *     impresoras (Negro/Cian/Magenta/Amarillo + combinaciones), no un
 *     RGB continuo real.
 *
 * Autor: generado para un proyecto de emulacion de impresora sobre ESP32.
 * ==========================================================================
 */

#include <SPI.h>
#include <SD.h>
#include "font5x7.h"

// ================================ CONFIGURACION ===========================

// --- Tarjeta SD (SPI) ---
#define SD_CS_PIN     5      // Chip Select de la SD
// Si se usan pines SPI no estandar, llamar antes a SPI.begin(SCK,MISO,MOSI,CS)

// --- Puerto serie hacia el MAX232 (Serial2 = UART2 del ESP32) ---
#define SERIAL_RX_PIN 16
#define SERIAL_TX_PIN 17
#define SERIAL_BAUD   9600   // Velocidad tipica de impresora serie de la epoca
#define USE_XONXOFF   true   // Control de flujo por software (recomendado)

// --- Geometria de la pagina virtual (rejilla interna a 180 dpi) ---
#define PAGE_WIDTH_DOTS   1440   // 1440/180 = 8.0 pulgadas de ancho de impresion
#define BAND_HEIGHT       48     // Alto del buffer de bandas en filas (RAM ~ 1440*48 bytes)
#define DEFAULT_LINE_DOTS 30     // 1/6" a 180dpi = 30 puntos (interlineado por defecto)
#define IDLE_TIMEOUT_MS   3000   // Si no llega nada en este tiempo, se cierra la pagina sola

// ================================ PALETA DE COLOR ==========================
// Aproximacion de los colores tipicos de una cinta de 4 bandas (K/C/M/Y)
// tal y como los seleccionaba el comando ESC r n de las Epson JX-80/LQ color.
struct RGB { uint8_t r, g, b; };
static const RGB PALETTE[8] = {
  {0,   0,   0  },  // 0 Negro
  {216, 0,   132},  // 1 Magenta
  {0,   174, 219},  // 2 Cian
  {102, 45,  145},  // 3 Violeta (magenta+cian)
  {255, 221, 0  },  // 4 Amarillo
  {237, 28,  36 },  // 5 Rojo/naranja (magenta+amarillo)
  {0,   166, 81 },  // 6 Verde (cian+amarillo)
  {255, 255, 255},  // 7 Blanco (no usado como tinta, solo de referencia)
};
#define COLOR_WHITE_INDEX 255  // marcador interno de "sin tinta" en el buffer

// ================================ ESTADO GLOBAL ============================

File pageFile;
bool pageOpen = false;          // hay un fichero de pagina abierto
bool pageHasInk = false;        // se ha pintado algo desde que se abrio
uint32_t pageIndex = 0;         // numero de pagina para el nombre de fichero
uint32_t rowsWrittenToFile = 0; // filas ya volcadas a la SD para la pagina actual

int32_t cursorX = 0;            // posicion horizontal actual, en puntos (0..PAGE_WIDTH_DOTS-1)
int32_t cursorY = 0;            // posicion vertical actual dentro de la pagina, en puntos
int32_t lineSpacingDots = DEFAULT_LINE_DOTS;
uint8_t currentColor = 0;       // indice de PALETTE, por defecto negro
bool boldMode = false;
bool underlineMode = false;

unsigned long lastByteMillis = 0;

// --- Buffer de bandas: filas [bandBase .. bandBase+BAND_HEIGHT-1] en RAM ---
// band[y % BAND_HEIGHT][x] = indice de color (0..6) o COLOR_WHITE_INDEX si vacio
static uint8_t band[BAND_HEIGHT][PAGE_WIDTH_DOTS];
int32_t bandBase = 0;  // primera fila (coordenada Y absoluta) representada en el buffer

// ================================ MAQUINA DE ESTADOS ESC/P =================

enum EscState {
  ST_NORMAL,
  ST_ESC,
  ST_ESC_R,        // ESC r n           (color de cinta)
  ST_ESC_3,        // ESC 3 n           (interlineado n/180")
  ST_ESC_MINUS,    // ESC - n           (subrayado on/off)
  ST_ESC_STAR_M,   // ESC * m ...       (grafico: falta el byte m)
  ST_ESC_STAR_NL,  // ESC * m nL ...
  ST_ESC_STAR_NH,  // ESC * m nL nH ... -> pasa a recibir datos
  ST_ESC_STAR_DATA,
  ST_ESC_KLYZ_NL,  // ESC K/L/Y/Z nL nH ... (grafico 8 agujas de densidad fija)
  ST_ESC_KLYZ_NH,
  ST_ESC_KLYZ_DATA,
  ST_ESC_SKIP1     // descarta 1 byte de parametro de comandos no soportados
};

EscState escState = ST_NORMAL;

// Parametros en curso para los modos de grafico
uint8_t  gfxPins;          // 8 o 24 agujas
uint8_t  gfxBytesPerCol;   // 1 u 3 bytes por columna
uint8_t  gfxStepDots;      // avance horizontal (en puntos de 180dpi) por columna
uint16_t gfxNumCols;       // numero total de columnas a recibir
uint16_t gfxColIndex;      // columna actual recibida
uint8_t  gfxColBuf[3];     // bytes acumulados de la columna en curso
uint8_t  gfxColBufPos;
uint8_t  gfxMergeFactor;   // nº de columnas de origen que se combinan (OR) en 1 punto de salida
uint8_t  gfxMergeCount;    // columnas ya combinadas en el acumulador actual
uint8_t  gfxMergeAccum[3]; // acumulador de la fusion (para modos >180dpi, p.ej. 360dpi)

// Tabla de modos ESC * m -> {agujas, dpi_horizontal}
struct GfxMode { uint8_t pins; uint16_t dpi; };
GfxMode lookupStarMode(uint8_t m) {
  switch (m) {
    case 0:  return {8,  60};
    case 1:  return {8,  120};
    case 2:  return {8,  120};
    case 3:  return {8,  240};
    case 4:  return {8,  80};
    case 6:  return {8,  90};
    case 32: return {24, 60};
    case 33: return {24, 120};
    case 38: return {24, 90};
    case 39: return {24, 180};
    case 40: return {24, 360};
    case 71: return {24, 180};
    case 72: return {24, 360};
    default: return {8,  60}; // modo desconocido: se asume el mas simple
  }
}

// ================================ UTILIDADES DE BUFFER =====================

void flushOneRow(); // declaracion adelantada (definida junto al resto de manejo del fichero BMP)

void ensureBandCovers(int32_t y) {
  // Si la fila y ya no cabe en la ventana actual del buffer, volcamos a la SD
  // las filas mas antiguas hasta que quepa.
  while (y >= bandBase + BAND_HEIGHT) {
    flushOneRow();
  }
}

int32_t maxYUsed = -1; // fila mas baja en la que se ha pintado algo desde que se abrio la pagina

void plotDot(int32_t x, int32_t y, uint8_t colorIndex) {
  if (x < 0 || x >= PAGE_WIDTH_DOTS || y < 0) return;
  ensureBandCovers(y);
  if (y < bandBase) return; // fila ya volcada a la SD (no deberia ocurrir con avance monotono)
  int32_t rel = y % BAND_HEIGHT; // buffer en anillo: mismo indexado que usa flushOneRow()
  band[rel][x] = colorIndex;
  pageHasInk = true;
  if (y > maxYUsed) maxYUsed = y;
}

// ================================ FICHERO BMP ===============================

char currentFileName[32];

void writeLE16(File &f, uint16_t v) { f.write((uint8_t)(v & 0xFF)); f.write((uint8_t)(v >> 8)); }
void writeLE32(File &f, uint32_t v) {
  f.write((uint8_t)(v & 0xFF));
  f.write((uint8_t)((v >> 8) & 0xFF));
  f.write((uint8_t)((v >> 16) & 0xFF));
  f.write((uint8_t)((v >> 24) & 0xFF));
}

uint32_t rowSizeBytes() {
  uint32_t raw = (uint32_t)PAGE_WIDTH_DOTS * 3;
  return (raw + 3) & ~((uint32_t)3); // redondeo a multiplo de 4
}

void openNewPage() {
  pageIndex++;
  snprintf(currentFileName, sizeof(currentFileName), "/PAGE%04lu.BMP", (unsigned long)pageIndex);
  pageFile = SD.open(currentFileName, FILE_WRITE);
  if (!pageFile) {
    Serial.printf("[ERROR] No se pudo crear %s en la SD\n", currentFileName);
    pageOpen = false;
    return;
  }

  // --- Cabecera BMP (54 bytes), con alto y tamano en 0 como marcador ---
  pageFile.write((const uint8_t*)"BM", 2);
  writeLE32(pageFile, 0);           // tamano de fichero (se rellena al cerrar)
  writeLE32(pageFile, 0);           // reservado
  writeLE32(pageFile, 54);          // offset a los datos de pixel
  writeLE32(pageFile, 40);          // tamano de BITMAPINFOHEADER
  writeLE32(pageFile, PAGE_WIDTH_DOTS);
  writeLE32(pageFile, 0);           // alto (se rellena al cerrar, en negativo = top-down)
  writeLE16(pageFile, 1);           // planos
  writeLE16(pageFile, 24);          // bits por pixel
  writeLE32(pageFile, 0);           // compresion: BI_RGB (sin comprimir)
  writeLE32(pageFile, 0);           // tamano de la imagen (0 valido para BI_RGB)
  writeLE32(pageFile, 2834);        // resolucion X ~ 180dpi en pixeles/metro
  writeLE32(pageFile, 2834);        // resolucion Y ~ 180dpi en pixeles/metro
  writeLE32(pageFile, 0);           // colores en la paleta
  writeLE32(pageFile, 0);           // colores importantes

  bandBase = 0;
  cursorX = 0;
  cursorY = 0;
  rowsWrittenToFile = 0;
  pageHasInk = false;
  maxYUsed = -1;
  memset(band, COLOR_WHITE_INDEX, sizeof(band));
  pageOpen = true;
  Serial.printf("[INFO] Nueva pagina: %s\n", currentFileName);
}

void flushOneRow() {
  // Vuelca a la SD la fila mas antigua del buffer (bandBase) y la deja en blanco
  // para poder reutilizar ese hueco.
  if (!pageOpen) { bandBase++; return; }
  uint8_t rowRGB[PAGE_WIDTH_DOTS * 3];
  int32_t rel = bandBase % BAND_HEIGHT;
  if (rel < 0) rel += BAND_HEIGHT;
  for (int x = 0; x < PAGE_WIDTH_DOTS; x++) {
    uint8_t idx = band[rel][x];
    RGB c = (idx == COLOR_WHITE_INDEX) ? PALETTE[7] : PALETTE[idx];
    // BMP almacena en orden BGR
    rowRGB[x*3+0] = c.b;
    rowRGB[x*3+1] = c.g;
    rowRGB[x*3+2] = c.r;
  }
  uint32_t rs = rowSizeBytes();
  pageFile.write(rowRGB, PAGE_WIDTH_DOTS * 3);
  for (uint32_t p = PAGE_WIDTH_DOTS * 3; p < rs; p++) pageFile.write((uint8_t)0);

  memset(band[rel], COLOR_WHITE_INDEX, PAGE_WIDTH_DOTS);
  bandBase++;
  rowsWrittenToFile++;
}

void closePage() {
  if (!pageOpen) return;

  if (USE_XONXOFF) Serial2.write((uint8_t)0x13); // XOFF: puede tardar en escribir en SD

  // Volcar todo lo que quede en el buffer, hasta la ultima fila donde se pinto tinta
  // (no basta con cursorY: un caracter dibujado en la ultima linea ocupa varias
  // filas por debajo de la posicion del cursor).
  int32_t targetTop = max(maxYUsed + 1, bandBase);
  while (bandBase < targetTop) flushOneRow();

  // Corregir cabecera con el alto real (negativo = orden top-down) y el tamano
  uint32_t rs = rowSizeBytes();
  uint32_t fileSize = 54 + rs * rowsWrittenToFile;
  pageFile.seek(2);
  writeLE32(pageFile, fileSize);
  pageFile.seek(22);
  writeLE32(pageFile, (uint32_t)(-(int32_t)rowsWrittenToFile)); // alto negativo = top-down
  pageFile.close();

  Serial.printf("[INFO] Pagina cerrada: %s (%lu filas)\n", currentFileName, (unsigned long)rowsWrittenToFile);
  pageOpen = false;

  if (USE_XONXOFF) Serial2.write((uint8_t)0x11); // XON
}

void finishPageIfNeeded() {
  if (pageOpen && pageHasInk) {
    closePage();
  } else if (pageOpen) {
    // Pagina vacia: se descarta sin generar fichero util
    pageFile.close();
    SD.remove(currentFileName);
    pageOpen = false;
  }
}

// ================================ RENDERIZADO DE TEXTO ======================

#define CHAR_SCALE_X 2
#define CHAR_SCALE_Y 3
#define CHAR_ADVANCE ((FONT_COLS * CHAR_SCALE_X) + 2)
#define CHAR_HEIGHT  (FONT_ROWS * CHAR_SCALE_Y)

void drawChar(uint8_t c) {
  if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) c = ' ';
  const uint8_t *glyph = &font5x7[(c - FONT_FIRST_CHAR) * FONT_COLS];

  for (int col = 0; col < FONT_COLS; col++) {
    uint8_t colBits = pgm_read_byte(&glyph[col]);
    for (int row = 0; row < FONT_ROWS; row++) {
      if (colBits & (1 << row)) {
        for (int sy = 0; sy < CHAR_SCALE_Y; sy++) {
          for (int sx = 0; sx < CHAR_SCALE_X; sx++) {
            int32_t x = cursorX + col * CHAR_SCALE_X + sx;
            int32_t y = cursorY + row * CHAR_SCALE_Y + sy;
            plotDot(x, y, currentColor);
            if (boldMode) plotDot(x + 1, y, currentColor); // negrita: doble golpe desplazado
          }
        }
      }
    }
  }
  if (underlineMode) {
    int32_t y = cursorY + CHAR_HEIGHT - 1;
    for (int x = 0; x < CHAR_ADVANCE; x++) plotDot(cursorX + x, y, currentColor);
  }
}

// ================================ MAQUINA DE ESTADOS: PROCESADO =============

void resetPrinterState() {
  currentColor = 0;
  boldMode = false;
  underlineMode = false;
  lineSpacingDots = DEFAULT_LINE_DOTS;
  cursorX = 0;
  cursorY = 0;
}

void startGraphicsCapture(uint8_t pins, uint16_t dpi) {
  gfxPins = pins;
  gfxBytesPerCol = (pins == 24) ? 3 : 1;
  if (dpi > 180) {
    // Mas denso que nuestra rejilla de 180dpi (p.ej. 360dpi): se combinan
    // por OR varias columnas de origen en un unico punto de salida, en vez
    // de "estirar" la imagen avanzando 1 punto por cada columna de origen.
    gfxMergeFactor = (uint8_t)((dpi / 180.0) + 0.5);
    if (gfxMergeFactor < 1) gfxMergeFactor = 1;
    gfxStepDots = 1;
  } else {
    // Menos denso (60/90/120dpi): el cabezal golpea un punto y avanza mas
    // de 1 punto de nuestra rejilla antes del siguiente, dejando huecos
    // reales entre columnas (asi se comporta tambien la impresora fisica).
    gfxMergeFactor = 1;
    uint16_t step = (uint16_t)((180.0 / dpi) + 0.5);
    if (step < 1) step = 1;
    gfxStepDots = step;
  }
  gfxMergeCount = 0;
  memset(gfxMergeAccum, 0, sizeof(gfxMergeAccum));
}

void plotMergedColumn() {
  // gfxMergeAccum contiene 1 o 3 bytes ya combinados (MSB = aguja superior de cada byte)
  for (int b = 0; b < gfxBytesPerCol; b++) {
    uint8_t byteVal = gfxMergeAccum[b];
    for (int bit = 0; bit < 8; bit++) {
      if (byteVal & (0x80 >> bit)) {
        int pinIndex = b * 8 + bit;         // 0..7 (8 agujas) o 0..23 (24 agujas)
        int32_t y;
        if (gfxPins == 24) {
          y = cursorY + pinIndex;            // 1 punto por aguja a 180dpi: encaja exacto
        } else {
          y = cursorY + pinIndex * 3;        // 8 agujas a 1/60" = 3 puntos a 180dpi
        }
        plotDot(cursorX, y, currentColor);
      }
    }
  }
  cursorX += gfxStepDots;
  memset(gfxMergeAccum, 0, sizeof(gfxMergeAccum));
  gfxMergeCount = 0;
}

void plotGraphicsColumn() {
  // Se llama con 1 columna de origen ya recibida en gfxColBuf; se combina
  // en el acumulador y solo se "pinta" y se avanza X cuando el grupo de
  // fusion esta completo (grupo de 1 columna en los modos <=180dpi).
  for (int b = 0; b < gfxBytesPerCol; b++) gfxMergeAccum[b] |= gfxColBuf[b];
  gfxMergeCount++;
  gfxColIndex++;
  gfxColBufPos = 0;
  if (gfxMergeCount >= gfxMergeFactor) {
    plotMergedColumn();
  }
}

void handleByte(uint8_t b) {
  lastByteMillis = millis();

  // Si no hay pagina abierta, se abre de forma perezosa en cuanto llega algo
  if (!pageOpen) openNewPage();

  switch (escState) {

    case ST_NORMAL:
      if (b == 0x1B) { escState = ST_ESC; return; }
      switch (b) {
        case 0x0A: // LF
          cursorY += lineSpacingDots;
          return;
        case 0x0D: // CR
          cursorX = 0;
          return;
        case 0x0C: // FF
          finishPageIfNeeded();
          return;
        case 0x08: // BS
          cursorX -= CHAR_ADVANCE;
          if (cursorX < 0) cursorX = 0;
          return;
        case 0x09: { // HT: tabulador cada 8 celdas de caracter
          int32_t cell = CHAR_ADVANCE * 8;
          cursorX = ((cursorX / cell) + 1) * cell;
          return;
        }
        case 0x00: case 0x07: // NUL, BEL: se ignoran
          return;
        default:
          if (b >= 0x20 && b <= 0x7E) {
            drawChar(b);
            cursorX += CHAR_ADVANCE;
          }
          // bytes >= 0x80 (juegos de caracteres extendidos/acentos) no
          // soportados: se ignoran de forma segura en vez de imprimir basura.
          return;
      }

    case ST_ESC:
      switch (b) {
        case '@': resetPrinterState(); escState = ST_NORMAL; return;      // ESC @
        case '2': lineSpacingDots = DEFAULT_LINE_DOTS; escState = ST_NORMAL; return; // 1/6"
        case '0': lineSpacingDots = 23; escState = ST_NORMAL; return;     // 1/8" (aprox)
        case '3': escState = ST_ESC_3; return;                            // ESC 3 n
        case 'E': boldMode = true;  escState = ST_NORMAL; return;         // negrita ON
        case 'F': boldMode = false; escState = ST_NORMAL; return;         // negrita OFF
        case 'r': escState = ST_ESC_R; return;                            // color de cinta
        case '-': escState = ST_ESC_MINUS; return;                       // subrayado
        case '*': escState = ST_ESC_STAR_M; return;                      // grafico ESC *
        case 'K': startGraphicsCapture(8, 60);  escState = ST_ESC_KLYZ_NL; return;
        case 'L': startGraphicsCapture(8, 120); escState = ST_ESC_KLYZ_NL; return;
        case 'Y': startGraphicsCapture(8, 120); escState = ST_ESC_KLYZ_NL; return;
        case 'Z': startGraphicsCapture(8, 240); escState = ST_ESC_KLYZ_NL; return;
        default:
          // Comando ESC/P no soportado: se descarta un unico byte de posible
          // parametro para intentar no desincronizar el resto del flujo.
          escState = ST_ESC_SKIP1;
          return;
      }

    case ST_ESC_SKIP1:
      escState = ST_NORMAL;
      return;

    case ST_ESC_R:
      currentColor = (b <= 6) ? b : 0;
      escState = ST_NORMAL;
      return;

    case ST_ESC_3:
      lineSpacingDots = b; // ESC 3 n -> n/180" == n puntos a nuestra resolucion de 180dpi
      escState = ST_NORMAL;
      return;

    case ST_ESC_MINUS:
      underlineMode = (b != 0);
      escState = ST_NORMAL;
      return;

    case ST_ESC_STAR_M: {
      GfxMode gm = lookupStarMode(b);
      startGraphicsCapture(gm.pins, gm.dpi);
      escState = ST_ESC_STAR_NL;
      return;
    }
    case ST_ESC_STAR_NL:
      gfxNumCols = b;
      escState = ST_ESC_STAR_NH;
      return;
    case ST_ESC_STAR_NH:
      gfxNumCols |= ((uint16_t)b << 8);
      gfxColIndex = 0; gfxColBufPos = 0;
      escState = (gfxNumCols == 0) ? ST_NORMAL : ST_ESC_STAR_DATA;
      return;
    case ST_ESC_STAR_DATA:
      gfxColBuf[gfxColBufPos++] = b;
      if (gfxColBufPos >= gfxBytesPerCol) {
        plotGraphicsColumn();
        if (gfxColIndex >= gfxNumCols) {
          if (gfxMergeCount > 0) plotMergedColumn(); // grupo de fusion incompleto al final del bloque
          escState = ST_NORMAL;
        }
      }
      return;

    case ST_ESC_KLYZ_NL:
      gfxNumCols = b;
      escState = ST_ESC_KLYZ_NH;
      return;
    case ST_ESC_KLYZ_NH:
      gfxNumCols |= ((uint16_t)b << 8);
      gfxColIndex = 0; gfxColBufPos = 0;
      escState = (gfxNumCols == 0) ? ST_NORMAL : ST_ESC_KLYZ_DATA;
      return;
    case ST_ESC_KLYZ_DATA:
      gfxColBuf[gfxColBufPos++] = b;
      if (gfxColBufPos >= gfxBytesPerCol) {
        plotGraphicsColumn();
        if (gfxColIndex >= gfxNumCols) {
          if (gfxMergeCount > 0) plotMergedColumn(); // grupo de fusion incompleto al final del bloque
          escState = ST_NORMAL;
        }
      }
      return;
  }
}

// ================================ ARDUINO SETUP / LOOP ======================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[INFO] Emulador de impresora Epson LQ 24 agujas color - iniciando...");

  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("[ERROR] No se pudo montar la tarjeta SD. Revisa el cableado/CS.");
  } else {
    Serial.println("[INFO] Tarjeta SD montada correctamente.");
  }

  Serial2.setRxBufferSize(2048); // margen extra frente a rafagas mientras se escribe en la SD
  Serial2.begin(SERIAL_BAUD, SERIAL_8N1, SERIAL_RX_PIN, SERIAL_TX_PIN);

  memset(band, COLOR_WHITE_INDEX, sizeof(band));
  resetPrinterState();
  lastByteMillis = millis();

  Serial.println("[INFO] Esperando datos por el puerto serie (impresora)...");
}

void loop() {
  while (Serial2.available()) {
    uint8_t b = (uint8_t)Serial2.read();
    handleByte(b);
  }

  // Si llevamos un rato sin recibir nada y hay una pagina con contenido sin
  // cerrar (el host no envio Form Feed final), la cerramos igualmente.
  if (pageOpen && pageHasInk && (millis() - lastByteMillis > IDLE_TIMEOUT_MS)) {
    Serial.println("[INFO] Sin actividad: cerrando pagina automaticamente.");
    finishPageIfNeeded();
  }
}
