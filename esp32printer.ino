//#define IADEVEL
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
 *     (ESC E/F/G/H), subrayado (ESC - n), sobrerrayado (ESC _ n), color
 *     de cinta (ESC r n),
 *     tipo de letra (ESC k n), calidad borrador/NLQ (ESC x n), paso
 *     pica/elite/15cpi (ESC P/M/g), condensado (SI/DC2), cursiva
 *     (ESC 4/5), ancho doble (ESC W n), alto doble/cuadruple (ESC w n,
 *     ver aviso debajo) y super/subindice (ESC S/T). Margenes izquierdo y
 *     derecho (ESC l n / ESC Q n: n columnas del paso (CPI) vigente en el
 *     momento de fijarlos; despues cambiar el CPI NO los mueve) y salto de
 *     linea automatico (CR+LF) al llegar al margen derecho, como una
 *     impresora real, en vez de perder los caracteres sobrantes.
 *     Justificacion (ESC a 0/1/2: izquierda/centro/derecha), tabuladores
 *     (HT, ESC D, ESC e, ESC R) y posicionamiento horizontal (ESC $, ESC \,
 *     ESC f). El texto NO se dibuja al llegar: pasa por un buffer de linea
 *     (LINE_BUFFER_CHARS) que se dibuja al terminar la linea (LF, CR, FF,
 *     salto automatico, grafico, boton...). Eso permite alinear, tener una
 *     linea base comun real aunque se mezclen alturas, y borrar con CAN/DEL.
 *     ESC h n: ancho Y alto doble/cuadruple con un solo comando (n=1 doble,
 *     n=2 cuadruple) y, como extension propia, n=4..7 = imprimir SOLO LA MITAD
 *     superior (4,6) o inferior (5,7) del caracter ampliado: la linea no crece
 *     (mitad de un doble = alto normal) y los caracteres salen el doble de
 *     anchos; sirve para carteles a dos colores (tabla ESC_H_* mas abajo).
 *     ESC SP n: separacion extra a la derecha de cada caracter (n/180" en
 *     NLQ, n/120" en borrador); forma parte del paso, asi que tambien cuenta
 *     para el salto automatico, los margenes y los tabuladores.
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
 *     Una linea con caracteres de alto doble/cuadruple avanza en el LF lo
 *     que haga falta para no pisar a la siguiente (nunca menos que el
 *     interlineado configurado), y los caracteres de distinto alto de una
 *     misma linea comparten la linea base (ver drawChar()).
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
#ifndef IADEVEL
#include "roman.h"
#include "courier.h"
#include "prestige.h"
#include "script.h"
#include "OCRB.h"
#include "OCRA.h"
#endif

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
#define BAND_HEIGHT       128    // Alto del buffer de bandas en filas. DEBE ser mayor que la linea mas alta
                                  // que pueda dibujarse (5x7 a x4 de alto = 84 filas, 8x8 = 96): con una
                                  // banda menor, los caracteres altos pierden su parte superior porque
                                  // ya se han volcado a la SD. 1440*128 = 180KB -> se reserva en PSRAM
                                  // (ver allocBand()), no como variable estatica.
#define BAND_BYTES        ((size_t)BAND_HEIGHT * PAGE_WIDTH_DOTS)
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
#define LINE_BUFFER_CHARS 512       // caracteres que caben en el buffer de linea (>=256; 12 bytes cada uno = 6KB). Si se llena, se dibuja lo acumulado y se sigue.
#define LINE_IDLE_FLUSH_MS 3000UL   // si el host se calla este tiempo con texto sin terminar (sin LF/CR/FF), se dibuja igualmente
#define DEL_CLEARS_WHOLE_LINE false // DEL (0x7F): false = borra solo el ULTIMO caracter del buffer (ESC/P real); true = borra todo el buffer como CAN
#define ESC_R_RESETS_TABS  true     // ESC R sin parametro = restaurar tabuladores de fabrica (modo IBM/Epson). false = ESC R n (juego internacional de caracteres, n se descarta)
#define MAX_HTABS          32       // tabuladores horizontales maximos de ESC D
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
// Un caracter pendiente de dibujar en el buffer de linea, con todos los atributos que tenia al llegar:
struct LineChar {
  int16_t x;        // posicion X logica (puntos) donde se imprimiria sin alinear
  uint8_t ch;
  uint8_t color;
  uint8_t flags;    // bit0 negrita, bit1 subrayado, bit2 sobrerrayado, bit3 cursiva, bit4 NLQ, (bit5 libre), bit6 condensado
  uint8_t typeface;
  uint8_t pitch;
  uint8_t hMul;     // heightMultiplier (ESC w)
  uint8_t script;   // scriptMode
  uint8_t wh;       // bits0-1: log2 del multiplicador de ancho (1/2/4); bits2-3: halfMode (ESC h 4..7)
  uint8_t sp;       // ESC SP n vigente (separacion extra entre caracteres)
};

// Declaraciones adelantadas: el codigo de escritura en SD (mas abajo) necesita
// avisar al LED en el momento exacto en que empieza/termina a escribir, pero
// las funciones del LED estan definidas mas adelante en el fichero.
void sdWriteBegin();
void sdWriteEnd();
void sdAccessBegin();
void sdAccessEnd();
void updateStatusLed();

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

int32_t cursorX = 0;            // posicion horizontal actual, en puntos (0..PAGE_WIDTH_DOTS-1)
int32_t cursorY = 0;            // posicion vertical actual dentro de la pagina, en puntos
int32_t leftMarginDots = 0;                 // ESC l n : margen izquierdo en puntos (ya convertido con el CPI de ese momento)
int32_t rightMarginDots = PAGE_WIDTH_DOTS;  // ESC Q n : margen derecho en puntos, medido desde el borde izquierdo
int32_t lineSpacingDots = DEFAULT_LINE_DOTS;
uint8_t currentColor = 0;       // indice de PALETTE, por defecto negro
bool boldMode = false;
bool underlineMode = false;
bool overscoreMode = false;     // ESC _ n : linea sobre la primera fila del caracter

// --- Estado tipografico (ver seccion RENDERIZADO DE TEXTO para el detalle) ---
uint8_t typefaceMode = 1;   // seleccionado por ESC k n (0 Roman,1 Sans Serif,2 Courier,3 Prestige,4 Script,5 OCR-B,6 OCR-A)
bool    lqMode       = false; // ESC x n : false=borrador (draft), true=NLQ/calidad carta
uint8_t pitchMode    = 0;    // ESC P/M/g : 0=pica(10cpi) 1=elite(12cpi) 2=15cpi
bool    condensedMode = false; // SI (0x0F) / DC2 (0x12)
bool    italicMode    = false; // ESC 4 / ESC 5
uint8_t widthMultiplier = 1;     // ESC W n (1 o 2) y ESC h n (1, 2 o 4): ancho de caracter respecto al paso (CPI) vigente
uint8_t charSpaceN = 0;          // ESC SP n : separacion extra a la derecha de cada caracter (n/180" NLQ, n/120" borrador)
uint8_t halfMode = 0;            // ESC h 4..7 (extension propia): 0 = caracter entero, 1 = solo mitad superior, 2 = solo mitad inferior
uint8_t heightMultiplier = 1;    // ESC w n (extension propia, ver aviso en la respuesta): 1, 2 o 4
uint8_t scriptMode    = 0;   // ESC S n / ESC T : 0=normal 1=superindice 2=subindice

// Estado de la LINEA en curso (se reinicia en cada LF / pagina nueva / ESC @):
uint16_t lineCellHeight = 0;  // alto de la celda mas alta dibujada en esta linea (linea base comun)
int32_t  lineMinAdvance = 0;  // avance minimo en el LF para no pisar la linea siguiente (solo con alto doble/cuadruple; 0 = usar interlineado)

// Buffer de linea (ver flushLineBuffer()): el texto se acumula aqui y se dibuja al cerrar la linea.
LineChar lineBuf[LINE_BUFFER_CHARS];
uint16_t lineBufCount = 0;
uint8_t  lineBufJust = 0;     // justificacion con la que se abrio el buffer (ESC a mid-linea vale desde la linea siguiente)
uint8_t  justification = 0;   // ESC a n : 0=izquierda 1=centro 2=derecha

// Tabuladores. Las posiciones se guardan en puntos RELATIVOS al margen izquierdo.
int32_t  hTabStops[MAX_HTABS];
uint8_t  hTabCount = 0;
uint8_t  hTabMode = 0;        // 0 = de fabrica (cada 8 columnas del paso vigente), 1 = lista ESC D, 2 = incremento fijo ESC e 0 n
int32_t  hTabFixedDots = 0;
int32_t  vTabIncDots = 0;     // ESC e 1 n : VT salta al siguiente multiplo (desde arriba de la hoja); 0 = VT actua como LF
uint8_t  escParam = 0;        // parametro intermedio de comandos ESC de varios bytes
uint16_t escWord = 0;

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
static uint8_t (*band)[PAGE_WIDTH_DOTS] = nullptr; // reservado en setup() por allocBand()
int32_t bandBase = 0;  // primera fila (coordenada Y absoluta) representada en el buffer

// Reserva el buffer de bandas (BAND_BYTES). Preferentemente en PSRAM; si la
// placa no la tiene (o esta desactivada en el menu Herramientas del IDE) se
// intenta en la RAM interna, que con ~180KB puede no caber junto con WiFi.
void allocBand() {
  if (psramFound()) band = (uint8_t (*)[PAGE_WIDTH_DOTS])ps_malloc(BAND_BYTES);
  if (!band)        band = (uint8_t (*)[PAGE_WIDTH_DOTS])malloc(BAND_BYTES);
  if (!band) {
    Serial.println("[ERROR] No hay memoria para el buffer de bandas (activa la PSRAM o baja BAND_HEIGHT).");
    while (true) delay(1000);
  }
}

// ================================ MAQUINA DE ESTADOS ESC/P =================

enum EscState {
  ST_NORMAL,
  ST_ESC,
  ST_ESC_R,        // ESC r n           (color de cinta)
  ST_ESC_3,        // ESC 3 n           (interlineado n/180")
  ST_ESC_MINUS,    // ESC - n           (subrayado on/off)
  ST_ESC_UNDERSCORE, // ESC _ n         (sobrerrayado on/off)
  ST_ESC_L_MARGIN, // ESC l n           (margen izquierdo, n columnas)
  ST_ESC_Q_MARGIN, // ESC Q n           (margen derecho, n columnas)
  ST_ESC_A,        // ESC a n           (justificacion)
  ST_ESC_H,        // ESC h n           (ancho+alto doble/cuadruple; 4..7 = medio caracter)
  ST_ESC_SP,       // ESC SP n          (separacion extra entre caracteres)
  ST_ESC_D,        // ESC D n1..nk NUL  (tabuladores horizontales)
  ST_ESC_E_M,      // ESC e m n         (incremento fijo de tabulador: m)
  ST_ESC_E_N,      //                   (n)
  ST_ESC_F_M,      // ESC f m n         (salto horizontal/vertical: m)
  ST_ESC_F_N,      //                   (n)
  ST_ESC_DOLLAR_L, // ESC $ nL nH       (posicion horizontal absoluta)
  ST_ESC_DOLLAR_H,
  ST_ESC_BSLASH_L, // ESC \ nL nH       (posicion horizontal relativa)
  ST_ESC_BSLASH_H,
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
uint16_t gfxDpi = 180;     // resolucion horizontal del grafico en curso (dpi)
int32_t  gfxX0 = 0;        // X en la que empezo el bloque (cursorX al recibir el comando)
uint8_t  gfxDotH = 1;      // alto en puntos de 180dpi de un punto del grafico: 1 (24 agujas) o 3 (8 agujas a 1/60")
uint16_t gfxNumCols;       // numero total de columnas a recibir
uint16_t gfxColIndex;      // columna actual recibida
uint8_t  gfxColBuf[3];     // bytes acumulados de la columna en curso
uint8_t  gfxColBufPos;

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
//
// MODELO DE PAGINA (acceso aleatorio en vertical)
//  - El fichero BMP de la pagina se crea ENTERO desde el primer momento, con el
//    tamano final de la hoja (PAGE_HEIGHT_DOTS filas) y todo blanco: es un BMP
//    valido en todo instante (cabecera definitiva), aunque se abra a medio imprimir.
//  - band[] es una VENTANA de BAND_HEIGHT filas sobre ese bitmap (buffer en anillo,
//    fila y -> hueco y % BAND_HEIGHT). La ventana puede deslizarse hacia abajo Y hacia
//    arriba (moveWindowTo): las filas que salen se escriben en el fichero si han sido
//    modificadas (bandDirty) y las que entran se LEEN de lo ya escrito (no se crean
//    en blanco), asi se puede volver a pintar en trozos verticales ya impresos sin
//    perder nada. Una fila que nunca se ha escrito se sabe en blanco sin leerla
//    (rowWrittenBits). Si hay cache PSRAM (copia completa de la hoja) las lecturas
//    salen de ella y no de la SD.
//  - El fichero se crea al PRIMER punto con tinta (materializePage), no al abrir la
//    pagina: una hoja que acaba en blanco no cuesta nada ni deja fichero.

int32_t maxYUsed = -1; // fila mas baja en la que se ha pintado algo desde que se abrio la pagina
char currentFileName[32];
bool pageFileCreated = false;                     // el fichero BMP de la pagina actual ya existe en la SD
int32_t lastRowWritten = -2;                      // ultima fila escrita (para escribir en secuencia sin hacer seek)
uint8_t bandDirty[BAND_HEIGHT];                   // por hueco del anillo: fila modificada y aun no escrita al fichero
uint8_t rowWrittenBits[(PAGE_HEIGHT_DOTS + 7) / 8]; // por fila: ya se ha escrito alguna vez al fichero (si no, es blanca)
static uint8_t whiteChunk[4096];                  // relleno blanco para crear el fichero (indice 7 en los dos nibbles)

uint32_t rowSizeBytes() {
  uint32_t raw = ((uint32_t)PAGE_WIDTH_DOTS + 1) / 2; // 4 bits/pixel = 2 pixeles por byte
  return (raw + 3) & ~((uint32_t)3); // redondeo a multiplo de 4
}

// Tamano del fichero BMP de una pagina (siempre el mismo: cabecera + hoja completa).
uint32_t pageFileTotalBytes() {
  return BMP_HEADER_BYTES + rowSizeBytes() * (uint32_t)PAGE_HEIGHT_DOTS;
}

// La cache PSRAM contiene la hoja completa (cabecera + todas las filas) y esta al dia.
bool cacheCoversPage() {
  return bmpCacheBuffer && bmpCacheBytes == pageFileTotalBytes();
}

// Escribe al fichero (y a la cache) la fila y si esta modificada. Debe llamarse con la SD bloqueada.
void writeBackRow(int32_t y) {
  if (y < 0 || y >= PAGE_HEIGHT_DOTS) return;
  int32_t rel = y % BAND_HEIGHT;
  if (!bandDirty[rel]) return;
  uint32_t rs = rowSizeBytes();
  uint8_t rowBuf[PAGE_WIDTH_DOTS / 2 + 4]; // +4: margen para el relleno a multiplo de 4
  memset(rowBuf, 0, sizeof(rowBuf));
  for (int x = 0; x < PAGE_WIDTH_DOTS; x++) {
    uint8_t idx = band[rel][x];
    if (idx == COLOR_WHITE_INDEX) idx = 7; // blanco = entrada 7 de la paleta
    // 2 pixeles por byte: el primero (x par) va en el nibble alto, el
    // segundo (x impar) en el nibble bajo -- orden estandar de BMP 4bpp.
    if (x & 1) rowBuf[x / 2] |= (idx & 0x0F);
    else       rowBuf[x / 2] |= (idx & 0x0F) << 4;
  }
  uint32_t off = BMP_HEADER_BYTES + (uint32_t)y * rs;
  if (lastRowWritten != y - 1) pageFile.seek(off); // escritura secuencial: el puntero ya esta en su sitio
  pageFile.write(rowBuf, rs);                      // una unica escritura por fila
  lastRowWritten = y;
  if (cacheCoversPage()) memcpy(bmpCacheBuffer + off, rowBuf, rs);
  rowWrittenBits[y >> 3] |= (uint8_t)(1 << (y & 7));
  bandDirty[rel] = 0;
}

// Carga en la ventana la fila y: lo que haya ya escrito en el fichero (o la cache),
// o blanco si esa fila nunca se ha escrito. Debe llamarse con la SD bloqueada.
void loadRow(int32_t y) {
  int32_t rel = y % BAND_HEIGHT;
  bandDirty[rel] = 0;
  if (y < 0 || y >= PAGE_HEIGHT_DOTS || !(rowWrittenBits[y >> 3] & (1 << (y & 7)))) {
    memset(band[rel], COLOR_WHITE_INDEX, PAGE_WIDTH_DOTS);
    return;
  }
  uint32_t rs = rowSizeBytes();
  uint32_t off = BMP_HEADER_BYTES + (uint32_t)y * rs;
  uint8_t rowBuf[PAGE_WIDTH_DOTS / 2 + 4];
  const uint8_t *src;
  if (cacheCoversPage()) {
    src = bmpCacheBuffer + off;          // sin tocar la SD
  } else {
    pageFile.seek(off);
    pageFile.read(rowBuf, rs);
    lastRowWritten = -2;                 // el puntero ya no esta tras la ultima fila escrita
    src = rowBuf;
  }
  for (int x = 0; x < PAGE_WIDTH_DOTS; x++) {
    uint8_t nib = (x & 1) ? (src[x / 2] & 0x0F) : (src[x / 2] >> 4);
    band[rel][x] = (nib == 7) ? COLOR_WHITE_INDEX : nib;
  }
}

// Desliza la ventana para que empiece en newBase: escribe las filas que salen (si estan
// modificadas) y carga las que entran. Las que se quedan no se tocan. Con la SD bloqueada.
void moveWindowTo(int32_t newBase) {
  if (newBase < 0) newBase = 0;
  if (newBase == bandBase) return;
  int32_t oldBase = bandBase, oldEnd = bandBase + BAND_HEIGHT, newEnd = newBase + BAND_HEIGHT;
  for (int32_t y = oldBase; y < oldEnd; y++)          // 1) las que salen, antes de reutilizar su hueco
    if (y < newBase || y >= newEnd) writeBackRow(y);
  for (int32_t y = newBase; y < newEnd; y++)          // 2) las que entran
    if (y < oldBase || y >= oldEnd) loadRow(y);
  bandBase = newBase;
}

void ensureBandCovers(int32_t y) {
  if (y >= bandBase && y < bandBase + BAND_HEIGHT) return; // caso normal: ya esta en la ventana
  sdWriteBegin();
  // Hacia abajo: se desliza lo minimo (y queda en la ultima fila). Hacia arriba: y pasa a ser
  // la PRIMERA fila de la ventana, que es lo que conviene al dibujar de arriba abajo.
  moveWindowTo((y >= bandBase + BAND_HEIGHT) ? (y - BAND_HEIGHT + 1) : y);
  sdWriteEnd();
}

// Crea el fichero de la pagina actual con su tamano definitivo (cabecera final + hoja blanca).
// Se llama al primer punto con tinta. Devuelve false si no se pudo (SD fallida o sin nombre libre).
bool materializePage() {
  sdAccessBegin(); // bloquea la SD para toda la funcion (busqueda de nombre + creacion + relleno)

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
    currentFileName[0] = 0;
    pageOpen = false;
    sdAccessEnd();
    return false;
  }
  pageFile = SD.open(currentFileName, "w+"); // lectura Y escritura: se vuelven a leer filas ya escritas
  if (!pageFile) {
    Serial.printf("[ERROR] No se pudo crear %s en la SD\n", currentFileName);
    pageIndex--;
    currentFileName[0] = 0;
    pageOpen = false;
    sdOk = false;
    sdAccessEnd();
    updateStatusLed(); // reflejar el fallo de inmediato, sin esperar al loop()
    return false;
  }

  writingToSD = true; // el LED solo refleja "escribiendo" a partir de aqui, no durante la busqueda de nombre
  updateStatusLed();

  // --- Cabecera BMP indexada de 4 bits (BMP_HEADER_BYTES = 118 bytes: 54 de cabecera
  // + 64 de tabla de color), DEFINITIVA desde el principio: alto de toda la hoja.
  uint32_t total = pageFileTotalBytes();
  uint32_t hNeg = (uint32_t)(-(int32_t)PAGE_HEIGHT_DOTS); // alto negativo = filas de arriba a abajo
  uint8_t header[BMP_HEADER_BYTES];
  memset(header, 0, sizeof(header));
  header[0] = 'B'; header[1] = 'M';
  header[2] = (uint8_t)(total & 0xFF); header[3] = (uint8_t)((total >> 8) & 0xFF);
  header[4] = (uint8_t)((total >> 16) & 0xFF); header[5] = (uint8_t)((total >> 24) & 0xFF);
  header[10] = (uint8_t)(BMP_HEADER_BYTES & 0xFF); // offset a los datos de pixel
  header[11] = (uint8_t)((BMP_HEADER_BYTES >> 8) & 0xFF);
  header[14] = 40; // tamano de BITMAPINFOHEADER
  header[18] = (uint8_t)(PAGE_WIDTH_DOTS & 0xFF);
  header[19] = (uint8_t)((PAGE_WIDTH_DOTS >> 8) & 0xFF);
  header[20] = (uint8_t)((PAGE_WIDTH_DOTS >> 16) & 0xFF);
  header[21] = (uint8_t)((PAGE_WIDTH_DOTS >> 24) & 0xFF);
  header[22] = (uint8_t)(hNeg & 0xFF); header[23] = (uint8_t)((hNeg >> 8) & 0xFF);
  header[24] = (uint8_t)((hNeg >> 16) & 0xFF); header[25] = (uint8_t)((hNeg >> 24) & 0xFF);
  header[26] = 1; // planos
  header[28] = 4; // bits por pixel: 4 (16 colores de paleta, indexado)
  // bytes 30..33 (compresion BI_RGB) y 34..37 (tamano de imagen) ya son 0
  header[38] = 0x12; header[39] = 0x0B; // ~2834 pixeles/metro (180dpi), X
  header[42] = 0x12; header[43] = 0x0B; // idem, Y
  // bytes 46..49 (colores en la paleta) y 50..53 (colores importantes) a 0 = "el maximo
  // del bit depth" (16), convencion estandar.
  // Tabla de color (16 entradas x 4 bytes BGR0): las primeras 8 son la paleta real de la cinta.
  for (int i = 0; i < 8; i++) {
    uint8_t *entry = &header[54 + i * 4];
    entry[0] = PALETTE[i].b;
    entry[1] = PALETTE[i].g;
    entry[2] = PALETTE[i].r;
    entry[3] = 0;
  }
  pageFile.write(header, sizeof(header));

  // --- Hoja entera en blanco (indice 7 en los dos nibbles = 0x77). Escribir de verdad
  // (y no solo hacer seek) es imprescindible: en FAT, extender un fichero con seek
  // deja datos basura sin inicializar.
  memset(whiteChunk, 0x77, sizeof(whiteChunk));
  uint32_t remaining = total - BMP_HEADER_BYTES;
  while (remaining > 0) {
    uint32_t n = (remaining < sizeof(whiteChunk)) ? remaining : (uint32_t)sizeof(whiteChunk);
    pageFile.write(whiteChunk, n);
    remaining -= n;
  }
  pageFile.flush();
  lastRowWritten = -2; // el puntero esta al final del fichero

  // --- Cache PSRAM: copia completa de la hoja (cabecera + blanco). bmpCacheBytes solo
  // se fija al final: el servidor web nunca ve una cache a medio inicializar.
  bmpCacheBytes = 0;
  if (bmpCacheBuffer && bmpCacheCapacity >= total) {
    memcpy(bmpCacheBuffer, header, sizeof(header));
    memset(bmpCacheBuffer + BMP_HEADER_BYTES, 0x77, total - BMP_HEADER_BYTES);
    bmpCacheBytes = total;
  }

  pageFileCreated = true;
  writingToSD = false;
  updateStatusLed();
  sdAccessEnd();

  Serial.printf("[INFO] Nueva pagina: %s\n", currentFileName);
  return true;
}

void plotDot(int32_t x, int32_t y, uint8_t colorIndex) {
  if (x < 0 || x >= PAGE_WIDTH_DOTS || y < 0 || y >= PAGE_HEIGHT_DOTS) return;
  if (!pageFileCreated && !materializePage()) return; // primer punto con tinta: se crea el fichero
  ensureBandCovers(y);
  int32_t rel = y % BAND_HEIGHT; // buffer en anillo: mismo indexado que usan writeBackRow()/loadRow()
  band[rel][x] = colorIndex;
  bandDirty[rel] = 1;
  pageHasInk = true;
  if (y > maxYUsed) maxYUsed = y;
}

// Abre una pagina nueva en el sentido LOGICO (posicion, buffer de bandas en blanco...). No toca la
// SD: el fichero se crea al primer punto con tinta (materializePage()).
void openNewPage() {
  bandBase = 0;
  cursorX = leftMarginDots;
  cursorY = 0;
  pageHasInk = false;
  maxYUsed = -1;
  memset(band, COLOR_WHITE_INDEX, BAND_BYTES);
  memset(bandDirty, 0, sizeof(bandDirty));
  memset(rowWrittenBits, 0, sizeof(rowWrittenBits));
  lastRowWritten = -2;
  lineCellHeight = 0; lineMinAdvance = 0; // linea nueva arriba de la hoja
  pageFileCreated = false;
  currentFileName[0] = 0;
  bmpCacheBytes = 0; // la cache de la pagina anterior ya no vale
  pageOpen = true;
}

void closePage() {
  if (!pageOpen) return;

  if (USE_XONXOFF) Serial2.write((uint8_t)0x13); // XOFF: puede tardar en escribir en SD

  if (pageFileCreated) {
    sdWriteBegin();
    // El fichero ya tiene el tamano definitivo: solo hay que escribir las filas de la
    // ventana que sigan sin volcar.
    for (int32_t y = bandBase; y < bandBase + BAND_HEIGHT; y++) writeBackRow(y);
    pageFile.flush();
    pageFile.close();
    sdWriteEnd();
    Serial.printf("[INFO] Pagina cerrada: %s (%lu filas)\n", currentFileName, (unsigned long)PAGE_HEIGHT_DOTS);
  }
  pageOpen = false;
  pageFileCreated = false;

  if (USE_XONXOFF) Serial2.write((uint8_t)0x11); // XON
}

void flushLineBuffer(); // declaracion adelantada (definida tras drawChar)

void finishPageIfNeeded() {
  flushLineBuffer(); // el texto pendiente de la linea se dibuja ANTES de cerrar la hoja
  if (pageOpen && pageHasInk) {
    closePage();
  } else if (pageOpen) {
    // Pagina vacia: se descarta (si no llego a crearse fichero, no hay nada que borrar)
    if (pageFileCreated) {
      sdWriteBegin();
      pageFile.close();
      SD.remove(currentFileName);
      pageIndex--; // el numero queda libre para la siguiente pagina
      sdWriteEnd();
    }
    pageOpen = false;
    pageFileCreated = false;
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

// Ancho base en puntos de UNA columna al paso (CPI) vigente: sin ancho doble (ESC W / ESC h)
// y sin la separacion extra de ESC SP.
uint16_t computeBaseColumnDots() {
  float cpi;
  if (condensedMode) cpi = (pitchMode == 1) ? 20.0f : 17.14f; // condensada elite / pica
  else if (pitchMode == 0) cpi = 10.0f;   // pica
  else if (pitchMode == 1) cpi = 12.0f;   // elite
  else cpi = 15.0f;                       // ESC g
  uint16_t advance = (uint16_t)((180.0f / cpi) + 0.5f);
  if (advance < 4) advance = 4;
  return advance;
}

// ESC SP n: separacion extra a la derecha de cada caracter, en puntos de 180dpi:
// n/180" en NLQ (1 punto por unidad) y n/120" en borrador (1,5 puntos por unidad).
uint16_t computeSpacingDots() {
  return lqMode ? (uint16_t)charSpaceN : (uint16_t)(((uint16_t)charSpaceN * 3) / 2);
}

// Ancho de una columna al paso vigente SIN el ancho doble pero CON la separacion de
// ESC SP (el paso incluye ESC SP): es la unidad en la que se expresan los margenes
// ESC l / ESC Q y los tabuladores ESC D / ESC e.
uint16_t computeColumnDots() {
  return computeBaseColumnDots() + computeSpacingDots();
}

// Celda del GLIFO: la columna base por el multiplicador de ancho (ESC W / ESC h).
// Es el ancho con el que se escala el dibujo del caracter (sin la separacion extra).
uint16_t computeCellDots() {
  return computeBaseColumnDots() * widthMultiplier;
}

// Avance real de un caracter (su "paso"): celda del glifo + separacion de ESC SP.
// Es lo que avanza el cursor y lo que se usa para el salto automatico y BS.
uint16_t computeAdvanceDots() {
  return computeCellDots() + computeSpacingDots();
}

#ifdef IADEVEL
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
#else
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
#endif

// Factor de alto (ESC w) realmente aplicable: la linea mas alta debe caber en el
// buffer de bandas (BAND_HEIGHT); si no cabe se reduce en vez de perder la parte superior.
uint8_t effectiveHeightMul(FontChoice fc) {
  uint8_t m = heightMultiplier;
  while (m > 1 && (int)fc.rows * fc.baseScaleY * m > BAND_HEIGHT - 4) m--;
  return m;
}

// ESC h n : tamano de ancho+alto de una vez. n=0,1,2 como la Epson (normal, doble, cuadruple);
// n=4..7 es EXTENSION PROPIA: solo se imprime la mitad superior (halfMode 1) o inferior (2) del
// caracter ya ampliado (doble: 4 sup / 5 inf; cuadruple: 6 sup / 7 inf). n=3 y >7 no estan
// definidos (se ignoran con aviso DEBUG). Para remapear los codigos basta con tocar esta tabla.
const uint8_t ESC_H_WMUL[8] = {1, 2, 4, 0, 2, 2, 4, 4}; // multiplicador de ancho (0 = n indefinido)
const uint8_t ESC_H_HMUL[8] = {1, 2, 4, 0, 2, 2, 4, 4}; // multiplicador de alto
const uint8_t ESC_H_HALF[8] = {0, 0, 0, 0, 1, 2, 1, 2}; // 0 entero, 1 mitad superior, 2 mitad inferior

// Alto que ocupa el caracter actual en la linea (su "celda"): el del caracter ampliado entero o,
// con medio caracter (ESC h 4..7), la mitad superior o inferior de ese alto.
uint16_t charCellHeight(FontChoice fc) {
  uint8_t m = effectiveHeightMul(fc);
  uint16_t full = (uint16_t)fc.rows * fc.baseScaleY * m;
  if (halfMode != 0 && m > 1) return (halfMode == 1) ? full / 2 : full - full / 2;
  return full;
}

// Linea horizontal de subrayado / sobrerrayado a lo ancho de la celda del
// caracter actual (cursorX .. cursorX+advance-1), de 'thick' filas desde yTop.
// NLQ: continua. Borrador: a "golpes de aguja" del MISMO tamano que los puntos
// de los caracteres (dotW = ancho de un punto de la fuente: ~3 px a 10 cpi,
// 2 a 12 cpi...), un golpe si y un hueco igual, usando la X ABSOLUTA de la
// pagina para que el patron siga sin cortes de un caracter al siguiente.
void drawScoreLine(int32_t yTop, uint16_t advance, uint8_t dotW, uint8_t thick) {
  if (dotW < 1) dotW = 1;
  for (int x = 0; x < advance; x++) {
    int32_t absX = cursorX + x;
    if (!lqMode && ((absX / dotW) & 1)) continue;
    for (uint8_t t = 0; t < thick; t++) plotDot(absX, yTop + t, currentColor);
  }
}

void drawChar(uint8_t c) {
  FontChoice fc = chooseFont();
  if (c < fc.firstChar || c > fc.lastChar) c = ' ';
  const uint8_t *glyph = fc.data + (uint32_t)(c - fc.firstChar) * fc.cols;

  uint16_t advance = computeCellDots();  // celda del glifo (sin la separacion de ESC SP): fija la escala
  uint16_t pitch = advance + computeSpacingDots(); // paso completo: el subrayado/sobrerrayado cubre tambien la separacion
  uint16_t usable = (advance > 2) ? (advance - 2) : advance;
  uint8_t scaleX = usable / fc.cols;
  if (scaleX < 1) scaleX = 1;

  // ESC w: doble/cuadruple alto (extension propia). La linea mas alta debe
  // caber en el buffer de bandas (BAND_HEIGHT): si no cabe, se reduce el factor
  // en vez de perder la parte superior del caracter.
  uint8_t hMul = effectiveHeightMul(fc);

  uint8_t scaleY = fc.baseScaleY * hMul;
  uint8_t normalScaleY = scaleY; // sin la reduccion de super/subindice (para el grosor de subrayado/sobrerrayado)
  if (scriptMode != 0) { scaleY = (uint8_t)((scaleY * 2) / 3); if (scaleY < 1) scaleY = 1; }

  uint16_t normalHeight = fc.rows * fc.baseScaleY;       // alto sin ESC w
  uint16_t fullHeight = fc.rows * fc.baseScaleY * hMul;  // alto del caracter ENTERO (sin super/subindice, con doble/cuadruple ya aplicado)
  uint16_t thisHeight = fc.rows * scaleY;

  // Medio caracter (ESC h 4..7): se dibuja solo la mitad superior o inferior del
  // caracter ya ampliado. winStart/winEnd = ventana visible en filas del caracter
  // entero; layoutHeight = lo que ocupa en la linea (ver charCellHeight()).
  bool halfActive = (halfMode != 0 && hMul > 1);
  uint16_t winStart = 0, winEnd = fullHeight;
  if (halfActive) {
    if (halfMode == 1) winEnd = fullHeight / 2;
    else               winStart = fullHeight / 2;
  }
  uint16_t layoutHeight = winEnd - winStart;
  uint16_t scriptOffset = (scriptMode == 2) ? (fullHeight - thisHeight) : 0; // subindice: alineado abajo dentro del caracter entero

  // Linea base comun: la celda de la linea tiene el alto de la celda mas alta de
  // la linea (lineCellHeight; flushLineBuffer() ya la ha medido entera), y cada
  // caracter se alinea por ABAJO en esa celda. Con texto de un solo tamano no
  // cambia nada (celda = alto del caracter).
  if (layoutHeight > lineCellHeight) lineCellHeight = layoutHeight;
  if (layoutHeight > normalHeight) {
    // La linea necesita al menos su alto + el mismo margen que hay entre una
    // linea normal y el interlineado; asi el LF no hace pisarse las lineas.
    // (La mitad de un doble mide lo mismo que un normal: no crece.)
    int32_t gap = (lineSpacingDots > normalHeight) ? (lineSpacingDots - normalHeight) : 0;
    if (layoutHeight + gap > lineMinAdvance) lineMinAdvance = layoutHeight + gap;
  }
  int32_t cellTop = cursorY + (lineCellHeight - layoutHeight);
  int32_t yBase = cellTop - winStart + scriptOffset; // y de la fila 0 del glifo (la ventana empieza en cellTop)

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
        if (halfActive) { // solo la parte del caracter entero que cae dentro de la ventana
          int32_t yRel = scriptOffset + row * scaleY + sy;
          if (yRel < winStart || yRel >= winEnd) continue;
        }
        for (int sx = 0; sx < scaleX; sx++) {
          int32_t x = cursorX + col * scaleX + sx + xShear;
          int32_t y = yBase + row * scaleY + sy;
          plotDot(x, y, currentColor);
          if (boldMode) plotDot(x + 1, y, currentColor); // negrita: doble golpe desplazado
        }
      }
    }
  }
  if (underlineMode || overscoreMode) {
    // Grosor: en NLQ 1 fila (linea continua); en borrador la misma altura que
    // un golpe de aguja de los caracteres (paintRows de un caracter normal:
    // 1 fila a 10 cpi sin ESC w, mas con alto doble/cuadruple).
    uint8_t lineThick = lqMode ? 1 : (uint8_t)max(1, normalScaleY / 2);
    // Con medio caracter, el subrayado es del caracter ENTERO y solo cae en la mitad inferior; el sobrerrayado en la superior.
    if (underlineMode && !(halfActive && halfMode == 1))  // subrayado: ultima fila(s) de la celda (linea base normal)
      drawScoreLine(cellTop + layoutHeight - lineThick, pitch, scaleX, lineThick);
    if (overscoreMode && !(halfActive && halfMode == 2))  // sobrerrayado: primera fila(s) de la celda del caracter
      drawScoreLine(cellTop, pitch, scaleX, lineThick);
  }
}

// ================================ BUFFER DE LINEA ===========================
//
// Los caracteres imprimibles no se dibujan al llegar: se guardan (con sus
// atributos) en lineBuf[] y se dibujan todos juntos al cerrar la linea. Asi se
// conoce la linea completa antes de pintar y se puede (1) alinear a derecha o
// centro (ESC a), (2) fijar la linea base comun con el caracter MAS ALTO de la
// linea, lo llegue antes o despues, (3) borrar lo pendiente con CAN/DEL.
// Los graficos, tabuladores y saltos de posicion NO pasan por el buffer: solo
// mueven cursorX (posicion logica) o fuerzan antes el volcado.

void lineBufPush(uint8_t ch) {
  if (lineBufCount >= LINE_BUFFER_CHARS) flushLineBuffer(); // lleno: se dibuja lo acumulado y se sigue
  if (lineBufCount == 0) lineBufJust = justification;
  LineChar &lc = lineBuf[lineBufCount++];
  lc.x = (int16_t)cursorX;
  lc.ch = ch;
  lc.color = currentColor;
  lc.flags = (boldMode ? 1 : 0) | (underlineMode ? 2 : 0) | (overscoreMode ? 4 : 0) | (italicMode ? 8 : 0) |
             (lqMode ? 16 : 0) | (condensedMode ? 64 : 0);
  lc.typeface = typefaceMode;
  lc.pitch = pitchMode;
  lc.hMul = heightMultiplier;
  lc.script = scriptMode;
  lc.wh = (widthMultiplier == 4 ? 2 : widthMultiplier == 2 ? 1 : 0) | (halfMode << 2);
  lc.sp = charSpaceN;
}

// Carga en las variables de estado "vivas" los atributos del caracter i del
// buffer, para reutilizar chooseFont()/computeAdvanceDots()/drawChar() tal cual.
void applyLineChar(uint16_t i) {
  const LineChar &lc = lineBuf[i];
  currentColor = lc.color;
  boldMode = lc.flags & 1;       underlineMode = lc.flags & 2;  overscoreMode = lc.flags & 4;
  italicMode = lc.flags & 8;     lqMode = lc.flags & 16;
  condensedMode = lc.flags & 64;
  widthMultiplier = 1 << (lc.wh & 3);  halfMode = lc.wh >> 2;
  charSpaceN = lc.sp;
  typefaceMode = lc.typeface;    pitchMode = lc.pitch;
  heightMultiplier = lc.hMul;    scriptMode = lc.script;
}

void flushLineBuffer() {
  if (lineBufCount == 0) return;
  uint16_t n = lineBufCount;
  lineBufCount = 0; // se vacia ya: lo que sigue solo lee lineBuf[0..n-1]

  // Estado vivo que hay que devolver intacto al terminar
  int32_t sX = cursorX; uint8_t sColor = currentColor;
  bool sBold = boldMode, sUl = underlineMode, sOs = overscoreMode, sIt = italicMode, sLq = lqMode, sCond = condensedMode;
  uint8_t sFace = typefaceMode, sPitch = pitchMode, sHMul = heightMultiplier, sScript = scriptMode, sWMul = widthMultiplier, sHalf = halfMode, sSp = charSpaceN;

  // 1) Medir: celda mas alta de la linea y extension horizontal. Los espacios
  //    del final no cuentan para centrar/alinear (no son parte visible del texto).
  int16_t lastInk = -1;
  for (uint16_t i = 0; i < n; i++) if (lineBuf[i].ch != ' ') lastInk = i;
  uint16_t cell = lineCellHeight; // CR + sobreimpresion: la linea fisica ya impresa tambien cuenta
  int32_t minX = 0x7FFFFFFF, maxX = -1;
  for (uint16_t i = 0; i < n; i++) {
    applyLineChar(i);
    FontChoice fc = chooseFont();
    uint16_t h = charCellHeight(fc); // alto de celda de ESTE caracter (mitad si es medio caracter)
    if (h > cell) cell = h;
    if ((int16_t)i <= lastInk) {
      int32_t l = lineBuf[i].x, r = l + computeCellDots(); // sin la separacion final: no es parte visible del texto
      if (l < minX) minX = l;
      if (r > maxX) maxX = r;
    }
  }

  // 2) Desplazamiento horizontal segun la justificacion con la que se abrio la linea
  int32_t offset = 0;
  if (maxX >= 0 && (lineBufJust == 1 || lineBufJust == 2)) {
    int32_t width = maxX - minX;
    if (lineBufJust == 1) offset = (leftMarginDots + (rightMarginDots - leftMarginDots - width) / 2) - minX; // centrado entre margenes
    else                  offset = rightMarginDots - maxX;                                                   // pegado al margen derecho
    if (minX + offset < leftMarginDots) offset = leftMarginDots - minX; // no cabe: se queda en el margen izquierdo
  }

  // 3) Dibujar con la celda comun (linea base compartida, alineados por abajo)
  lineCellHeight = cell;
  for (uint16_t i = 0; i < n; i++) {
    applyLineChar(i);
    cursorX = lineBuf[i].x + offset;
    drawChar(lineBuf[i].ch);
  }

  cursorX = sX; currentColor = sColor;
  boldMode = sBold; underlineMode = sUl; overscoreMode = sOs; italicMode = sIt; lqMode = sLq; condensedMode = sCond;
  typefaceMode = sFace; pitchMode = sPitch; heightMultiplier = sHMul; scriptMode = sScript; widthMultiplier = sWMul; halfMode = sHalf; charSpaceN = sSp;
}

// ================================ MAQUINA DE ESTADOS: PROCESADO =============

void resetPrinterState() {
  currentColor = 0;
  boldMode = false;
  underlineMode = false;
  overscoreMode = false;
  lineCellHeight = 0;
  lineMinAdvance = 0;
  leftMarginDots = 0;
  rightMarginDots = PAGE_WIDTH_DOTS;
  lineSpacingDots = DEFAULT_LINE_DOTS;
  cursorX = 0;
  cursorY = 0;
  typefaceMode = 1;
  lqMode = false;
  pitchMode = 0;
  condensedMode = false;
  italicMode = false;
  widthMultiplier = 1;
  halfMode = 0;
  charSpaceN = 0;
  heightMultiplier = 1;
  scriptMode = 0;
  lineBufCount = 0;
  justification = 0;
  hTabMode = 0; hTabCount = 0; hTabFixedDots = 0; vTabIncDots = 0;
}

void startGraphicsCapture(uint8_t pins, uint16_t dpi) {
  gfxPins = pins;
  gfxBytesPerCol = (pins == 24) ? 3 : 1;
  gfxDpi = (dpi == 0) ? 180 : dpi;
  gfxX0 = cursorX;
  gfxDotH = (pins == 24) ? 1 : 3; // 24 agujas: 1 punto por aguja (1/180"); 8 agujas: 1/60" = 3 puntos
}

// Dibuja la columna de origen gfxColIndex (ya recibida en gfxColBuf). Cada punto
// del grafico ocupa su celda REAL en nuestra rejilla de 180dpi, no un unico pixel:
//  - ancho: de c*180/dpi a (c+1)*180/dpi (por posicion, no por paso entero, asi
//    80/72/120/144dpi no acumulan error y un bloque 0xFF sale sin huecos). Si dpi>180
//    (240, 360) varias columnas de origen caen en el mismo punto: se fusionan (OR).
//  - alto: gfxDotH filas por aguja (3 en 8 agujas, 1 en 24).
void plotGraphicsColumn() {
  int32_t c = gfxColIndex;
  int32_t xa = gfxX0 + (c * 180) / gfxDpi;
  int32_t xb = gfxX0 + ((c + 1) * 180) / gfxDpi;
  if (xb <= xa) xb = xa + 1;
  for (int b = 0; b < gfxBytesPerCol; b++) {
    uint8_t byteVal = gfxColBuf[b];            // MSB = aguja superior de cada byte
    for (int bit = 0; bit < 8; bit++) {
      if (!(byteVal & (0x80 >> bit))) continue;
      int pinIndex = b * 8 + bit;              // 0..7 (8 agujas) o 0..23 (24 agujas)
      int32_t y0 = cursorY + pinIndex * gfxDotH;
      for (int32_t x = xa; x < xb; x++)
        for (uint8_t dy = 0; dy < gfxDotH; dy++)
          plotDot(x, y0 + dy, currentColor);
    }
  }
  gfxColIndex++;
  gfxColBufPos = 0;
}

// Fin del bloque: el cursor queda justo detras del ultimo punto, segun el dpi.
void finishGraphicsBlock() {
  cursorX = gfxX0 + ((int32_t)gfxNumCols * 180) / gfxDpi;
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

// Salto de linea: avanza el interlineado configurado, o mas si la linea que
// se cierra contenia caracteres de alto doble/cuadruple (que no cabrian y se
// pisarian con la linea siguiente). Con texto normal lineMinAdvance es 0 y
// el comportamiento es el de siempre.
void doLineFeed() {
  int32_t adv = lineSpacingDots;
  if (lineMinAdvance > adv) adv = lineMinAdvance;
  cursorY += adv;
  lineCellHeight = 0;
  lineMinAdvance = 0;
}

// LF: vuelca el texto pendiente de la linea, avanza y (AUTO_CR_ON_LF) vuelve al margen izquierdo.
void lineFeedAction() {
  flushLineBuffer();
  doLineFeed();
  if (AUTO_CR_ON_LF) cursorX = leftMarginDots;
}

// HT: siguiente tabulador a la derecha de cursorX. Si no hay ninguno antes del
// margen derecho se ignora (como las impresoras reales). Posiciones relativas al margen izquierdo.
void doHorizontalTab() {
  int32_t rel = cursorX - leftMarginDots;
  int32_t next = -1;
  if (hTabMode == 0) {                       // de fabrica: cada 8 columnas del paso vigente
    int32_t cell = computeAdvanceDots() * 8;
    next = ((rel / cell) + 1) * cell;
  } else if (hTabMode == 2) {                // ESC e 0 n: incremento fijo
    next = ((rel / hTabFixedDots) + 1) * hTabFixedDots;
  } else {                                   // ESC D: lista explicita
    for (uint8_t i = 0; i < hTabCount; i++) if (hTabStops[i] > rel) { next = hTabStops[i]; break; }
  }
  if (next < 0) return;
  int32_t nx = leftMarginDots + next;
  if (nx >= rightMarginDots) return;
  cursorX = nx;
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
          lineFeedAction();
          return;
        case 0x0D: // CR: dibuja la linea pendiente y vuelve al margen izquierdo (sin avanzar papel: permite sobreimprimir)
          flushLineBuffer();
          cursorX = leftMarginDots;
          if (AUTO_LF_ON_CR) doLineFeed();
          return;
        case 0x0B: // VT: tabulador vertical (ESC e 1 n); sin tabuladores definidos actua como LF
          if (vTabIncDots > 0) {
            flushLineBuffer();
            cursorY = ((cursorY / vTabIncDots) + 1) * vTabIncDots;
            lineCellHeight = 0; lineMinAdvance = 0;
            if (AUTO_CR_ON_LF) cursorX = leftMarginDots;
          } else {
            lineFeedAction();
          }
          return;
        case 0x18: // CAN: borra el buffer de linea (el texto pendiente no se imprime) y el cursor vuelve donde empezaba
          if (lineBufCount > 0) { cursorX = lineBuf[0].x; lineBufCount = 0; }
          return;
        case 0x7F: // DEL: borra el ultimo caracter del buffer (o todo, segun DEL_CLEARS_WHOLE_LINE)
          if (lineBufCount > 0) {
            if (DEL_CLEARS_WHOLE_LINE) { cursorX = lineBuf[0].x; lineBufCount = 0; }
            else { lineBufCount--; cursorX = lineBuf[lineBufCount].x; }
          }
          return;
        case 0x0C: // FF
          finishPageIfNeeded();
          return;
        case 0x08: // BS
          cursorX -= computeAdvanceDots();
          if (cursorX < leftMarginDots) cursorX = leftMarginDots;
          return;
        case 0x09: // HT: tabulador horizontal (ver doHorizontalTab)
          doHorizontalTab();
          return;
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
            uint16_t adv = computeAdvanceDots();
            // Salto automatico: si el caracter no cabe antes del margen derecho,
            // la impresora hace CR+LF (independiente de AUTO_CR_ON_LF, que es
            // para el LF que manda el host) y lo imprime en la linea siguiente.
            // Solo se envuelve una vez: si no cupiera ni vacia la linea, se dibuja
            // igualmente (recortado) en vez de entrar en bucle.
            if (cursorX + (int32_t)adv > rightMarginDots) {
              flushLineBuffer(); // la linea que se cierra se dibuja (y alinea) antes de bajar
              cursorX = leftMarginDots;
              doLineFeed();
              if (cursorY >= PAGE_HEIGHT_DOTS) { // el salto acaba la hoja: pagina nueva sin perder el caracter
                finishPageIfNeeded();
                openNewPage();
                cursorX = leftMarginDots;
              }
            }
            lineBufPush(b); // se dibuja al cerrar la linea (flushLineBuffer)
            cursorX += adv;
          }
          // bytes >= 0x80 (juegos de caracteres extendidos/acentos) no
          // soportados: se ignoran de forma segura en vez de imprimir basura.
          return;
      }

    case ST_ESC:
      switch (b) {
        case '@': flushLineBuffer(); resetPrinterState(); escState = ST_NORMAL; return;      // ESC @ (el texto pendiente se imprime antes)
        case '2': lineSpacingDots = DEFAULT_LINE_DOTS; escState = ST_NORMAL; return; // 1/6"
        case '0': lineSpacingDots = 23; escState = ST_NORMAL; return;     // 1/8" (aprox)
        case '3': escState = ST_ESC_3; return;                            // ESC 3 n
        case 'E': boldMode = true;  escState = ST_NORMAL; return;         // negrita ON
        case 'F': boldMode = false; escState = ST_NORMAL; return;         // negrita OFF
        case 'G': boldMode = true;  escState = ST_NORMAL; return;         // doble golpe ON (se trata como negrita, ver limitaciones)
        case 'H': boldMode = false; escState = ST_NORMAL; return;         // doble golpe OFF
        case 'r': escState = ST_ESC_R; return;                            // color de cinta
        case '-': escState = ST_ESC_MINUS; return;                       // subrayado
        case '_': escState = ST_ESC_UNDERSCORE; return;                  // sobrerrayado
        case 'l': escState = ST_ESC_L_MARGIN; return;                    // ESC l n: margen izquierdo
        case 'Q': escState = ST_ESC_Q_MARGIN; return;                    // ESC Q n: margen derecho
        case 'a': escState = ST_ESC_A; return;                           // ESC a n: justificacion
        case ' ': escState = ST_ESC_SP; return;                          // ESC SP n: separacion extra entre caracteres
        case 'h': escState = ST_ESC_H; return;                           // ESC h n: ancho+alto de una vez / medio caracter
        case 'D': hTabCount = 0; hTabMode = 1; escState = ST_ESC_D; return; // ESC D n1..nk NUL: tabuladores (cancela los anteriores)
        case 'e': escState = ST_ESC_E_M; return;                         // ESC e m n: incremento fijo de tabulador
        case 'f': escState = ST_ESC_F_M; return;                         // ESC f m n: salto horizontal/vertical
        case '$': escState = ST_ESC_DOLLAR_L; return;                    // ESC $ nL nH: posicion horizontal absoluta
        case '\\': escState = ST_ESC_BSLASH_L; return;                    // ESC \ nL nH: posicion horizontal relativa
        case 'R':                                                        // ESC R: tabuladores de fabrica (modo IBM) / juego internacional (modo Epson)
          if (ESC_R_RESETS_TABS) { hTabMode = 0; hTabCount = 0; vTabIncDots = 0; escState = ST_NORMAL; }
          else escState = ST_ESC_SKIP1; // ESC R n: n se descarta
          return;
        case '*': flushLineBuffer(); escState = ST_ESC_STAR_M; return;   // grafico ESC *
        case 'K': flushLineBuffer(); startGraphicsCapture(8, 60);  escState = ST_ESC_KLYZ_NL; return;
        case 'L': flushLineBuffer(); startGraphicsCapture(8, 120); escState = ST_ESC_KLYZ_NL; return;
        case 'Y': flushLineBuffer(); startGraphicsCapture(8, 120); escState = ST_ESC_KLYZ_NL; return;
        case 'Z': flushLineBuffer(); startGraphicsCapture(8, 240); escState = ST_ESC_KLYZ_NL; return;
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

    case ST_ESC_L_MARGIN: { // n columnas (valor binario crudo, como ESC 3) al CPI de AHORA
      int32_t m = (int32_t)b * computeColumnDots();
      if (m < rightMarginDots) {           // debe quedar a la izquierda del margen derecho; si no, se ignora
        leftMarginDots = m;
        if (cursorX < leftMarginDots) cursorX = leftMarginDots;
      }
      escState = ST_NORMAL;
      return;
    }

    case ST_ESC_Q_MARGIN: {
      int32_t m = (int32_t)b * computeColumnDots();
      if (m > PAGE_WIDTH_DOTS) m = PAGE_WIDTH_DOTS; // mas alla de la hoja: lo maximo posible
      if (m > leftMarginDots) rightMarginDots = m;  // debe quedar a la derecha del margen izquierdo; si no, se ignora
      escState = ST_NORMAL;
      return;
    }

    case ST_ESC_SP: // n unidades (byte crudo, 0..255): n/180" en NLQ, n/120" en borrador
      charSpaceN = b;
      escState = ST_NORMAL;
      return;

    case ST_ESC_H: { // ver tabla ESC_H_*: 0 normal, 1 doble, 2 cuadruple, 4/5 doble sup/inf, 6/7 cuadruple sup/inf
      uint8_t v = decodeNumericParam(b);
      if (v < 8 && ESC_H_WMUL[v] != 0) {
        widthMultiplier = ESC_H_WMUL[v];
        heightMultiplier = ESC_H_HMUL[v];
        halfMode = ESC_H_HALF[v];
      } else {
        Serial.printf("[DEBUG] ESC h %u no definido: se ignora\n", v);
      }
      escState = ST_NORMAL;
      return;
    }

    case ST_ESC_A: { // justificacion: 0 izquierda, 1 centro, 2 derecha (3 = completa: no soportada, se usa izquierda)
      uint8_t v = decodeNumericParam(b);
      if (v <= 2) justification = v;
      else { justification = 0; Serial.println("[DEBUG] ESC a 3 (justificacion completa) no soportada: se usa izquierda"); }
      escState = ST_NORMAL;
      return;
    }

    case ST_ESC_D: { // columnas (byte crudo) al paso vigente; termina con NUL, con un valor no creciente o al llegar a MAX_HTABS
      int32_t pos = (int32_t)b * computeColumnDots();
      if (b == 0 || hTabCount >= MAX_HTABS || (hTabCount > 0 && pos <= hTabStops[hTabCount - 1])) {
        escState = ST_NORMAL;
        if (b != 0) handleByteInner(b); // no era un tabulador: se procesa como dato normal
        return;
      }
      hTabStops[hTabCount++] = pos;
      return;
    }

    case ST_ESC_E_M: escParam = decodeNumericParam(b); escState = ST_ESC_E_N; return;
    case ST_ESC_E_N:
      if (escParam == 0 && b > 0) { hTabFixedDots = (int32_t)b * computeColumnDots(); hTabMode = 2; } // horizontal: cada n columnas del paso vigente
      else if (escParam == 1)      { vTabIncDots = (int32_t)b * lineSpacingDots; }                      // vertical: cada n lineas (0 = VT es LF)
      escState = ST_NORMAL;
      return;

    case ST_ESC_F_M: escParam = decodeNumericParam(b); escState = ST_ESC_F_N; return;
    case ST_ESC_F_N:
      if (escParam == 0) { // salto horizontal: n columnas del paso vigente, sin pasar del margen derecho
        int32_t nx = cursorX + (int32_t)b * computeAdvanceDots();
        cursorX = (nx > rightMarginDots) ? rightMarginDots : nx;
      } else if (escParam == 1) { // salto vertical: n lineas
        for (uint8_t i = 0; i < b; i++) {
          lineFeedAction();
          if (cursorY >= PAGE_HEIGHT_DOTS) { // el salto acaba la hoja: sigue en la hoja siguiente
            Serial.println("[INFO] Fin de hoja alcanzado: cerrando pagina automaticamente.");
            finishPageIfNeeded();
            openNewPage();
            cursorX = leftMarginDots;
          }
        }
      }
      escState = ST_NORMAL;
      return;

    case ST_ESC_DOLLAR_L: escWord = b; escState = ST_ESC_DOLLAR_H; return;
    case ST_ESC_DOLLAR_H: { // absoluta: n/60" desde el margen izquierdo (3 puntos a 180dpi); se ignora si pasa del margen derecho
      int32_t nx = leftMarginDots + (int32_t)(escWord | ((uint16_t)b << 8)) * 3;
      if (nx <= rightMarginDots) cursorX = nx;
      escState = ST_NORMAL;
      return;
    }

    case ST_ESC_BSLASH_L: escWord = b; escState = ST_ESC_BSLASH_H; return;
    case ST_ESC_BSLASH_H: { // relativa con signo: 1/180" en NLQ (1 punto), 1/120" en borrador (1,5 puntos); se ignora fuera de margenes
      int32_t v = (int16_t)(escWord | ((uint16_t)b << 8));
      int32_t d = lqMode ? v : (v * 3) / 2;
      int32_t nx = cursorX + d;
      if (nx >= leftMarginDots && nx <= rightMarginDots) cursorX = nx;
      escState = ST_NORMAL;
      return;
    }

    case ST_ESC_UNDERSCORE:
      overscoreMode = (decodeNumericParam(b) != 0);
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
      widthMultiplier = (decodeNumericParam(b) != 0) ? 2 : 1;
      escState = ST_NORMAL;
      return;

    case ST_ESC_LOWER_W: {
      uint8_t v = decodeNumericParam(b);
      heightMultiplier = (v == 1) ? 2 : (v == 2) ? 4 : 1; // 0=normal 1=doble 2=cuadruple, cualquier otro valor -> normal
      halfMode = 0;                                       // ESC w siempre es caracter entero (los medios caracteres se piden con ESC h 4..7)
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
          finishGraphicsBlock();
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
          finishGraphicsBlock();
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

  allocBand(); // antes de tocar band[]
  memset(band, COLOR_WHITE_INDEX, BAND_BYTES);
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

  // Texto pendiente sin terminar (sin LF/CR/FF) y el host se ha callado: se dibuja
  // igualmente para que se vea en la web y no se pierda si se corta la corriente.
  if (lineBufCount > 0 && (millis() - lastByteMillis > LINE_IDLE_FLUSH_MS)) {
    flushLineBuffer();
  }

  // Si llevamos un rato sin recibir nada y hay una pagina con contenido sin
  // cerrar (el host no envio Form Feed final), la cerramos igualmente.
  if (pageOpen && pageHasInk && (millis() - lastByteMillis > IDLE_TIMEOUT_MS)) {
    Serial.println("[INFO] Sin actividad: cerrando pagina automaticamente.");
    finishPageIfNeeded();
  }

  updateStatusLed();
}
