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
 * sin actividad, vuelca la pagina completa a un fichero BMP indexado de 4
 * bits (16 colores de paleta, formato BI_RGB sin comprimir) en la SD.
 *
 * ---------------------------------------------------------------------
 * HARDWARE ESPERADO (ajustar en la seccion CONFIGURACION):
 *   - ESP32 (cualquier variante con SPI libre para SD y un UART libre)
 *   - Lector de tarjeta SD por SPI
 *   - MAX232 (u otro transceptor RS-232) en un UART hardware (Serial2)
 *
 * ---------------------------------------------------------------------
 * LIMITACIONES CONOCIDAS (documentadas para no llevar a confusion):
 *   - La hoja tiene una altura FIJA (PAGE_HEIGHT_DOTS, por defecto 2043
 *     puntos ~ hoja A4 a 180dpi): un Form Feed a mitad de hoja rellena de
 *     blanco lo que falte, y si el contenido llega al final sin recibir
 *     Form Feed, la pagina se cierra sola (igual que con un Form Feed) y
 *     se abre una nueva automaticamente. Esto implica que TODOS los BMP
 *     generados pesan lo mismo (PAGE_WIDTH_DOTS x PAGE_HEIGHT_DOTS, sin
 *     comprimir) independientemente de cuanto se haya impreso realmente.
 *   - Resolucion interna fija a 180 dpi tanto en horizontal como en
 *     vertical. Los modos de 360 dpi (ESC * m=40/72) se aceptan pero se
 *     "diezman" a 180 dpi (se descarta una columna de cada dos).
 *   - No se implementa avance de papel hacia atras (no existe en ESC/P
 *     estandar salvo microjustificacion, que tampoco se soporta), por lo
 *     que la imagen se genera en una unica pasada, de arriba a abajo.
 *   - Se soporta un subconjunto practico de ESC/P: inicializacion,
 *     avance de linea/pagina, interlineado (ESC 2 / ESC 0 / ESC 3 n),
 *     graficos de puntos (ESC K/L/Y/Z y ESC *), negrita/doble-golpe
 *     (ESC E/F/G/H), subrayado (ESC - n), color de cinta (ESC r n),
 *     tipo de letra (ESC k n), calidad borrador/NLQ (ESC x n), paso
 *     pica/elite/15cpi (ESC P/M/g), condensado (SI/DC2), cursiva
 *     (ESC 4/5), ancho doble (ESC W n), alto doble/cuadruple (ESC w n,
 *     ver aviso debajo) y super/subindice (ESC S/T).
 *     Los caracteres definidos por el usuario, tabulaciones verticales,
 *     microavances (ESC J), formato de pagina, etc. no estan
 *     implementados; los bytes de parametro de secuencias no
 *     reconocidas se descartan de forma conservadora (y ahora ademas
 *     generan una linea de log "[DEBUG] Comando ESC no soportado" con
 *     el caracter recibido, para poder detectarlas facilmente) para no
 *     desincronizar el interprete.
 *   - "ESC w n" (alto doble/cuadruple, n=0 normal/1 doble/2 cuadruple) NO
 *     es un comando ESC/P original de Epson: el ESC/P clasico de
 *     impresoras matriciales no tiene uno, porque una cabeza de agujas
 *     fisica no puede imprimir mas alto en una sola pasada (por eso ni
 *     siquiera el "Master Select", ESC !, lo incluye, aunque si incluye
 *     el ancho doble). Aqui SI es trivial al ser software, asi que se ha
 *     anadido como extension propia del emulador, simetrica a ESC W. Si
 *     el host usa otra secuencia para esto, aparecera en el log de
 *     depuracion de comandos no soportados y se puede remapear facil.
 *   - Los "tipos de letra" son una aproximacion honesta, no una copia
 *     fiel: solo se dispone de dos fuentes de puntos de dominio publico
 *     verificadas (una de 5x7 tipo palo seco y otra de 8x8 tipo
 *     VGA/maquina de escribir). Las 7 tipografias de ESC k n se reparten
 *     entre esas dos familias reales (ver drawChar()/chooseFont() para
 *     el detalle exacto), y en modo borrador siempre se usa la de 5x7,
 *     igual que hacian las impresoras reales.
 *   - La paleta de color es la tipica de cinta de 4 bandas de estas
 *     impresoras (Negro/Cian/Magenta/Amarillo + combinaciones), no un
 *     RGB continuo real.
 *
 * Autor: (C) Alvaro Alea Fdz. distribuido bajo licencia GPL 3 o superior.
 * ==========================================================================
 */

#include <SPI.h>
#include <SD.h>
#include <Adafruit_NeoPixel.h> // Libreria "Adafruit NeoPixel" (instalar desde el Gestor de Librerias si falta)
#include <freertos/FreeRTOS.h> // mutex de la SD (sdMutex) -- incluidas en el nucleo ESP32 de Arduino
#include <freertos/semphr.h>
#include "font5x7.h"
#include "font8x8.h"
/**/
#include "roman.h"
#include "courier.h"
#include "prestige.h"
#include "script.h"
#include "OCRB.h"
#include "OCRA.h"
/**/
// ================================ CONFIGURACION ===========================

// --- Tarjeta SD (SPI) --- (pines para ESP32-S3; el S3 no tiene un mapeo
// VSPI/HSPI fijo como el ESP32 clasico, asi que hay que indicarlos a mano)
#define SD_CS_PIN     10     // Chip Select de la SD
#define SD_MOSI_PIN   11
#define SD_SCK_PIN    12
#define SD_MISO_PIN   13
#define SD_SPI_FREQ_HZ 24000000UL // 24MHz: bastante mas rapido que el por defecto
                                   // (4MHz). Si da errores de lectura/escritura
                                   // (cableado largo o mala calidad), bajalo.

// --- Puerto serie hacia el MAX232 (Serial2 = UART2 del ESP32) ---
#define SERIAL_TX_PIN 17
#define SERIAL_RX_PIN 18
#define SERIAL_RTS_PIN 15   // RTS de salida: avisa al host cuando NO debe seguir enviando
#define SERIAL_CTS_PIN 16   // CTS de entrada: el host nos dice cuando puede recibir (no usado al imprimir)
#define SERIAL_BAUD   9600   // Velocidad del puerto serie de la impresora
#define USE_HW_FLOW_CONTROL true  // Control de flujo por hardware RTS/CTS (preferido: hay pines cableados)
#define USE_XONXOFF   false  // Control de flujo por software (alternativa si no se cablean RTS/CTS)

// --- Geometria de la pagina virtual (rejilla interna a 180 dpi) ---
#define PAGE_WIDTH_DOTS   1440   // 1440/180 = 8.0 pulgadas de ancho de impresion
#define PAGE_HEIGHT_DOTS  2043   // Alto FIJO de cada pagina (aprox. una hoja A4 a 180dpi).
                                  // Cada BMP generado mide siempre PAGE_WIDTH_DOTS x
                                  // PAGE_HEIGHT_DOTS: un Form Feed (o el cierre automatico
                                  // al llegar al final de la hoja) rellena de blanco lo que
                                  // falte, y si el contenido llega a esta altura sin recibir
                                  // Form Feed, la pagina se cierra sola y se abre una nueva.

// --- Cache en PSRAM para el servidor web (ver wifi_web.ino) ---
// Con el formato indexado de 4 bits, una pagina completa (PAGE_WIDTH_DOTS x
// PAGE_HEIGHT_DOTS) ocupa ~1.4MB -- cabe de sobra en los 8MB de PSRAM de un
// ESP32-S3 N16R8 (antes, en 24 bits sin comprimir, eran ~8.4MB y NO cabia
// una pagina entera; de ahi que esto sea un "presupuesto" y no el tamano
// de la pagina). Mientras se imprime, cada fila que se vuelca a la SD se
// copia tambien aqui (si hay hueco); el servidor web sirve directamente
// desde esta cache lo que quepa en ella y solo recurre a leer de la SD
// para el resto (en la practica, con 2MB de presupuesto, nunca hace falta
// para una pagina de tamano normal). Si no hay PSRAM en la placa, esto se
// desactiva solo y todo se sirve desde la SD como antes.
#define WEB_BMP_CACHE_MAX_BYTES (2UL * 1024 * 1024) // 2MB: cubre una pagina entera de sobra
#define BAND_HEIGHT       48     // Alto del buffer de bandas en filas (RAM ~ 1440*48 bytes)
#define DEFAULT_LINE_DOTS 30     // 1/6" a 180dpi = 30 puntos (interlineado por defecto)

// Equivalente por software a los DIP-switch "Auto LF" / "Auto CR" de una
// Epson real. Muchos ordenadores antiguos solo mandan CR (0x0D) al final de
// linea y esperan que la propia impresora añada el salto de linea (o al
// reves: solo mandan LF esperando que la impresora vuelva al margen). Si al
// imprimir el papel no avanza (todo se solapa en la misma linea), prueba a
// poner AUTO_LF_ON_CR en true; si en cambio el texto no vuelve al margen
// izquierdo entre lineas, prueba AUTO_CR_ON_LF.
#define AUTO_LF_ON_CR  false  // CR (0x0D) tambien hace un salto de linea
#define AUTO_CR_ON_LF  true   // LF (0x0A) tambien vuelve al margen izquierdo
#define IDLE_TIMEOUT_MS   600000UL  // 10 minutos. millis() es un unsigned long de 32 bits
                                     // (desborda a los ~49.7 dias), asi que 10 minutos caben
                                     // de sobra sin ningun problema; no hace falta recortarlo.

// --- Boton de cierre manual de pagina ---
// Boton con pull-up EXTERNO a la patilla 46 (no se usa INPUT_PULLUP porque
// hay resistencia externa): en reposo la patilla esta en HIGH y al pulsar
// el boton la lleva a GND (LOW).
#define BUTTON_PIN         46
#define BUTTON_DEBOUNCE_MS 50

// --- LED RGB de estado ---
// LED direccionable (tipo WS2812/NeoPixel, un solo pin de datos) en la
// patilla 47. Colores: verde = sin pagina empezada, azul = pagina
// empezada (en espera), morado = recibiendo datos por el puerto serie,
// amarillo = destello breve al pulsar el boton de cierre (Form Feed
// manual), rojo = problema con la tarjeta SD.
#define RGB_LED_PIN        48
#define RGB_LED_BRIGHTNESS 0.15f  // 15% en los 3 colores
#define RECEIVING_HOLD_MS  150UL  // cuanto se mantiene "morado" tras el ultimo byte recibido
#define BUTTON_FLASH_MS    400UL  // duracion del destello amarillo al pulsar el boton
#define SD_RETRY_INTERVAL_MS 5000UL // cada cuanto se reintenta remontar la SD si fallo

// ================================ PALETA DE COLOR ==========================
// Aproximacion de los colores tipicos de una cinta de 4 bandas (K/C/M/Y)
// tal y como los seleccionaba el comando ESC r n de las Epson JX-80/LQ color.
struct RGB { uint8_t r, g, b; };

// Declarado aqui (y no junto a chooseFont()/drawChar() mas abajo) porque el
// IDE de Arduino genera automaticamente los prototipos de las funciones y
// los inserta al principio del fichero: si el struct se definiera despues,
// esos prototipos no lo conocerian todavia y la compilacion fallaria con
// "'FontChoice' does not name a type".
struct FontChoice { const uint8_t *data; uint8_t cols; uint8_t rows; uint8_t firstChar; uint8_t lastChar; uint8_t baseScaleY; };
struct GfxMode { uint8_t pins; uint16_t dpi; }; // ver el comentario de FontChoice: debe ir aqui, antes de cualquier funcion

// Declaraciones adelantadas: el codigo de escritura en SD (mas abajo) necesita
// avisar al LED en el momento exacto en que empieza/termina a escribir, pero
// las funciones del LED estan definidas mas adelante en el fichero.
void sdWriteBegin();
void sdWriteEnd();
void sdAccessBegin();
void sdAccessEnd();
void updateStatusLed();
void patchHeaderNow();

// Definidas en wifi_web.ino (otra pestana del MISMO sketch, debe estar en
// la misma carpeta): WiFi con portal de configuracion, OTA y servidor web.
// Corren en su PROPIA tarea de FreeRTOS (ver startWifiWebTask()) para que
// el autoConnect() bloqueante de WiFiManager no retrase el arranque de la
// emulacion de impresora ni un segundo.
void startWifiWebTask();
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

// --- Formato de fichero: BMP INDEXADO de 4 bits (16 colores de paleta, de
// los que se usan 8) en vez de BMP de 24 bits sin comprimir. Mismo aspecto
// visual exacto (la paleta es la misma de siempre), pero cada pixel ocupa
// medio byte en vez de tres: los ficheros quedan 6 veces mas pequenos, lo
// que ademas acelera mucho servirlos por el servidor web.
#define BMP_PALETTE_COLORS 16                              // minimo que admite el formato "4bpp" de BMP
#define BMP_HEADER_BYTES (54 + BMP_PALETTE_COLORS * 4)      // cabecera + tabla de color (54 + 64 = 118)

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

// --- Estado tipografico (ver seccion RENDERIZADO DE TEXTO para el detalle) ---
uint8_t typefaceMode = 1;   // seleccionado por ESC k n (0 Roman,1 Sans Serif,2 Courier,3 Prestige,4 Script,5 OCR-B,6 OCR-A)
bool    lqMode       = false; // ESC x n : false=borrador (draft), true=NLQ/calidad carta
uint8_t pitchMode    = 0;    // ESC P/M/g : 0=pica(10cpi) 1=elite(12cpi) 2=15cpi
bool    condensedMode = false; // SI (0x0F) / DC2 (0x12)
bool    italicMode    = false; // ESC 4 / ESC 5
bool    doubleWidthMode = false; // ESC W n
uint8_t heightMultiplier = 1;    // ESC w n (extension propia, ver aviso en la respuesta): 1, 2 o 4
uint8_t scriptMode    = 0;   // ESC S n / ESC T : 0=normal 1=superindice 2=subindice

unsigned long lastByteMillis = 0;
bool everReceivedByte = false;  // evita que el LED muestre "recibiendo" antes del primer byte real

bool sdOk = true;               // false si el montaje inicial o la apertura de un fichero fallan
unsigned long lastSdRetryMillis = 0;
bool writingToSD = false;       // true mientras hay una escritura fisica en curso en la SD

// Cache en PSRAM de la pagina EN CURSO (ver comentario de WEB_BMP_CACHE_MAX_BYTES
// arriba). bmpCacheBytes = cuantos bytes del fichero actual estan reflejados
// en la cache ahora mismo (desde el byte 0); nunca mas que bmpCacheCapacity.
uint8_t *bmpCacheBuffer = nullptr;
uint32_t bmpCacheCapacity = 0;
uint32_t bmpCacheBytes = 0;

bool buttonFlashActive = false;
unsigned long buttonFlashStartMillis = 0;

Adafruit_NeoPixel rgbLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
uint32_t lastLedColorSet = 0xFFFFFFFF; // sentinela invalido para forzar el primer show()

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
  ST_ESC_K,        // ESC k n           (tipo de letra)
  ST_ESC_X,        // ESC x n           (calidad borrador/NLQ)
  ST_ESC_W,        // ESC W n           (ancho doble on/off)
  ST_ESC_LOWER_W,  // ESC w n           (alto doble/cuadruple -- extension propia, no ESC/P original)
  ST_ESC_S,        // ESC S n           (superindice/subindice)
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
  if (y < bandBase + BAND_HEIGHT) return; // caso normal: no hace falta escribir nada
  sdWriteBegin();
  while (y >= bandBase + BAND_HEIGHT) {
    flushOneRow();
  }
  patchHeaderNow(); // la cabecera refleja ya las filas recien volcadas
  sdWriteEnd();
}

int32_t maxYUsed = -1; // fila mas baja en la que se ha pintado algo desde que se abrio la pagina

void plotDot(int32_t x, int32_t y, uint8_t colorIndex) {
  if (x < 0 || x >= PAGE_WIDTH_DOTS || y < 0 || y >= PAGE_HEIGHT_DOTS) return;
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
  uint32_t raw = ((uint32_t)PAGE_WIDTH_DOTS + 1) / 2; // 4 bits/pixel = 2 pixeles por byte
  return (raw + 3) & ~((uint32_t)3); // redondeo a multiplo de 4
}

// Deja la cabecera BMP (alto y tamano de fichero, en los offsets 22 y 2)
// coherente con lo que hay REALMENTE escrito en la SD en este momento
// (rowsWrittenToFile filas completas), no con lo que se espera llegar a
// escribir. Se llama tanto tras cada tanda de filas volcadas durante la
// impresion como al cerrar la pagina, de forma que si alguien abre el
// fichero a medio generar, encuentra siempre un BMP valido y autoconsistente
// (ancho x filas-escritas-hasta-ahora), nunca una cabecera a 0 ni una que
// prometa mas filas de las que hay fisicamente en el fichero.
// Copia 'len' bytes al final de la cache PSRAM (si hay hueco) empezando en
// la posicion bmpCacheBytes, y avanza bmpCacheBytes. Si no hay cache o ya no
// queda sitio, no hace nada (la pagina sigue funcionando igual, sin cache
// para esa parte: el servidor web recurrira a la SD para lo que falte).
void cacheAppend(const uint8_t *data, uint32_t len) {
  if (!bmpCacheBuffer) return;
  if (bmpCacheBytes + len > bmpCacheCapacity) return;
  memcpy(bmpCacheBuffer + bmpCacheBytes, data, len);
  bmpCacheBytes += len;
}

// Reescribe 4 bytes ya presentes en la cache (para mantener los campos de
// alto/tamano de la cabecera cacheada en sincronia con patchHeaderNow()).
void cachePatchU32(uint32_t offset, uint32_t value) {
  if (!bmpCacheBuffer) return;
  if (offset + 4 > bmpCacheBytes) return; // esos bytes aun no estan cacheados
  bmpCacheBuffer[offset + 0] = (uint8_t)(value & 0xFF);
  bmpCacheBuffer[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
  bmpCacheBuffer[offset + 2] = (uint8_t)((value >> 16) & 0xFF);
  bmpCacheBuffer[offset + 3] = (uint8_t)((value >> 24) & 0xFF);
}

void patchHeaderNow() {
  uint32_t rs = rowSizeBytes();
  uint32_t endPos = BMP_HEADER_BYTES + rs * rowsWrittenToFile; // fin real de los datos escritos hasta ahora
  pageFile.seek(2);
  writeLE32(pageFile, endPos); // tamano de fichero = lo que hay escrito de verdad
  pageFile.seek(22);
  writeLE32(pageFile, (uint32_t)(-(int32_t)rowsWrittenToFile)); // alto negativo (top-down) = filas escritas hasta ahora
  pageFile.flush(); // que quede fisicamente en la SD antes de que alguien mas lo lea
  pageFile.seek(endPos); // IMPRESCINDIBLE: volver al final real para poder seguir anexando filas

  cachePatchU32(2, endPos);
  cachePatchU32(22, (uint32_t)(-(int32_t)rowsWrittenToFile));
}

void openNewPage() {
  sdAccessBegin(); // bloquea la SD para toda la funcion (busqueda de nombre + apertura + cabecera)

  // pageIndex es un contador en RAM que arranca de 0 en cada reinicio del
  // ESP32, pero los ficheros de sesiones anteriores siguen en la SD. Para no
  // sobrescribirlos, se busca el primer nombre libre a partir de pageIndex+1.
  do {
    pageIndex++;
    snprintf(currentFileName, sizeof(currentFileName), "/PAGE%04lu.BMP", (unsigned long)pageIndex);
  } while (SD.exists(currentFileName) && pageIndex < 9999);

  if (SD.exists(currentFileName)) {
    // No deberia ocurrir salvo que la SD ya tenga las 9999 paginas usadas.
    Serial.println("[ERROR] No se encontro un nombre de pagina libre (PAGE0001..PAGE9999.BMP agotados).");
    pageOpen = false;
    sdAccessEnd();
    return;
  }
  pageFile = SD.open(currentFileName, FILE_WRITE);
  if (!pageFile) {
    Serial.printf("[ERROR] No se pudo crear %s en la SD\n", currentFileName);
    pageOpen = false;
    sdOk = false;
    sdAccessEnd();
    updateStatusLed(); // reflejar el fallo de inmediato, sin esperar al loop()
    return;
  }

  // Resto de la funcion: ya dentro de la zona de escritura (indicador LED
  // incluido). writingToSD ya estaba implicitamente "en curso" desde el
  // sdAccessBegin() de arriba, pero el LED solo refleja "escribiendo" a
  // partir de aqui para no mostrar celeste durante la mera busqueda de
  // nombre, que es casi instantanea.
  writingToSD = true;
  updateStatusLed();

  // --- Cabecera BMP indexada de 4 bits (BMP_HEADER_BYTES = 118 bytes:
  // 54 de cabecera + 64 de tabla de color), construida en RAM y escrita de
  // una sola vez (una sola transaccion SPI en vez de muchas escrituras
  // sueltas). Alto y tamano se dejan primero a 0 y los corrige
  // patchHeaderNow() justo debajo, que en este punto (0 filas escritas) los
  // deja en su valor coherente real: tamano=BMP_HEADER_BYTES, alto=0.
  uint8_t header[BMP_HEADER_BYTES];
  memset(header, 0, sizeof(header));
  header[0] = 'B'; header[1] = 'M';
  // bytes 2..5 (tamano) y 10..13 (offset a los pixeles) se fijan abajo
  header[10] = (uint8_t)(BMP_HEADER_BYTES & 0xFF); // offset a los datos de pixel
  header[11] = (uint8_t)((BMP_HEADER_BYTES >> 8) & 0xFF);
  header[14] = 40; // tamano de BITMAPINFOHEADER
  header[18] = (uint8_t)(PAGE_WIDTH_DOTS & 0xFF);
  header[19] = (uint8_t)((PAGE_WIDTH_DOTS >> 8) & 0xFF);
  header[20] = (uint8_t)((PAGE_WIDTH_DOTS >> 16) & 0xFF);
  header[21] = (uint8_t)((PAGE_WIDTH_DOTS >> 24) & 0xFF);
  // bytes 22..25 (alto) se fijan abajo
  header[26] = 1; // planos
  header[28] = 4; // bits por pixel: 4 (16 colores de paleta, indexado)
  // bytes 30..33 (compresion BI_RGB) y 34..37 (tamano de imagen) ya son 0
  header[38] = 0x12; header[39] = 0x0B; // ~2834 pixeles/metro (180dpi), X
  header[42] = 0x12; header[43] = 0x0B; // idem, Y
  // bytes 46..49 (colores en la paleta) y 50..53 (colores importantes) se
  // dejan a 0, que para un bitmap indexado significa "el maximo del bit
  // depth" (16), convencion estandar.

  // Tabla de color (16 entradas x 4 bytes BGR0), justo despues de los 54
  // bytes de cabecera. Las primeras 8 son la paleta real de la cinta; las
  // 8 restantes no se usan (se dejan a 0, ya estan a 0 por el memset).
  for (int i = 0; i < 8; i++) {
    uint8_t *entry = &header[54 + i * 4];
    entry[0] = PALETTE[i].b;
    entry[1] = PALETTE[i].g;
    entry[2] = PALETTE[i].r;
    entry[3] = 0;
  }

  pageFile.write(header, sizeof(header));

  bandBase = 0;
  cursorX = 0;
  cursorY = 0;
  rowsWrittenToFile = 0;
  pageHasInk = false;
  maxYUsed = -1;
  memset(band, COLOR_WHITE_INDEX, sizeof(band));
  pageOpen = true;

  bmpCacheBytes = 0; // la cache PSRAM empieza de cero para cada pagina nueva
  cacheAppend(header, sizeof(header));

  patchHeaderNow(); // deja la cabecera (en la SD y en la cache) coherente: BMP_HEADER_BYTES/0

  writingToSD = false;
  updateStatusLed();
  sdAccessEnd();

  Serial.printf("[INFO] Nueva pagina: %s\n", currentFileName);
}

void flushOneRow() {
  // Vuelca a la SD la fila mas antigua del buffer (bandBase) y la deja en blanco
  // para poder reutilizar ese hueco.
  if (!pageOpen) { bandBase++; return; }
  uint32_t rs = rowSizeBytes();
  uint8_t rowBuf[PAGE_WIDTH_DOTS / 2 + 4]; // +4: margen para el relleno a multiplo de 4
  memset(rowBuf, 0, sizeof(rowBuf));
  int32_t rel = bandBase % BAND_HEIGHT;
  if (rel < 0) rel += BAND_HEIGHT;
  for (int x = 0; x < PAGE_WIDTH_DOTS; x++) {
    uint8_t idx = band[rel][x];
    if (idx == COLOR_WHITE_INDEX) idx = 7; // blanco = entrada 7 de la paleta
    // 2 pixeles por byte: el primero (x par) va en el nibble alto, el
    // segundo (x impar) en el nibble bajo -- orden estandar de BMP 4bpp.
    if (x & 1) rowBuf[x / 2] |= (idx & 0x0F);
    else       rowBuf[x / 2] |= (idx & 0x0F) << 4;
  }
  // el resto de rowBuf (relleno a multiplo de 4) ya quedo a 0 por el memset

  pageFile.write(rowBuf, rs); // una unica escritura, en vez de datos+relleno por separado
  cacheAppend(rowBuf, rs);

  memset(band[rel], COLOR_WHITE_INDEX, PAGE_WIDTH_DOTS);
  bandBase++;
  rowsWrittenToFile++;
}

void closePage() {
  if (!pageOpen) return;

  if (USE_XONXOFF) Serial2.write((uint8_t)0x13); // XOFF: puede tardar en escribir en SD

  sdWriteBegin();

  // Volcar todo lo que quede en el buffer y, ademas, rellenar de blanco el
  // resto de la hoja hasta la altura fija PAGE_HEIGHT_DOTS: asi todas las
  // paginas generadas miden siempre lo mismo (aspecto de hoja A4), tanto si
  // el cierre lo provoca un Form Feed a mitad de hoja como si lo provoca
  // haber llegado justo al final.
  while (bandBase < PAGE_HEIGHT_DOTS) flushOneRow();

  patchHeaderNow(); // cabecera final: alto y tamano coherentes con las PAGE_HEIGHT_DOTS filas ya escritas
  pageFile.close();

  sdWriteEnd();

  Serial.printf("[INFO] Pagina cerrada: %s (%lu filas)\n", currentFileName, (unsigned long)rowsWrittenToFile);
  pageOpen = false;

  if (USE_XONXOFF) Serial2.write((uint8_t)0x11); // XON
}

void finishPageIfNeeded() {
  if (pageOpen && pageHasInk) {
    closePage();
  } else if (pageOpen) {
    // Pagina vacia: se descarta sin generar fichero util
    sdWriteBegin();
    pageFile.close();
    SD.remove(currentFileName);
    pageOpen = false;
    sdWriteEnd();
  }
}

// ================================ RENDERIZADO DE TEXTO ======================
//
// NOTA IMPORTANTE sobre los "tipos de letra": una impresora matricial LQ real
// tenia varias familias (Roman, Sans Serif, Courier, Prestige, Script,
// OCR-A, OCR-B) con sus propios juegos de puntos internos. Aqui solo se
// dispone de dos fuentes de puntos verificadas y de dominio publico: una de
// 5x7 (aspecto de palo seco, "Sans Serif") y otra de 8x8 (aspecto mas denso,
// tipo VGA/maquina de escribir). El comando ESC k n se interpreta y se
// recuerda correctamente, pero las 7 tipografias se reparten entre estas
// DOS familias reales como aproximacion visual, no como reproduccion fiel de
// cada tipografia Epson original. En modo borrador (draft) se usa siempre la
// fuente 5x7, tal y como hacian las propias impresoras (el modo borrador no
// distinguia tipografias).

uint16_t computeAdvanceDots() {
  float cpi;
  if (condensedMode) cpi = (pitchMode == 1) ? 20.0f : 17.14f; // condensada elite / pica
  else if (pitchMode == 0) cpi = 10.0f;   // pica
  else if (pitchMode == 1) cpi = 12.0f;   // elite
  else cpi = 15.0f;                       // ESC g
  uint16_t advance = (uint16_t)((180.0f / cpi) + 0.5f);
  if (doubleWidthMode) advance *= 2;
  if (advance < 4) advance = 4;
  return advance;
}

/*
FontChoice chooseFont() {
  if (!lqMode) {
    return { font5x7, FONT_COLS, FONT_ROWS, FONT_FIRST_CHAR, FONT_LAST_CHAR, 3 };
  }
  switch (typefaceMode) {
    case 1: // Sans Serif
      return { font5x7, FONT_COLS, FONT_ROWS, FONT_FIRST_CHAR, FONT_LAST_CHAR, 3 };
    default: // Roman, Courier, Prestige, Script, OCR-B, OCR-A -> familia de 8x8
      return { font5x7, FONT_COLS, FONT_ROWS, FONT_FIRST_CHAR, FONT_LAST_CHAR, 3 };
  }
}
*/
FontChoice chooseFont() {
  if (!lqMode) {
    return { font5x7, FONT_COLS, FONT_ROWS, FONT_FIRST_CHAR, FONT_LAST_CHAR, 3 };
  }
  switch (typefaceMode) {
    case 0: // Roman
      return { fontroman, FONT0_COLS, FONT0_ROWS, FONT0_FIRST_CHAR, FONT0_LAST_CHAR, 3 };
    case 1: // Sans Serif
      return { font8x8, FONT1_COLS, FONT1_ROWS, FONT1_FIRST_CHAR, FONT1_LAST_CHAR, 3 };
    case 2: // Courier
      return { fontcourier, FONT2_COLS, FONT2_ROWS, FONT2_FIRST_CHAR, FONT2_LAST_CHAR, 3 };
    case 3: // Prestige
      return { fontprestige, FONT3_COLS, FONT3_ROWS, FONT3_FIRST_CHAR, FONT3_LAST_CHAR, 3 };
    case 4: // Script
      return { fontscript, FONT4_COLS, FONT4_ROWS, FONT4_FIRST_CHAR, FONT4_LAST_CHAR, 3 };
    case 5: // OCR-B
      return { fontocrb, FONT5_COLS, FONT5_ROWS, FONT5_FIRST_CHAR, FONT5_LAST_CHAR, 3 };
    case 6: // OCR-A
      return { fontocra, FONT6_COLS, FONT6_ROWS, FONT6_FIRST_CHAR, FONT6_LAST_CHAR, 3 };
    default: // Roman, Courier, Prestige, Script, OCR-B, OCR-A -> familia de 8x8
      return { font5x7, FONT_COLS, FONT_ROWS, FONT_FIRST_CHAR, FONT_LAST_CHAR, 3 };
  }
}
/**/

void drawChar(uint8_t c) {
  FontChoice fc = chooseFont();
  if (c < fc.firstChar || c > fc.lastChar) c = ' ';
  const uint8_t *glyph = fc.data + (uint32_t)(c - fc.firstChar) * fc.cols;

  uint16_t advance = computeAdvanceDots();
  uint16_t usable = (advance > 2) ? (advance - 2) : advance;
  uint8_t scaleX = usable / fc.cols;
  if (scaleX < 1) scaleX = 1;

  uint8_t scaleY = fc.baseScaleY * heightMultiplier; // ESC w: doble/cuadruple alto (extension propia)
  if (scriptMode != 0) { scaleY = (uint8_t)((scaleY * 2) / 3); if (scaleY < 1) scaleY = 1; }

  uint16_t fullHeight = fc.rows * fc.baseScaleY * heightMultiplier; // alto normal (sin super/subindice, con doble/cuadruple ya aplicado)
  uint16_t thisHeight = fc.rows * scaleY;
  int32_t yBase = cursorY;
  if (scriptMode == 2) yBase = cursorY + (fullHeight - thisHeight); // subindice: alineado abajo

  // Cursiva por cizalladura continua: cada fila del caracter se desplaza un
  // poco mas hacia la derecha cuanto mas arriba esta (fila 0 = la de mas
  // desplazamiento, la ultima fila = sin desplazamiento), dando el lean
  // tipico de la cursiva. IMPORTANTE (ver aviso en la respuesta): la
  // version anterior dividia esto entre 2, lo que dejaba un desplazamiento
  // maximo de 2-3 puntos sobre un caracter de ~21 puntos de alto: tecnicamente
  // se aplicaba, pero era casi imperceptible a simple vista. Aqui se usa el
  // recorrido completo (sin dividir) para que se note claramente.
  uint8_t italicShearMax = fc.rows - 1;

  // Efecto de "bandas" del modo borrador: al expandir cada punto de la
  // fuente a un bloque de scaleY filas, en borrador solo se pinta la mitad
  // superior de ese bloque (dejando la mitad inferior en blanco), que es el
  // aspecto rayado tipico de una impresora matricial en draft. En modo NLQ
  // se pinta el bloque completo, como hasta ahora.
  uint8_t paintRows = lqMode ? scaleY : (uint8_t)max(1, scaleY / 2);

  for (int col = 0; col < fc.cols; col++) {
    uint8_t colBits = pgm_read_byte(&glyph[col]);
    for (int row = 0; row < fc.rows; row++) {
      if (!(colBits & (1 << row))) continue;
      int8_t xShear = italicMode ? (int8_t)(italicShearMax - row) : 0;
      for (int sy = 0; sy < paintRows; sy++) {
        for (int sx = 0; sx < scaleX; sx++) {
          int32_t x = cursorX + col * scaleX + sx + xShear;
          int32_t y = yBase + row * scaleY + sy;
          plotDot(x, y, currentColor);
          if (boldMode) plotDot(x + 1, y, currentColor); // negrita: doble golpe desplazado
        }
      }
    }
  }
  if (underlineMode) {
    int32_t y = cursorY + fullHeight - 1; // el subrayado siempre va en la linea base normal
    for (int x = 0; x < advance; x++) {
      int32_t absX = cursorX + x;
      // En borrador, igual que el resto del texto, las agujas golpean un
      // punto si y otro no (ver paintRows mas arriba): el subrayado debe
      // ser coherente con eso, una linea de puntos en vez de continua. Se
      // usa la coordenada ABSOLUTA de la pagina (no "x", que es local a
      // este caracter) para que el patron de puntos siga alineado de un
      // caracter al siguiente sea cual sea el paso (CPI) actual, en vez de
      // reiniciarse en cada salto de caracter.
      if (!lqMode && (absX & 1)) continue;
      plotDot(absX, y, currentColor);
    }
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
  typefaceMode = 1;
  lqMode = false;
  pitchMode = 0;
  condensedMode = false;
  italicMode = false;
  doubleWidthMode = false;
  heightMultiplier = 1;
  scriptMode = 0;
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

// Muchos programas y sistemas antiguos no mandan los parametros de tipo
// "on/off" o de enumeracion pequena (0-9) como el valor binario crudo, sino
// como el digito ASCII correspondiente ('0'-'9' = 0x30-0x39), o incluso con
// el bit 7 puesto (0xB0-0xB9), algo habitual en algunos ordenadores de 8
// bits. Se aceptan las tres formas indistintamente: 0x00/0x30/0xB0 -> 0,
// 0x01/0x31/0xB1 -> 1, etc.
uint8_t decodeNumericParam(uint8_t b) {
  if (b >= 0x30 && b <= 0x39) return b - 0x30; // digito ASCII ('0'-'9')
  if (b >= 0xB0 && b <= 0xB9) return b - 0xB0; // digito ASCII con el bit 7 puesto
  return b;                                    // valor binario crudo
}

void handleByteInner(uint8_t b) {
  lastByteMillis = millis();
  everReceivedByte = true;

  // Si no hay pagina abierta, se abre de forma perezosa en cuanto llega algo
  if (!pageOpen) openNewPage();

  switch (escState) {

    case ST_NORMAL:
      if (b == 0x1B) { escState = ST_ESC; return; }
      switch (b) {
        case 0x0A: // LF
          cursorY += lineSpacingDots;
          if (AUTO_CR_ON_LF) cursorX = 0;
          return;
        case 0x0D: // CR
          cursorX = 0;
          if (AUTO_LF_ON_CR) cursorY += lineSpacingDots;
          return;
        case 0x0C: // FF
          finishPageIfNeeded();
          return;
        case 0x08: // BS
          cursorX -= computeAdvanceDots();
          if (cursorX < 0) cursorX = 0;
          return;
        case 0x09: { // HT: tabulador cada 8 celdas de caracter
          int32_t cell = computeAdvanceDots() * 8;
          cursorX = ((cursorX / cell) + 1) * cell;
          return;
        }
        case 0x0F: // SI: activa condensado (tambien existe como ESC SI)
          condensedMode = true;
          return;
        case 0x12: // DC2: cancela condensado
          condensedMode = false;
          return;
        case 0x00: case 0x07: // NUL, BEL: se ignoran
          return;
        default:
          if (b >= 0x20 && b <= 0x7E) {
            drawChar(b);
            cursorX += computeAdvanceDots();
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
        case 'G': boldMode = true;  escState = ST_NORMAL; return;         // doble golpe ON (se trata como negrita, ver limitaciones)
        case 'H': boldMode = false; escState = ST_NORMAL; return;         // doble golpe OFF
        case 'r': escState = ST_ESC_R; return;                            // color de cinta
        case '-': escState = ST_ESC_MINUS; return;                       // subrayado
        case '*': escState = ST_ESC_STAR_M; return;                      // grafico ESC *
        case 'K': startGraphicsCapture(8, 60);  escState = ST_ESC_KLYZ_NL; return;
        case 'L': startGraphicsCapture(8, 120); escState = ST_ESC_KLYZ_NL; return;
        case 'Y': startGraphicsCapture(8, 120); escState = ST_ESC_KLYZ_NL; return;
        case 'Z': startGraphicsCapture(8, 240); escState = ST_ESC_KLYZ_NL; return;
        case 'k': escState = ST_ESC_K; return;                            // ESC k n: tipo de letra
        case 'x': escState = ST_ESC_X; return;                            // ESC x n: borrador/NLQ
        case 'P': pitchMode = 0; escState = ST_NORMAL; return;            // pica (10 cpi)
        case 'M': pitchMode = 1; escState = ST_NORMAL; return;            // elite (12 cpi)
        case 'g': pitchMode = 2; escState = ST_NORMAL; return;            // 15 cpi
        case 0x0F: condensedMode = true;  escState = ST_NORMAL; return;   // ESC SI: condensado ON
        case 0x12: condensedMode = false; escState = ST_NORMAL; return;   // cancelar condensado
        case 'W': escState = ST_ESC_W; return;                            // ESC W n: ancho doble
        case 'w': escState = ST_ESC_LOWER_W; return;                      // ESC w n: alto doble/cuadruple (extension propia)
        case '4': italicMode = true;  escState = ST_NORMAL; return;       // cursiva ON
        case '5': italicMode = false; escState = ST_NORMAL; return;       // cursiva OFF
        case 'S': escState = ST_ESC_S; return;                            // ESC S n: super/subindice
        case 'T': scriptMode = 0; escState = ST_NORMAL; return;           // cancela super/subindice
        default:
          // Comando ESC/P no soportado: se descarta un unico byte de posible
          // parametro para intentar no desincronizar el resto del flujo.
          // Se deja una linea de depuracion con el caracter recibido (y su
          // valor en hexadecimal, ya que muchos de estos comandos son no
          // imprimibles) para poder ver por el puerto serie de USB que
          // secuencias esta mandando el host que todavia no se interpretan.
          Serial.printf("[DEBUG] Comando ESC no soportado: '%c' (0x%02X)\n",
                        (b >= 0x20 && b <= 0x7E) ? (char)b : '?', b);
          escState = ST_ESC_SKIP1;
          return;
      }

    case ST_ESC_SKIP1:
      escState = ST_NORMAL;
      return;

    case ST_ESC_R:
      currentColor = (decodeNumericParam(b) <= 6) ? decodeNumericParam(b) : 0;
      escState = ST_NORMAL;
      return;

    case ST_ESC_3:
      lineSpacingDots = b; // ESC 3 n -> n/180" == n puntos a nuestra resolucion de 180dpi
      escState = ST_NORMAL;
      return;

    case ST_ESC_MINUS:
      underlineMode = (decodeNumericParam(b) != 0);
      escState = ST_NORMAL;
      return;

    case ST_ESC_K:
      { uint8_t v = decodeNumericParam(b); typefaceMode = (v <= 6) ? v : 0; }
      escState = ST_NORMAL;
      return;

    case ST_ESC_X:
      lqMode = (decodeNumericParam(b) != 0);
      escState = ST_NORMAL;
      return;

    case ST_ESC_W:
      doubleWidthMode = (decodeNumericParam(b) != 0);
      escState = ST_NORMAL;
      return;

    case ST_ESC_LOWER_W: {
      uint8_t v = decodeNumericParam(b);
      heightMultiplier = (v == 1) ? 2 : (v == 2) ? 4 : 1; // 0=normal 1=doble 2=cuadruple, cualquier otro valor -> normal
      escState = ST_NORMAL;
      return;
    }

    case ST_ESC_S:
      scriptMode = (decodeNumericParam(b) == 0) ? 1 : 2; // 0=superindice, 1=subindice (segun ESC/P)
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

void handleByte(uint8_t b) {
  handleByteInner(b);
  // Si el cursor ha llegado (o se ha pasado) del final de la hoja fija sin
  // que haya llegado un Form Feed, cerramos la pagina igualmente (rellenada
  // de blanco hasta PAGE_HEIGHT_DOTS por closePage()) y dejamos que el
  // siguiente byte abra una pagina nueva de forma perezosa, tal y como ya
  // hace el resto del codigo tras un Form Feed o el boton de cierre.
  if (pageOpen && cursorY >= PAGE_HEIGHT_DOTS) {
    Serial.println("[INFO] Fin de hoja alcanzado: cerrando pagina automaticamente.");
    finishPageIfNeeded();
  }
}

// ================================ BOTON DE CIERRE MANUAL =====================

bool buttonLastReading = HIGH;
bool buttonStableState = HIGH;
unsigned long buttonLastChangeMillis = 0;

void checkCloseButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != buttonLastReading) {
    buttonLastChangeMillis = millis();
    buttonLastReading = reading;
  }
  if ((millis() - buttonLastChangeMillis) > BUTTON_DEBOUNCE_MS && reading != buttonStableState) {
    buttonStableState = reading;
    if (buttonStableState == LOW) { // flanco de bajada = boton pulsado (pull-up externo)
      Serial.println("[INFO] Boton pulsado: cerrando pagina manualmente.");
      finishPageIfNeeded();
      buttonFlashActive = true;
      buttonFlashStartMillis = millis();
      updateStatusLed(); // mostrar el amarillo ya, sin esperar a la siguiente vuelta de loop()
    }
  }
}

// ================================ LED RGB DE ESTADO =========================

void updateStatusLed(); // adelantada: sdWriteBegin/End la llaman para reflejar el cambio al instante

// Mutex de la tarjeta SD: ahora que WiFi/OTA/servidor web corren en su
// PROPIA tarea de FreeRTOS en el otro nucleo (ver wifi_web.ino), hay dos
// nucleos que de verdad pueden intentar tocar la SPI de la SD a la vez (el
// bucle principal, que imprime, y el servidor web, que sirve el BMP). La
// libreria SD no es segura frente a eso, asi que cualquier acceso a la SD
// -- escribir o leer -- debe hacerse entre sdAccessBegin() y sdAccessEnd().
SemaphoreHandle_t sdMutex = NULL;

void sdAccessBegin() {
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
}
void sdAccessEnd() {
  if (sdMutex) xSemaphoreGive(sdMutex);
}

// sdWriteBegin/End son para el camino de ESCRITURA del emulador de
// impresora: ademas de reservar el acceso exclusivo a la SD, reflejan en el
// LED que se esta escribiendo. El servidor web, que solo LEE, usa
// directamente sdAccessBegin()/sdAccessEnd() (sin tocar el LED, ver
// wifi_web.ino) para no mentir con el color "escribiendo".
void sdWriteBegin() {
  sdAccessBegin();
  writingToSD = true;
  updateStatusLed(); // cambia el LED YA, sin esperar a la siguiente vuelta de loop()
}
void sdWriteEnd() {
  writingToSD = false;
  updateStatusLed();
  sdAccessEnd();
}

uint32_t ledColor(uint8_t r, uint8_t g, uint8_t b) {
  return rgbLed.Color(r, g, b); // el brillo lo aplica rgbLed.setBrightness() (ver setup()), no aqui
}

uint32_t computeLedColor() {
  if (!sdOk)                                   return ledColor(255, 0,   0);   // rojo: problema con la SD
  if (buttonFlashActive)                       return ledColor(255, 255, 0);   // amarillo: boton pulsado
  if (writingToSD)                             return ledColor(0,   180, 255); // celeste: escribiendo en la SD
  if (everReceivedByte && (millis() - lastByteMillis) < RECEIVING_HOLD_MS)
                                                return ledColor(160, 32, 240);  // morado: recibiendo datos
  if (pageOpen)                                return ledColor(0,   0,   255); // azul: pagina empezada
  return ledColor(0, 255, 0);                                                  // verde: sin pagina empezada
}

void updateStatusLed() {
  if (buttonFlashActive && (millis() - buttonFlashStartMillis >= BUTTON_FLASH_MS)) {
    buttonFlashActive = false;
  }
  uint32_t c = computeLedColor();
  if (c != lastLedColorSet) {
    rgbLed.setPixelColor(0, c);
    rgbLed.show();
    lastLedColorSet = c;
  }
}

// ================================ ARDUINO SETUP / LOOP ======================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[INFO] Emulador de impresora Epson LQ 24 agujas color - iniciando...");

  sdMutex = xSemaphoreCreateMutex(); // antes de cualquier acceso a la SD

  rgbLed.begin();
  rgbLed.setBrightness((uint8_t)(RGB_LED_BRIGHTNESS * 255.0f + 0.5f)); // brillo real de la libreria (antes no se ajustaba)
  rgbLed.setPixelColor(0, ledColor(0, 255, 0)); // verde por defecto (sin pagina) mientras arranca
  rgbLed.show();

  // Cache en PSRAM para el servidor web (ver WEB_BMP_CACHE_MAX_BYTES arriba).
  // psramFound()/ps_malloc() son funciones del propio nucleo ESP32 de
  // Arduino; si la placa no tiene PSRAM (o psramFound() da negativo),
  // bmpCacheBuffer se queda en nullptr y todo sigue funcionando igual, solo
  // que el servidor web leera siempre de la SD (como hasta ahora).
  if (psramFound()) {
    bmpCacheBuffer = (uint8_t *)ps_malloc(WEB_BMP_CACHE_MAX_BYTES);
    if (bmpCacheBuffer) {
      bmpCacheCapacity = WEB_BMP_CACHE_MAX_BYTES;
      Serial.printf("[INFO] Cache PSRAM activa para el servidor web: %lu KB.\n",
                    (unsigned long)(bmpCacheCapacity / 1024));
    } else {
      Serial.println("[WARN] Hay PSRAM pero ps_malloc() fallo; el servidor web leera siempre de la SD.");
    }
  } else {
    Serial.println("[INFO] Esta placa no tiene PSRAM; el servidor web leera siempre de la SD.");
  }

  // El ESP32-S3 no tiene un mapeo VSPI/HSPI fijo: hay que indicar los pines
  // explicitamente antes de montar la SD.
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, SPI, SD_SPI_FREQ_HZ)) {
    Serial.println("[ERROR] No se pudo montar la tarjeta SD. Revisa el cableado/CS.");
    sdOk = false;
  } else {
    Serial.println("[INFO] Tarjeta SD montada correctamente.");
    sdOk = true;
  }
  updateStatusLed(); // reflejar el resultado del montaje de inmediato

  Serial2.setRxBufferSize(2048); // margen extra frente a rafagas mientras se escribe en la SD
  if (USE_HW_FLOW_CONTROL) {
    // OJO: esto NO es lo que fija RX/TX -- eso lo hace siempre Serial2.begin()
    // un poco mas abajo (recibe RX/TX como parametros y los configura tanto
    // si hay control de flujo como si no). setPins() aqui sirve UNICAMENTE
    // para añadir los pines CTS/RTS antes de begin(), que es el unico sitio
    // donde se pueden indicar (el begin() de 5 parametros que usamos no
    // acepta CTS/RTS directamente). Por eso esta en un if: si no se usa
    // control de flujo por hardware, simplemente no se tocan esos dos pines
    // y el puerto serie funciona igual de bien solo con RX/TX.
    Serial2.setPins(SERIAL_RX_PIN, SERIAL_TX_PIN, SERIAL_CTS_PIN, SERIAL_RTS_PIN);
  }
  Serial2.begin(SERIAL_BAUD, SERIAL_8N1, SERIAL_RX_PIN, SERIAL_TX_PIN);
  if (USE_HW_FLOW_CONTROL) {
    // El propio driver de UART baja RTS automaticamente cuando el buffer de
    // recepcion se llena por encima del umbral, y lo sube cuando hay hueco:
    // asi el host deja de enviar mientras escribimos en la SD, sin que
    // nuestro codigo tenga que gestionarlo a mano (a diferencia del XON/XOFF).
    Serial2.setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS_RTS, 64);
  }

  memset(band, COLOR_WHITE_INDEX, sizeof(band));
  resetPrinterState();
  lastByteMillis = millis();

  pinMode(BUTTON_PIN, INPUT); // pull-up externo ya presente en la placa; no usar INPUT_PULLUP
  buttonLastReading = digitalRead(BUTTON_PIN);
  buttonStableState = buttonLastReading;

  Serial.println("[INFO] Esperando datos por el puerto serie (impresora)...");

  // WiFi (con portal de configuracion si hace falta), OTA y servidor web,
  // en su propia tarea en el otro nucleo: la impresora ya esta lista para
  // recibir datos AHORA MISMO, sin esperar a que WiFiManager conecte (o se
  // quede hasta 3 minutos esperando a que alguien configure la red).
  startWifiWebTask();
}

void loop() {
  while (Serial2.available()) {
    uint8_t b = (uint8_t)Serial2.read();
    handleByte(b);
  }

  checkCloseButton();

  // Si la SD fallo (al arrancar o al abrir un fichero), reintentar montarla
  // de vez en cuando en vez de quedarse en rojo para siempre.
  if (!sdOk && (millis() - lastSdRetryMillis > SD_RETRY_INTERVAL_MS)) {
    lastSdRetryMillis = millis();
    Serial.println("[INFO] Reintentando montar la tarjeta SD...");
    sdAccessBegin();
    SD.end();
    bool recovered = SD.begin(SD_CS_PIN, SPI, SD_SPI_FREQ_HZ);
    sdAccessEnd();
    if (recovered) {
      Serial.println("[INFO] Tarjeta SD recuperada.");
      sdOk = true;
      updateStatusLed(); // reflejar la recuperacion de inmediato, sin esperar a la siguiente vuelta
    }
  }

  // Si llevamos un rato sin recibir nada y hay una pagina con contenido sin
  // cerrar (el host no envio Form Feed final), la cerramos igualmente.
  if (pageOpen && pageHasInk && (millis() - lastByteMillis > IDLE_TIMEOUT_MS)) {
    Serial.println("[INFO] Sin actividad: cerrando pagina automaticamente.");
    finishPageIfNeeded();
  }

  updateStatusLed();
}
