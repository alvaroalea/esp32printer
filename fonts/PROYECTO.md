# Emulador Epson LQ 24 agujas color sobre ESP32-S3

Firmware Arduino que hace de impresora serie Epson LQ (24 agujas, color) para un
ordenador antiguo (contexto: usuario con varios proyectos Sinclair QL). Recibe
ESC/P por UART, renderiza a un lienzo virtual y escribe cada pagina como BMP
indexado en la SD, sirviendola tambien en vivo por WiFi mientras se genera.

## Ficheros (un unico sketch, 2 pestañas .ino + 2 headers; deben ir en la MISMA carpeta)
- `esp32printer.ino` — **fichero de referencia** (el usuario lo subio y edita el; no regenerar desde cero). Interprete ESC/P, render de texto/graficos, escritura BMP, boton, LED, setup()/loop() principal.
- `wifi_web.ino` — 2ª pestaña. WiFi (WiFiManager, portal config), OTA (ArduinoOTA), servidor web (WebServer) que sirve el BMP en curso. Corre en su propia tarea FreeRTOS (core 0).
- `font5x7.h` / `font8x8.h` — fuentes bitmap verificadas (extraidas de Adafruit GFX glcdfont.c y dhepper/font8x8, dominio publico; generadas por script, no transcritas a mano). 
- Licencia: GPLv3 (puesta por el usuario en la cabecera de esp32printer.ino).
- Claude o cualquier IA o LLM debe descomentar la primera linea y definir IADEVEL para su analisis.

## Hardware (ESP32-S3-WROOM-1 N16R8: 16MB flash, 8MB PSRAM)
- SD (SPI): CS=10 MOSI=11 SCK=12 MISO=13, `SD_SPI_FREQ_HZ`=24MHz.
- Serie impresora (MAX232, Serial2): TX=17 RX=18 RTS=15 CTS=16, `SERIAL_BAUD`=2400, flow control por HW (`USE_HW_FLOW_CONTROL`), XON/XOFF SW como alternativa (desactivado).
- Boton cierre manual de pagina ("Form Feed"): pin 46, pull-up EXTERNO (no INPUT_PULLUP), antirrebote 50ms.
- LED RGB WS2812 (1 unidad): pin 48, brillo 15% via `setBrightness()`. Estados (prioridad desc.): rojo=fallo SD, amarillo=destello 400ms al pulsar boton, celeste=escribiendo en SD, morado=recibiendo datos serie (150ms hold), azul=pagina abierta, verde=reposo. Logica en `computeLedColor()`/`updateStatusLed()`.

## Geometria de pagina y formato de fichero
- `PAGE_WIDTH_DOTS`=1440 (8"), `PAGE_HEIGHT_DOTS`=2043 (fijo, ~A4), rejilla interna 180dpi.
- Page height FIJA: un FF (o fin de hoja sin FF, o boton, o timeout 10min `IDLE_TIMEOUT_MS`) cierra y SIEMPRE rellena de blanco hasta `PAGE_HEIGHT_DOTS` — todas las paginas miden igual.
- **Formato BMP indexado de 4 bits** (16 colores paleta, 8 usados: negro/magenta/cian/violeta/amarillo/rojo/verde/blanco — cinta 4 bandas). `BMP_HEADER_BYTES`=118 (54+tabla color 16x4). ~1.4MB/pagina (antes 24bpp sin comprimir = ~8.4MB).
- Nombre fichero: `/PAGEnnnn.BMP`, `openNewPage()` busca el primer nn libre en la SD via `SD.exists()` (pageIndex en RAM no sobrevive reinicios).
- **Cabecera SIEMPRE coherente mientras se escribe**: `patchHeaderNow()` reescribe alto/tamaño tras cada tanda de filas (no solo al cerrar), para que un lector externo (el servidor web) pueda abrir el fichero a medio generar y sea un BMP valido (excepto una ventana muy breve a 0 filas justo al abrir pagina, antes del primer flush — alto=0, invalido para algunos lectores como PIL).
- Buffer de bandas en RAM: `band[BAND_HEIGHT=48][PAGE_WIDTH_DOTS]`, anillo indexado por `y % BAND_HEIGHT`; `ensureBandCovers()` vuelca a SD cuando hace falta sitio.
- Cache PSRAM para servir el BMP en curso sin tocar SD: `bmpCacheBuffer` (`ps_malloc`, `WEB_BMP_CACHE_MAX_BYTES`=2MB, cubre una pagina entera de sobra). `cacheAppend()`/`cachePatchU32()` lo mantienen en espejo con lo escrito en SD.

## Concurrencia (desde que WiFi se independizo del loop principal)
- `esp32printer.ino` → `setup()`/`loop()` en core 1 (Arduino estandar): escucha serie, imprime, SD.
- `wifi_web.ino` → tarea FreeRTOS propia en **core 0** (`startWifiWebTask()`), para que el `autoConnect()` bloqueante de WiFiManager (hasta `WIFI_CONFIG_PORTAL_TIMEOUT_S`=180s) NUNCA retrase el arranque de la impresora.
- `sdMutex` (FreeRTOS semaphore) protege TODO acceso a SD entre ambos nucleos. `sdWriteBegin()/End()` = lock + LED "escribiendo"; `sdAccessBegin()/End()` = lock puro (usa wifi_web.ino al leer, sin mentir con el LED). Servir un BMP grande bloquea el mutex SOLO durante cada lectura suelta de `WEB_STREAM_CHUNK_BYTES`=4096, nunca durante el envio WiFi completo (evita estancar la impresora con una descarga lenta).
- Ningun bloqueo esta anidado (semaforo no recursivo) — revisar esto si se tocan `openNewPage()`/`closePage()`/`finishPageIfNeeded()`.

## Servidor web (`wifi_web.ino`)
Rutas: `/` (visor con auto-refresco `WEB_REFRESH_MS`=2000ms), `/current.bmp` (pagina en curso o ultima cerrada, hibrido cache+SD), `/list` (enlaces a todas las `/PAGEnnnn.BMP`), `/wifi-reset` (borra credenciales WiFi y reinicia), `onNotFound` → sirve cualquier `.BMP` existente en la SD (asi funcionan los enlaces de `/list`).
WiFi: `WiFiManager` (tzapu, libreria 3ª) con AP de config `ImpresoraEpson-Setup`. `tryQuickWifiReconnect()`: 3 intentos rapidos (`WIFI_QUICK_RETRY_*`) con `WiFi.begin()` (credenciales NVS) antes de `wm.autoConnect()`, para mitigar `E (...) wifi:Association refused too many times` tras reset/reflash (rechazo transitorio del router). OTA: `ArduinoOTA` (hostname `impresora-epson`, password `cambiame123` — **cambiar**).

## ESC/P soportado (subconjunto; resto cae en `default` → log `[DEBUG] Comando ESC no soportado: 'c' (0xXX)` y se descarta 1 byte sin desincronizar)
- `ESC @` reset · LF/CR/FF/BS/HT/NUL/BEL · `ESC 2`/`ESC 0`/`ESC 3 n` interlineado
- `ESC K/L/Y/Z` y `ESC * m nL nH` graficos bit-image (8 o 24 agujas; >180dpi se fusiona por OR, no se estira)
- `ESC E/F/G/H` negrita·doble-golpe (unificados) · `ESC - n` subrayado (en borrador: a puntos alternos usando X ABSOLUTA de pagina, no local al caracter, para que no se rompa el patron entre caracteres de distinto CPI) · `ESC r n` color (0-6, paleta cinta)
- `ESC k n` tipo letra (0-6; ver limitacion fuentes abajo) · `ESC x n` calidad draft/NLQ · `ESC P/M/g` pica/elite/15cpi · SI(0x0F)/DC2(0x12) condensado · `ESC 4/5` cursiva (cizalla continua, SIN dividir entre 2 — la v1 dividida era casi imperceptible) · `ESC W n` ancho doble · `ESC w n` **alto doble/cuadruple — EXTENSION PROPIA, no ESC/P real** (ESC/P clasico no tiene comando de alto, fisicamente imposible en 1 pasada con cabeza de agujas fija; si el host usa otra secuencia, se vera en el log DEBUG) · `ESC S n`/`ESC T` super/subindice
- **Parametros tipo flag/enum pequeño** (r, -, k, x, W, S — NO el de ESC 3, que es magnitud 0-255) aceptan binario 0x00/0x01.. O digito ASCII 0x30-0x39 O digito ASCII+bit7 0xB0-0xB9 (`decodeNumericParam()`), por sistemas que mandan el parametro como caracter.
- `AUTO_LF_ON_CR`=false, `AUTO_CR_ON_LF`=true (el host del usuario solo manda LF esperando que la impresora vuelva al margen).

## Render de texto (`drawChar()`, en `chooseFont()`)
- **chooseFont() activa AHORA MISMO devuelve `font5x7` para TODO** (draft y las 7 tipografias NLQ por igual).
- Justo debajo, **anulado por la definicion de IADEVEL**, hay una 2ª `chooseFont()` ya escrita por el usuario (WIP, no activa) que diferencia font8x8 para Sans Serif y referencia fuentes nuevas por tipografia aun no definidas (`fontroman`/`fontcourier`/`fontprestige`/`fontscript`/`fontocrb`/`fontocra`, con sus `FONT0_COLS.. FONT6_*`) — el plan declarado del usuario es añadir esos headers de fuente mas adelante. No activarla ni tocarla sin que lo pida y se proporcionen dichos archivos .h.
- Escala dinamica: `scaleX` segun CPI actual, `scaleY` = `baseScaleY * heightMultiplier` (reducido 2/3 si super/subindice).
- Modo borrador (`!lqMode`): banding — solo se pinta la mitad superior de cada bloque de punto expandido (`paintRows`), efecto rayado tipico draft.
- Cursiva: cizalla continua fila a fila, SIN dividir (recorrido completo `fc.rows-1`).

## Como verificar cambios (no hay hardware real a mano)
Compilacion sintactica con g++ y stubs propios de Arduino/ESP32 (Adafruit_NeoPixel, SD, WiFi, WiFiManager, ArduinoOTA, WebServer, FreeRTOS...) en un entorno aparte — concatenar `esp32printer.ino`+`wifi_web.ino` (asi es como el IDE de Arduino junta las pestañas) y `g++ -std=c++14 -fsyntax-only -I stubs -x c++ combinado.cpp`. Para logica de render (fuentes, cursiva, subrayado, alto doble) ha sido util portar el algoritmo a Python+PIL y renderizar/inspeccionar visualmente antes de dar el cambio por bueno — coger los datos de fuente directamente de `font5x7.h`/`font8x8.h` via regex, no reescribirlos a mano.
**Ojo con el IDE de Arduino**: auto-genera prototipos de funciones insertandolos al principio del archivo concatenado; cualquier `struct` propio usado como tipo de retorno debe declararse ANTES de cualquier funcion (arriba del todo), o falla "'X' does not name a type" — ya corregido una vez (`FontChoice`, `GfxMode`).
