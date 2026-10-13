/*
 * ==========================================================================
 *  WiFi + portal de configuracion + OTA + servidor web
 * ==========================================================================
 *
 * IMPORTANTE: este fichero es una SEGUNDA PESTANA del mismo sketch que
 * impresora_epson_esp32.ino. Para que el IDE de Arduino lo compile junto
 * con el resto, tiene que estar guardado en la MISMA CARPETA que ese
 * fichero (el IDE junta automaticamente todas las pestanas .ino de una
 * carpeta en un unico programa). No es un sketch aparte.
 *
 * Que hace:
 *   - Al arrancar, intenta conectar a la ultima red WiFi guardada. Si no
 *     hay ninguna guardada, o falla la conexion, el ESP32 monta su PROPIO
 *     punto de acceso llamado "ImpresoraEpson-Setup"; conectate a el desde
 *     un movil u ordenador y se abrira solo (portal cautivo) una pagina
 *     donde elegir la red WiFi de casa/taller y su contrasena. Una vez
 *     configurada queda guardada en la memoria no volatil del ESP32 y en
 *     los siguientes arranques se conecta solo, sin volver a preguntar.
 *   - Libreria usada para eso: "WiFiManager" de tzapu (de terceros, codigo
 *     abierto) -- instalarla desde el Gestor de Librerias del IDE de
 *     Arduino buscando "WiFiManager" (autor: tzapu). ArduinoOTA y
 *     WebServer, en cambio, ya vienen incluidas en el propio nucleo ESP32
 *     de Arduino: no hace falta instalar nada mas para esas dos.
 *   - Los AJUSTES DE LA IMPRESORA (baudios del puerto serie, control de flujo por
 *     hardware y por software, Auto LF / Auto CR y la direccion de la impresora) se
 *     cambian en http://<ip>/config, servida por el servidor web normal: sin
 *     reiniciar y sin cambiar de red. Se guardan en la memoria flash (Preferences) y
 *     se aplican al momento. Los mismos campos siguen estando tambien en el portal
 *     del WiFi (deja el SSID en blanco y pulsa Save: la red no se toca).
 *   - OTA (ArduinoOTA): permite subir un nuevo firmware por WiFi desde el
 *     IDE de Arduino (Herramientas -> Puerto -> aparecera el ESP32 por
 *     red ademas de por USB) sin necesidad de cable. Protegido con
 *     contrasena, ver OTA_PASSWORD mas abajo (cambiala).
 *   - Servidor web (puerto 80) con:
 *       /              pagina principal, muestra la imagen y se refresca
 *                      sola cada 2 segundos
 *       /current.bmp   la pagina que se esta imprimiendo AHORA MISMO (o la
 *                      ultima cerrada si no hay ninguna abierta), servida
 *                      desde la cache PSRAM (o la SD si no hay) mientras se
 *                      sigue escribiendo -- el fichero se crea desde el
 *                      principio con el tamano completo de la hoja (cabecera
 *                      definitiva y fondo blanco), asi que es siempre un BMP
 *                      valido aunque la pagina no se haya terminado de
 *                      imprimir todavia (ver materializePage())
 *       /config        ajustes de la impresora (baudios, control de flujo, Auto
 *                      LF/CR, direccion de la impresora): formulario servido
 *                      por este mismo servidor, se aplican al momento
 *       /wifi-portal   abre a peticion el portal de configuracion del WiFi
 *                      (la misma del primer arranque) sin borrar la red
 *                      guardada: sirve para cambiar los ajustes de la impresora
 *       /list          lista de todos los PAGEnnnn.BMP que hay en la SD, cada
 *                      uno con un boton [pdf] (si existe su PAGEnnnn.PDF), un
 *                      boton Print (POST /print: imprime la hoja en formato
 *                      PWG raster por un socket TCP crudo, tipo JetDirect, a
 *                      la direccion ippUrl) y un boton para borrarlo (POST
 *                      /delete, que borra el BMP y su PDF)
 *       /wifi-reset    borra la red WiFi guardada y reinicia (para volver
 *                      a configurar otra red)
 *
 * ARQUITECTURA: todo esto (WiFiManager, OTA y el servidor web) corre en su
 * PROPIA TAREA de FreeRTOS, anclada al nucleo 0 (ver startWifiWebTask()),
 * mientras que el bucle principal de Arduino (el que procesa el puerto
 * serie de la impresora) sigue corriendo normalmente en el nucleo 1. Asi,
 * el autoConnect() de WiFiManager -- que es bloqueante, y puede tardar
 * hasta WIFI_CONFIG_PORTAL_TIMEOUT_S segundos si no hay red configurada --
 * NUNCA retrasa el arranque de la emulacion de impresora: el ESP32 esta
 * listo para recibir e imprimir desde el primer instante, tenga o no WiFi.
 *
 * Como ahora SI hay dos nucleos que de verdad pueden tocar la tarjeta SD al
 * mismo tiempo (el bucle principal escribiendo, este servidor leyendo),
 * todo acceso a la SD desde aqui esta protegido por el mismo mutex que usa
 * el emulador de impresora (sdAccessBegin()/sdAccessEnd(), definidas en
 * impresora_epson_esp32.ino) -- imprescindible, SD.h no es segura frente a
 * accesos concurrentes desde dos tareas/nucleos distintos.
 * ==========================================================================
 */

#include <WiFi.h>
#include <WiFiManager.h>   // libreria de terceros: "WiFiManager" de tzapu, aqui solo para el debug en PC.
#include <ArduinoOTA.h>    // incluida en el nucleo ESP32 de Arduino
#include <WebServer.h>     // incluida en el nucleo ESP32 de Arduino
#include <freertos/task.h> // xTaskCreatePinnedToCore -- incluida en el nucleo ESP32 de Arduino

#define WIFI_WEB_TASK_STACK_BYTES 8192 // subela si ves un stack overflow en esta tarea
#define WIFI_WEB_TASK_CORE 0           // nucleo 0: el bucle principal (impresora) corre en el 1
#define WIFI_WEB_TASK_PRIORITY 1

#define OTA_HOSTNAME "impresora-epson"
#define OTA_PASSWORD "cambiame123"   // cambia esto antes de usarlo
#define WIFI_CONFIG_AP_NAME "ImpresoraEpson-Setup"
#define WIFI_CONFIG_PORTAL_TIMEOUT_S 180 // si nadie configura nada en 3 min,
                                          // sigue arrancando sin WiFi en vez
                                          // de quedarse esperando para siempre

#define WEB_REFRESH_MS 2000          // cada cuanto se refresca sola la imagen en /
#define WEB_STREAM_CHUNK_BYTES 4096  // trozo de lectura al servir desde la SD (antes
                                      // era un buffer pequeno interno de streamFile())

// Reintentos rapidos con las credenciales ya guardadas, antes de recurrir a
// WiFiManager. Pensado para el tipico "E (...) wifi:Association refused too
// many times, max allowed 1" que suelta a veces el propio IDF justo tras un
// reset o reflasheo: casi siempre es el router, que todavia tiene colgada
// la sesion anterior del ESP32 y la suelta sola al cabo de unos segundos.
// Como todo esto corre en su propia tarea (ver startWifiWebTask()), estos
// reintentos con espera NUNCA afectan a la impresora.
#define WIFI_QUICK_RETRY_COUNT      3    // cuantos intentos rapidos antes de WiFiManager
#define WIFI_QUICK_RETRY_WAIT_MS    5000 // cuanto se espera a que conecte cada intento
#define WIFI_QUICK_RETRY_DELAY_MS   5000 // pausa entre un intento fallido y el siguiente

WebServer webServer(80);

// --- Pagina principal ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<title>Emulador Impresora ESC/P</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>body{font-family:sans-serif;background:#222;color:#eee;text-align:center}"
                "img{max-width:95%;border:1px solid #555;margin-top:10px;background:#fff}"
                "a{color:#8cf}</style></head><body>"
                "<h2>Emulador Impresora ESC/P (ESP32)</h2>";
  if (pageOpen && pageFileCreated) {
    html += "<p>Imprimiendo ahora: " + String(currentFileName) + "</p>";
  } else if (pageIndex > 0) {
    html += "<p>Sin pagina abierta ahora mismo (se muestra la ultima generada).</p>";
  } else {
    html += "<p>Todavia no se ha generado ninguna pagina.</p>";
  }
  if (bmpCacheCapacity > 0) {
    html += "<p style='font-size:0.85em;color:#9c9'>Cache PSRAM: " +
            String((unsigned long)(bmpCacheBytes / 1024)) + " / " +
            String((unsigned long)(bmpCacheCapacity / 1024)) + " KB usados</p>";
  } else {
    html += "<p style='font-size:0.85em;color:#c99'>Cache PSRAM no disponible: sirviendo desde la SD</p>";
  }
  html += "<img id='pg' src='/current.bmp' onerror=\"this.style.display='none'\">"
          "<p><a href='/list'>Ver todas las paginas</a> &middot; "
          "<a href='/current.bmp'>Pagina Actual</a> &middot; "
          "<a href='/config'>Configurar impresora</a> &middot; "
          "<a href='/wifi-portal'>Cambiar red WiFi</a> &middot; "
          "<a href='/wifi-reset'>Olvidar WiFi</a></p>"
          "<script>setInterval(function(){"
          "document.getElementById('pg').style.display='';"
          "document.getElementById('pg').src='/current.bmp?' + Date.now();"
          "}, " + String((unsigned long)WEB_REFRESH_MS) + ");</script>"
          "</body></html>";
  webServer.send(200, "text/html", html);
}

// --- Sirve un fichero .BMP de la SD como respuesta HTTP (sin cache: para
// paginas ya cerradas o para cualquier fichero que no sea el que esta en
// curso ahora mismo) ---
// Envia un fichero ya abierto, en trozos de WEB_STREAM_CHUNK_BYTES. El
// mutex de la SD solo se toma alrededor de cada f.read() individual (unos
// pocos milisegundos), nunca mientras se manda el trozo por WiFi (que con
// una red lenta puede tardar bastante) -- asi el bucle principal, que
// imprime, nunca espera mas de lo que tarda una lectura suelta de la SD.
void sendFileChunked(File &f, uint32_t totalSize, const char *contentType) {
  webServer.setContentLength(totalSize);
  webServer.send(200, contentType, "");
  WiFiClient client = webServer.client();
  client.setNoDelay(true); // manda cada trozo ya, sin esperar a llenar el paquete TCP

  uint8_t buf[WEB_STREAM_CHUNK_BYTES];
  uint32_t remaining = totalSize;
  while (remaining > 0) {
    size_t toRead = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
    sdAccessBegin();
    size_t n = f.read(buf, toRead);
    sdAccessEnd();
    if (n == 0) break; // no deberia pasar, pero evita un bucle infinito
    client.write(buf, n);
    remaining -= n;
    yield(); // deja respirar al resto del sistema (WiFi, watchdog...) en ficheros grandes
  }
}

// --- Sirve un fichero .BMP de la SD como respuesta HTTP (sin cache: para
// paginas ya cerradas o para cualquier fichero que no sea el que esta en
// curso ahora mismo) ---
void streamBmpFile(const char *path) {
  sdAccessBegin();
  File f = SD.open(path, FILE_READ);
  uint32_t size = f ? f.size() : 0;
  sdAccessEnd();
  if (!f) {
    webServer.send(404, "text/plain", "No hay ninguna pagina generada todavia.");
    return;
  }
  String p = path;
  p.toUpperCase();
  sendFileChunked(f, size, p.endsWith(".PDF") ? "application/pdf" : "image/bmp"); // tambien sirve los .PDF de las hojas
  sdAccessBegin();
  f.close();
  sdAccessEnd();
}

// --- Sirve la pagina EN CURSO combinando la cache PSRAM (rapido) con la SD
// (solo para la "cola" que no quepa en la cache, si la pagina ha crecido
// mas alla del presupuesto de WEB_BMP_CACHE_MAX_BYTES) ---
void streamCurrentBmpCached() {
  uint32_t totalSize = pageFileTotalBytes(); // el fichero de la pagina en curso siempre tiene el tamano completo de la hoja

  if (!bmpCacheBuffer || bmpCacheBytes == 0) {
    // Sin cache utilizable todavia (placa sin PSRAM, o pagina recien abierta):
    // se sirve igual que cualquier otro fichero, directamente de la SD.
    streamBmpFile(currentFileName);
    return;
  }

  webServer.sendHeader("Cache-Control", "no-store");
  webServer.setContentLength(totalSize);
  webServer.send(200, "image/bmp", "");
  WiFiClient client = webServer.client();
  client.setNoDelay(true); // manda cada trozo ya, sin esperar a llenar el paquete TCP

  // Parte servida directamente desde la cache PSRAM: sin tocar la SD para
  // nada, ni falta que hace el mutex (es solo RAM).
  uint32_t fromCache = (bmpCacheBytes < totalSize) ? bmpCacheBytes : totalSize;
  client.write(bmpCacheBuffer, fromCache);

  // Si la pagina ha crecido mas de lo que cabe en la cache, el resto se lee
  // de la SD a partir de justo donde se quedo la cache, con el mismo
  // bloqueo fino (solo durante cada lectura) que sendFileChunked().
  if (fromCache < totalSize) {
    sdAccessBegin();
    File f = SD.open(currentFileName, FILE_READ);
    if (f) f.seek(fromCache);
    sdAccessEnd();
    if (f) {
      uint8_t buf[WEB_STREAM_CHUNK_BYTES];
      uint32_t remaining = totalSize - fromCache;
      while (remaining > 0) {
        size_t toRead = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        sdAccessBegin();
        size_t n = f.read(buf, toRead);
        sdAccessEnd();
        if (n == 0) break; // no deberia pasar, pero evita un bucle infinito
        client.write(buf, n);
        remaining -= n;
        yield(); // deja respirar al resto del sistema (WiFi, watchdog...) en ficheros grandes
      }
      sdAccessBegin();
      f.close();
      sdAccessEnd();
    }
  }
}

// --- /current.bmp: la pagina en curso, o la ultima cerrada si no hay ninguna abierta ---
void handleCurrentBmp() {
  if (pageOpen && pageFileCreated) { // (una pagina abierta pero aun sin tinta no tiene fichero: se sirve la ultima cerrada)
    streamCurrentBmpCached();
    return;
  }
  if (pageIndex > 0) {
    char path[32];
    snprintf(path, sizeof(path), "/PAGE%04lu.BMP", (unsigned long)pageIndex);
    streamBmpFile(path);
    return;
  }
  webServer.send(404, "text/plain", "Todavia no se ha generado ninguna pagina.");
}

// --- /list: listado de paginas guardadas en la SD ---
// Nombre de hoja generada por la impresora: exactamente "PAGE" + 4 digitos + ".BMP", sin
// barras ni rutas. Es lo unico que /delete acepta borrar (evita rutas arbitrarias).
bool isPageFileName(const String &n) {
  if (n.length() != 12 || !n.startsWith("PAGE") || !n.endsWith(".BMP")) return false;
  for (int i = 4; i < 8; i++) if (n[i] < '0' || n[i] > '9') return false;
  return true;
}

// --- /list: listado de paginas guardadas en la SD, cada una con su boton de borrar ---
void handleList() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Paginas</title>"
                 "<style>body{font-family:sans-serif}"
                 ".del{font-size:.7em;padding:0 .4em;margin-left:.4em;color:#b00;cursor:pointer}"
                 ".prn{font-size:.7em;padding:0 .4em;margin-left:.4em;cursor:pointer}"
                 "a.pdf{font-size:.7em;padding:0 .4em;margin-left:.4em;border:1px solid #767676;border-radius:3px;"
                 "background:#efefef;color:#000;text-decoration:none}</style></head><body>"
                 "<h2>Paginas en la SD</h2><ul>";
  sdAccessBegin(); // listado breve: un unico bloqueo para toda la funcion
  File root = SD.open("/");
  if (root) {
    File entry = root.openNextFile();
    while (entry) {
      String name = entry.name();
      if (name.endsWith(".BMP") || name.endsWith(".bmp")) {
        // entry.name() a veces viene con barra inicial y a veces sin ella
        // segun la version del core; se normaliza para que el enlace sea
        // siempre una ruta absoluta valida ("/PAGE0001.BMP").
        String href = name.startsWith("/") ? name : ("/" + name);
        String bare = name.startsWith("/") ? name.substring(1) : name;
        html += "<li><a href='" + href + "'>" + name + "</a> (" +
                String(entry.size()) + " bytes)";
        if (pageOpen && pageFileCreated && href == String(currentFileName)) {
          html += " <small>(imprimiendose ahora)</small>"; // la hoja en curso no se puede borrar (y su PDF aun no existe)
        } else if (isPageFileName(bare)) {
          // [pdf]: descarga del PDF de la hoja (PAGEnnnn.PDF), solo si existe. Los PDF no se listan aparte.
          String pdfName = bare.substring(0, 8) + ".PDF";
          if (SD.exists(("/" + pdfName).c_str())) {
            html += " <a class='pdf' href='/" + pdfName + "' download='" + pdfName + "' title='Descargar el PDF de la hoja'>pdf</a>";
          }
          // Print: imprime la hoja (PWG raster por socket crudo, a la direccion ippUrl). POST + confirmacion, como Borrar (gasta papel).
          html += " <form method='POST' action='/print' style='display:inline' "
                  "onsubmit=\"return confirm('Imprimir " + bare + " en la impresora?')\">"
                  "<input type='hidden' name='f' value='" + bare + "'>"
                  "<button type='submit' class='prn' title='Imprimir la hoja en la impresora'>Print</button></form>";
          // POST (no un enlace GET: un enlace podria borrarse al precargarlo el navegador o al
          // seguirlo un rastreador) + confirmacion en el navegador antes de enviar.
          html += " <form method='POST' action='/delete' style='display:inline' "
                  "onsubmit=\"return confirm('Borrar " + bare + " de la SD?')\">"
                  "<input type='hidden' name='f' value='" + bare + "'>"
                  "<button type='submit' class='del' title='Borrar de la SD'>Borrar</button></form>";
        }
        html += "</li>";
      }
      entry = root.openNextFile();
    }
    root.close();
  }
  sdAccessEnd();
  html += "</ul><p><a href='/'>Volver</a></p></body></html>";
  webServer.send(200, "text/html", html);
}

// --- /delete (solo POST): borra de la SD una hoja PAGEnnnn.BMP y su PAGEnnnn.PDF (si existe) y vuelve al listado ---
void handleDelete() {
  String f = webServer.arg("f");
  if (!isPageFileName(f)) {
    webServer.send(400, "text/plain", "Nombre de fichero no valido.");
    return;
  }
  String path = "/" + f;
  String pdfPath = "/" + f.substring(0, 8) + ".PDF"; // el PDF de la hoja: mismo nombre, extension .PDF
  int result = 0; // 0 = borrada, 1 = no existe, 2 = se esta imprimiendo, 3 = fallo al borrar
  sdAccessBegin(); // la comprobacion de "hoja en curso" va DENTRO del bloqueo: asi no puede cambiar mientras se borra
  if (pageOpen && pageFileCreated && path == String(currentFileName)) result = 2;
  else {
    bool hasBmp = SD.exists(path.c_str()), hasPdf = SD.exists(pdfPath.c_str());
    if (!hasBmp && !hasPdf) result = 1;
    else {
      // se intentan borrar los dos aunque falle el primero; cualquier fallo -> 500
      if (hasBmp && !SD.remove(path.c_str())) result = 3;
      if (hasPdf && !SD.remove(pdfPath.c_str())) result = 3;
    }
  }
  sdAccessEnd();
  if (result == 2) {
    webServer.send(409, "text/plain", "Esa pagina se esta imprimiendo ahora mismo: no se puede borrar hasta que se cierre.");
    return;
  }
  if (result == 3) {
    webServer.send(500, "text/plain", "No se pudo borrar " + f + " (o su PDF) de la SD.");
    return;
  }
  if (result == 0) Serial.printf("[INFO] Pagina borrada desde la web: %s (y su PDF, si existia)\n", path.c_str());
  // 303 + Location: el navegador recarga /list con un GET, asi que F5 no repite el borrado.
  webServer.sendHeader("Location", "/list");
  webServer.send(303, "text/plain", "");
}

// ===== PWG PRINT BEGIN =====
// --- Impresion de una hoja (boton "Print" de /list): PWG Raster por socket TCP crudo (JetDirect) ---
// Lee PAGEnnnn.BMP de la SD (no el PDF) y lo manda a la impresora de ippUrl en formato
// image/pwg-raster (PWG 5102.4) por un socket TCP en bruto, como hace "AppSocket/JetDirect"
// (puerto 9100 por defecto): sin HTTP ni IPP, solo el flujo de bytes y cerrar. El flujo es la
// marca "RaS2" + una pagina: cabecera de 1796 bytes (formato cups_page_header2_t, enteros en
// big-endian; los mismos campos que escribe cupsRasterInitPWGHeader) + las filas comprimidas
// con el PackBits propio de PWG. La pagina es sRGB de 8 bits (24 bits/pixel): cada indice del BMP
// se convierte a RGB con la tabla de color del PROPIO BMP, asi que no depende de paleta_cmyk.h.
//
// GEOMETRIA (ajustable aqui): hoja PWG_PAGE_W_MM x PWG_PAGE_H_MM a PWG_DPI. La imagen se escala
// al ancho de la hoja menos dos margenes de PWG_SIDE_MARGIN_MM (mismo criterio que el PDF), por
// vecino mas cercano (cada punto del BMP es un punto de la matricial), y se centra en vertical.
// OJO: la impresora solo acepta las resoluciones de su atributo pwg-raster-document-resolution-
// supported (y el tamano de su media-supported): si no sale nada, empieza por PWG_DPI.
//
// COMPRESION (PWG): cada linea va precedida de su numero de repeticiones - 1 (0..255) y despues
// una secuencia de bloques: n = 0..127 -> el pixel siguiente se repite n+1 veces; n = 129..255 ->
// siguen 257-n pixeles distintos (2..128); 128 no se usa. Aprovechando que la hoja es casi toda
// blanca: cada fila ORIGINAL se codifica una sola vez (aunque ocupe 1-2 filas de la hoja) y las
// filas originales iguales consecutivas (hojas en blanco, interlineado) se funden en una linea con
// su contador de repeticiones. Como no hace falta saber el tamano total (es un socket en bruto),
// se codifica y envia en un solo paso, leyendo de la SD en bloques de PWG_ROWS_PER_READ filas.
// Bloquea la tarea web mientras dura el envio (el servidor no atiende otras peticiones).
#define PWG_DPI                300
#define PWG_PAGE_W_MM          210.0f   // A4 vertical (para Carta: 215.9 x 279.4 y PWG_PAGE_SIZE_NAME "na_letter_8.5x11in")
#define PWG_PAGE_H_MM          297.0f
#define PWG_SIDE_MARGIN_MM       5.0f
#define PWG_PAGE_SIZE_NAME     "iso_a4_210x297mm"
#define PWG_DEFAULT_PORT       9100
#define PWG_ROWS_PER_READ         8     // filas del BMP que se leen de la SD de una vez
#define PWG_SEND_BUF_BYTES    16384     // se acumula la salida y se manda al socket en trozos de este tamano
#define PWG_CONNECT_TIMEOUT_MS 8000
#define PWG_IO_TIMEOUT_MS     30000     // maximo sin poder enviar un solo byte (la impresora procesa/frena)
#define PWG_HEADER_BYTES       1796
#define PWG_WHITE_INDEX         256     // indice de paleta extra: blanco del papel (margenes)
#define PWG_ALIGN4(v) (((size_t)(v) + 3) & ~(size_t)3)

// Estado del envio (la tarea web hace una sola impresion a la vez)
static WiFiClient *pwgClient = nullptr;
static uint8_t *pwgSendBuf = nullptr;
static size_t pwgSendLen = 0;
static bool pwgNetFail = false;
static uint32_t pwgBytesOut = 0;

// Descompone la URL/direccion de la impresora para el socket crudo. Admite socket://, raw://,
// jetdirect://, tcp:// (el puerto indicado se respeta; si no hay, 9100), una direccion sin
// esquema ("192.168.1.70" o "192.168.1.70:9100") y, por comodidad, las URL ipp://, ipps://,
// http://, https://: en estas el puerto de la URL es el de IPP/HTTP (631, 80, 443), asi que si es
// uno de esos se usa 9100; cualquier otro puerto explicito (p. ej. ipp://host:9101) se respeta.
bool parsePrinterUrl(const char *url, char *host, size_t hostCap, uint16_t *port) {
  bool isRaw = true;
  const char *sch = strstr(url, "://");
  if (sch) {
    size_t sl = (size_t)(sch - url);
    auto is = [&](const char *name) { return strlen(name) == sl && strncasecmp(url, name, sl) == 0; };
    if (is("socket") || is("raw") || is("jetdirect") || is("tcp")) isRaw = true;
    else if (is("ipp") || is("ipps") || is("http") || is("https")) isRaw = false;
    else return false;
    url = sch + 3;
  }
  size_t hl = 0;
  while (url[hl] && url[hl] != ':' && url[hl] != '/') hl++;
  if (hl == 0 || hl >= hostCap) return false;
  memcpy(host, url, hl); host[hl] = 0; url += hl;
  uint32_t pv = 0;
  if (*url == ':') {
    url++;
    int digits = 0;
    while (*url >= '0' && *url <= '9') { pv = pv * 10 + (uint32_t)(*url - '0'); if (pv > 65535) return false; url++; digits++; }
    if (digits == 0 || pv == 0) return false;
  }
  if (*url && *url != '/') return false;
  if (pv == 0 || (!isRaw && (pv == 631 || pv == 80 || pv == 443))) pv = PWG_DEFAULT_PORT;
  *port = (uint16_t)pv;
  return true;
}

void pwgPut32(uint8_t *h, int off, uint32_t v) {
  h[off] = (uint8_t)(v >> 24); h[off + 1] = (uint8_t)(v >> 16); h[off + 2] = (uint8_t)(v >> 8); h[off + 3] = (uint8_t)v;
}

// Cabecera de pagina PWG (1796 bytes): sRGB 8 bits, chunked, una cara. Mismos campos y valores que
// escribe cupsRasterInitPWGHeader() de libcups (comprobado contra libcups en el PC).
void pwgBuildHeader(uint8_t *h, uint32_t wPx, uint32_t hPx) {
  memset(h, 0, PWG_HEADER_BYTES);
  strcpy((char *)h, "PwgRaster");                                   // MediaClass
  pwgPut32(h, 276, PWG_DPI); pwgPut32(h, 280, PWG_DPI);             // HWResolution
  uint32_t wPt = (uint32_t)(PWG_PAGE_W_MM * 72.0f / 25.4f), hPt = (uint32_t)(PWG_PAGE_H_MM * 72.0f / 25.4f);
  pwgPut32(h, 292, wPt); pwgPut32(h, 296, hPt);                     // ImagingBoundingBox (derecha, arriba) en puntos
  pwgPut32(h, 352, wPt); pwgPut32(h, 356, hPt);                     // PageSize en puntos
  pwgPut32(h, 372, wPx); pwgPut32(h, 376, hPx);                     // cupsWidth, cupsHeight en pixeles
  pwgPut32(h, 384, 8); pwgPut32(h, 388, 24); pwgPut32(h, 392, wPx * 3); // bits/color, bits/pixel, bytes/linea
  pwgPut32(h, 396, 0);                                              // ColorOrder: chunked (RGBRGB...)
  pwgPut32(h, 400, 19);                                             // ColorSpace: CUPS_CSPACE_SRGB
  pwgPut32(h, 420, 3);                                              // NumColors
  pwgPut32(h, 456, 1); pwgPut32(h, 460, 1);                         // CrossFeedTransform, FeedTransform
  pwgPut32(h, 480, 0xFFFFFF);                                       // AlternatePrimary (blanco)
  strlcpy((char *)h + 1732, PWG_PAGE_SIZE_NAME, 64);                // PageSizeName
}

// Codifica una linea de n pixeles (indices de paleta, 256 = blanco) en el PackBits de PWG.
// Devuelve los bytes escritos en out (como maximo 4*n).
size_t pwgEncodeLine(const uint16_t *idx, uint32_t n, const uint8_t pal[][3], uint8_t *out) {
  size_t o = 0;
  uint32_t i = 0;
  while (i < n) {
    uint32_t run = 1;
    while (i + run < n && run < 128 && idx[i + run] == idx[i]) run++;
    if (run >= 2) {                                   // repeticion: n-1, pixel
      out[o++] = (uint8_t)(run - 1);
      memcpy(out + o, pal[idx[i]], 3); o += 3;
      i += run;
    } else {                                          // pixeles distintos: hasta que empiece una repeticion
      uint32_t L = 1;
      while (i + L < n && L < 128) {
        if (i + L + 1 < n && idx[i + L] == idx[i + L + 1]) break;
        L++;
      }
      if (L == 1) {                                   // un solo pixel = repeticion de 1
        out[o++] = 0;
        memcpy(out + o, pal[idx[i]], 3); o += 3;
      } else {                                        // literal de L pixeles (2..128): 257-L
        out[o++] = (uint8_t)(257 - L);
        for (uint32_t k = 0; k < L; k++) { memcpy(out + o, pal[idx[i + k]], 3); o += 3; }
      }
      i += L;
    }
  }
  return o;
}

// Manda todos los bytes al socket (write() puede aceptar menos). false si se cierra o pasan
// PWG_IO_TIMEOUT_MS sin avanzar.
bool printerSendAll(WiFiClient *c, const uint8_t *p, size_t n) {
  size_t off = 0;
  uint32_t last = millis();
  while (off < n) {
    size_t w = c->write(p + off, n - off);
    if (w > 0) { off += w; last = millis(); continue; }
    if (!c->connected() || (uint32_t)(millis() - last) > PWG_IO_TIMEOUT_MS) return false;
    delay(1);
  }
  return true;
}
bool pwgFlush() {
  if (pwgNetFail) return false;
  if (pwgSendLen) {
    pwgNetFail = !printerSendAll(pwgClient, pwgSendBuf, pwgSendLen);
    pwgBytesOut += (uint32_t)pwgSendLen;
    pwgSendLen = 0;
    yield();
  }
  return !pwgNetFail;
}
bool pwgPut(const uint8_t *p, size_t n) {
  if (pwgNetFail) return false;
  while (n > 0) {
    size_t room = PWG_SEND_BUF_BYTES - pwgSendLen;
    size_t k = (n < room) ? n : room;
    memcpy(pwgSendBuf + pwgSendLen, p, k);
    pwgSendLen += k; p += k; n -= k;
    if (pwgSendLen == PWG_SEND_BUF_BYTES && !pwgFlush()) return false;
  }
  return true;
}
// Emite 'count' repeticiones de una linea ya codificada (el contador de PWG llega a 256 por linea)
bool pwgEmitLine(const uint8_t *line, size_t len, uint32_t count) {
  while (count > 0) {
    uint32_t r = (count < 256) ? count : 256;
    uint8_t rep = (uint8_t)(r - 1);
    if (!pwgPut(&rep, 1) || !pwgPut(line, len)) return false;
    count -= r;
  }
  return true;
}

// Imprime la hoja bmpPath (PAGEnnnn.BMP) en la impresora de ippUrl. true si se ha enviado entera
// (el socket crudo no da confirmacion: la impresora puede aun rechazarla). msg = texto para el usuario.
bool printPageRaster(const char *bmpPath, char *msg, size_t msgCap) {
  msg[0] = 0;
  if (!ippUrl[0]) { snprintf(msg, msgCap, "No hay direccion de impresora configurada (usa Configurar impresora)."); return false; }
  char host[80]; uint16_t port;
  if (!parsePrinterUrl(ippUrl, host, sizeof(host), &port)) {
    snprintf(msg, msgCap, "Direccion de impresora no valida: %s", ippUrl);
    return false;
  }

  // --- Geometria (pixeles de la hoja a PWG_DPI); la imagen se centra
  const uint32_t W = PAGE_WIDTH_DOTS, H = PAGE_HEIGHT_DOTS;
  const uint32_t srcRow = (W + 3) & ~3u;                              // bytes por fila del BMP (8 bits, multiplo de 4)
  const uint32_t pageW = (uint32_t)(PWG_PAGE_W_MM * PWG_DPI / 25.4f);
  const uint32_t pageH = (uint32_t)(PWG_PAGE_H_MM * PWG_DPI / 25.4f);
  uint32_t imgW = (uint32_t)((PWG_PAGE_W_MM - 2.0f * PWG_SIDE_MARGIN_MM) * PWG_DPI / 25.4f + 0.5f);
  if (imgW > pageW) imgW = pageW;
  uint32_t imgH = (imgW * H + W / 2) / W;
  if (imgH > pageH) { imgH = pageH; imgW = (imgH * W + H / 2) / H; }
  const uint32_t padL = (pageW - imgW) / 2, padT = (pageH - imgH) / 2, padB = pageH - imgH - padT;

  // --- Memoria de trabajo (un bloque; se libera al terminar)
  const size_t szSrc = PWG_ALIGN4((size_t)srcRow * PWG_ROWS_PER_READ > BMP_HEADER_BYTES ? (size_t)srcRow * PWG_ROWS_PER_READ : BMP_HEADER_BYTES);
  const size_t szPrev = PWG_ALIGN4(W), szHdr = PWG_ALIGN4(PWG_HEADER_BYTES);
  const size_t szIdx = PWG_ALIGN4((size_t)pageW * 2), szLut = PWG_ALIGN4((size_t)imgW * 2);
  const size_t szLine = PWG_ALIGN4((size_t)pageW * 4 + 16), szSend = PWG_SEND_BUF_BYTES;
  uint8_t *blk = (uint8_t *)malloc(szSrc + szPrev + szHdr + szIdx + szLut + szLine + szSend);
  if (!blk) { snprintf(msg, msgCap, "Memoria insuficiente para imprimir."); return false; }
  uint8_t *src = blk, *prevRow = src + szSrc, *hdr = prevRow + szPrev;
  uint16_t *idx = (uint16_t *)(hdr + szHdr), *lut = (uint16_t *)((uint8_t *)idx + szIdx);
  uint8_t *line = (uint8_t *)lut + szLut;
  pwgSendBuf = line + szLine;

  // --- BMP: cabecera, comprobaciones y tabla de color
  sdAccessBegin();
  File f = SD.open(bmpPath, FILE_READ);
  size_t got = f ? f.read(src, BMP_HEADER_BYTES) : 0;
  sdAccessEnd();
  auto rd32 = [&](int o) { return (uint32_t)src[o] | ((uint32_t)src[o + 1] << 8) | ((uint32_t)src[o + 2] << 16) | ((uint32_t)src[o + 3] << 24); };
  if (!f || got != BMP_HEADER_BYTES) {
    if (f) { sdAccessBegin(); f.close(); sdAccessEnd(); }
    free(blk); pwgSendBuf = nullptr;
    snprintf(msg, msgCap, "No se pudo leer %s de la SD.", bmpPath);
    return false;
  }
  uint32_t dataOff = rd32(10);
  if (src[0] != 'B' || src[1] != 'M' || rd32(14) != 40 || rd32(18) != W || (int32_t)rd32(22) != -(int32_t)H ||
      src[28] != 8 || rd32(30) != 0 || dataOff < BMP_HEADER_BYTES) {
    sdAccessBegin(); f.close(); sdAccessEnd();
    free(blk); pwgSendBuf = nullptr;
    snprintf(msg, msgCap, "%s no tiene el formato esperado (BMP de %dx%d, 8 bits, de arriba a abajo).", bmpPath, (int)W, (int)H);
    return false;
  }
  if ((uint64_t)f.size() < (uint64_t)dataOff + (uint64_t)srcRow * H) {   // fichero incompleto: no se abre la conexion
    sdAccessBegin(); f.close(); sdAccessEnd();
    free(blk); pwgSendBuf = nullptr;
    snprintf(msg, msgCap, "%s esta incompleto (menos filas de las esperadas).", bmpPath);
    return false;
  }
  uint8_t pal[PWG_WHITE_INDEX + 1][3];
  for (int i = 0; i < BMP_PALETTE_COLORS; i++) { pal[i][0] = src[54 + 4 * i + 2]; pal[i][1] = src[54 + 4 * i + 1]; pal[i][2] = src[54 + 4 * i]; } // BGR0 -> RGB
  pal[PWG_WHITE_INDEX][0] = pal[PWG_WHITE_INDEX][1] = pal[PWG_WHITE_INDEX][2] = 255;
  for (uint32_t x = 0; x < imgW; x++) lut[x] = (uint16_t)((x * W) / imgW);   // columna de la hoja -> columna del BMP
  sdAccessBegin(); f.seek(dataOff); sdAccessEnd();

  // --- Conexion y flujo: "RaS2" + cabecera de pagina
  WiFiClient client;
  Serial.printf("[PRINT] Enviando %s a %s:%u (PWG raster %lux%lu a %d dpi)\n", bmpPath, host, (unsigned)port, (unsigned long)pageW, (unsigned long)pageH, PWG_DPI);
  if (!client.connect(host, port, PWG_CONNECT_TIMEOUT_MS)) {
    sdAccessBegin(); f.close(); sdAccessEnd();
    free(blk); pwgSendBuf = nullptr;
    snprintf(msg, msgCap, "No se pudo conectar con la impresora (%s:%u).", host, (unsigned)port);
    return false;
  }
  pwgClient = &client; pwgSendLen = 0; pwgNetFail = false; pwgBytesOut = 0;
  uint32_t t0 = millis();
  pwgBuildHeader(hdr, pageW, pageH);
  bool ok = pwgPut((const uint8_t *)"RaS2", 4) && pwgPut(hdr, PWG_HEADER_BYTES);

  // --- Margen superior (lineas en blanco)
  size_t lineLen = 0;
  auto encodeBlank = [&]() {
    for (uint32_t x = 0; x < pageW; x++) idx[x] = PWG_WHITE_INDEX;
    lineLen = pwgEncodeLine(idx, pageW, pal, line);
  };
  if (ok && padT) { encodeBlank(); ok = pwgEmitLine(line, lineLen, padT); }

  // --- Imagen: cada fila del BMP se codifica una vez; las iguales consecutivas se funden en una linea
  bool sdFail = false, haveLine = false;
  uint32_t pend = 0, rowsDone = 0;
  while (ok && rowsDone < H) {
    uint32_t nrows = (H - rowsDone < PWG_ROWS_PER_READ) ? (H - rowsDone) : PWG_ROWS_PER_READ;
    sdAccessBegin();
    size_t n = f.read(src, (size_t)nrows * srcRow);
    sdAccessEnd();
    if (n != (size_t)nrows * srcRow) { sdFail = true; break; }
    for (uint32_t r = 0; r < nrows && ok; r++) {
      uint32_t i = rowsDone + r;
      uint32_t cnt = (uint32_t)(((uint64_t)(i + 1) * imgH + H - 1) / H) - (uint32_t)(((uint64_t)i * imgH + H - 1) / H); // filas de la hoja que cubre esta fila del BMP
      if (cnt == 0) continue;
      const uint8_t *row = src + (size_t)r * srcRow;
      if (haveLine && memcmp(row, prevRow, W) == 0) { pend += cnt; continue; }
      if (haveLine) ok = pwgEmitLine(line, lineLen, pend);
      if (!ok) break;
      for (uint32_t x = 0; x < padL; x++) idx[x] = PWG_WHITE_INDEX;
      for (uint32_t x = 0; x < imgW; x++) idx[padL + x] = row[lut[x]];
      for (uint32_t x = padL + imgW; x < pageW; x++) idx[x] = PWG_WHITE_INDEX;
      lineLen = pwgEncodeLine(idx, pageW, pal, line);
      memcpy(prevRow, row, W);
      haveLine = true; pend = cnt;
    }
    rowsDone += nrows;
  }
  if (ok && !sdFail && haveLine) ok = pwgEmitLine(line, lineLen, pend);

  // --- Margen inferior y fin
  if (ok && !sdFail && padB) { encodeBlank(); ok = pwgEmitLine(line, lineLen, padB); }
  if (ok && !sdFail) ok = pwgFlush();
  client.stop();
  sdAccessBegin(); f.close(); sdAccessEnd();
  uint32_t ms = millis() - t0;
  pwgClient = nullptr; pwgSendBuf = nullptr;
  free(blk);

  if (sdFail) snprintf(msg, msgCap, "Error leyendo %s de la SD; envio cancelado.", bmpPath);
  else if (!ok) snprintf(msg, msgCap, "Se corto la conexion con la impresora al enviar la hoja (%lu bytes enviados).", (unsigned long)pwgBytesOut);
  else snprintf(msg, msgCap, "Hoja enviada a la impresora (%s:%u, %lu bytes en %lu ms).", host, (unsigned)port, (unsigned long)pwgBytesOut, (unsigned long)ms);
  Serial.printf("[PRINT] %s %s\n", (ok && !sdFail) ? "OK:" : "ERROR:", msg);
  return ok && !sdFail;
}
// ===== PWG PRINT END =====

// Escapa <, >, &, ' y " para meter un texto (p. ej. el mensaje de la impresora) en HTML
String htmlEscape(const char *t) {
  String o;
  for (; *t; t++) {
    switch (*t) {
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '&': o += "&amp;"; break;
      case '\'': o += "&#39;"; break;
      case '"': o += "&quot;"; break;
      default: o += *t;
    }
  }
  return o;
}

// --- /print (solo POST): imprime la hoja PAGEnnnn.BMP (PWG raster por socket crudo) en la impresora de ippUrl ---
// Responde con una pagina con el resultado (no redirige: asi se ve el motivo si falla).
void handlePrint() {
  String f = webServer.arg("f");
  if (!isPageFileName(f)) {
    webServer.send(400, "text/plain", "Nombre de fichero no valido.");
    return;
  }
  String bmpPath = "/" + f;
  bool printing, exists;
  sdAccessBegin();
  printing = pageOpen && pageFileCreated && bmpPath == String(currentFileName);
  exists = SD.exists(bmpPath.c_str());
  sdAccessEnd();
  if (printing) {
    webServer.send(409, "text/plain", "Esa pagina se esta recibiendo ahora mismo: espere a que se cierre.");
    return;
  }
  if (!exists) {
    webServer.send(404, "text/plain", "No existe " + f + " en la SD.");
    return;
  }
  char msg[256];
  bool ok = printPageRaster(bmpPath.c_str(), msg, sizeof(msg));
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Imprimir</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>body{font-family:sans-serif}</style></head><body><h2>Imprimir " + htmlEscape(f.c_str()) + "</h2><p>" +
                (ok ? "&#10004; " : "&#10008; ") + htmlEscape(msg) + "</p><p><a href='/list'>Volver al listado</a></p></body></html>";
  webServer.send(ok ? 200 : 502, "text/html", html);
}

// --- Sirve cualquier .BMP o .PDF que exista en la SD por su nombre de fichero
// (ej. /PAGE0007.BMP o /PAGE0007.PDF), que es a donde apuntan los enlaces de /list. Se
// registra con onNotFound() porque esos nombres no se conocen de antemano
// (se van creando segun se imprime), asi que no se puede dar de alta una
// ruta fija por cada uno con webServer.on() ---
void handlePossibleBmpFile() {
  String uri = webServer.uri(); // p.ej. "/PAGE0007.BMP"
  if (uri.endsWith(".BMP") || uri.endsWith(".bmp") || uri.endsWith(".PDF") || uri.endsWith(".pdf")) {
    sdAccessBegin();
    bool exists = SD.exists(uri.c_str());
    sdAccessEnd();
    if (exists) {
      streamBmpFile(uri.c_str());
      return;
    }
  }
  webServer.send(404, "text/plain", "No encontrado: " + uri);
}

// --- /wifi-reset: olvida la red guardada y reinicia para volver a configurar ---
void handleWifiReset() {
  webServer.send(200, "text/plain", "Borrando la configuracion WiFi y reiniciando...");
  delay(300);
  WiFiManager wm;
  wm.resetSettings();
  delay(200);
  ESP.restart();
}

// Reintentos rapidos reutilizando las credenciales ya guardadas (las mismas
// que usa WiFiManager: WiFi.begin() sin argumentos reconecta con lo ultimo
// guardado en la NVS). Si no hay ninguna red guardada todavia (primer
// arranque), no tiene sentido reintentar nada: se devuelve false al
// instante para pasar directamente al portal de configuracion.
bool tryQuickWifiReconnect() {
  if (WiFi.SSID().length() == 0) return false;

  for (int attempt = 1; attempt <= WIFI_QUICK_RETRY_COUNT; attempt++) {
    Serial.printf("[WIFI] Intento rapido de reconexion %d/%d...\n", attempt, WIFI_QUICK_RETRY_COUNT);
    WiFi.begin();
    unsigned long start = millis();
    while (millis() - start < WIFI_QUICK_RETRY_WAIT_MS) {
      if (WiFi.status() == WL_CONNECTED) return true;
      delay(100);
    }
    if (attempt < WIFI_QUICK_RETRY_COUNT) delay(WIFI_QUICK_RETRY_DELAY_MS);
  }
  return false;
}

// ===== AJUSTES DE LA IMPRESORA EN LA PAGINA DE CONFIGURACION DEL WIFI =====
//
// Se anaden como "parametros personalizados" de WiFiManager: aparecen en la misma pagina "Configure
// WiFi", debajo de la contrasena. Los campos de texto/numero son WiFiManagerParameter normales
// (WiFiManager guarda lo escrito en el parametro al pulsar Save). Una casilla sin marcar NO se envia
// en el formulario, asi que WiFiManager no la puede "leer" como un campo: se escriben como HTML libre
// y se consultan en el callback con server->hasArg(). OJO: WiFiManager guarda PUNTEROS a las cadenas
// (id, etiqueta, HTML), no las copia: tienen que seguir vivas mientras dura el portal (de ahi los
// buffers y objetos estaticos).
static char cfgHtmlBuf[1500];                 // casillas de verificacion (HTML), se reescribe antes de cada portal
static char cfgBaudStr[12];
static WiFiManagerParameter *pBaud = nullptr; // campo de baudios
static WiFiManagerParameter *pIpp = nullptr;  // campo de la URL IPP
static WiFiManager *gPortalWm = nullptr;      // el WiFiManager que tiene el portal abierto (para leer las casillas)
volatile bool configPortalRequested = false;  // /wifi-portal pide abrir el portal; lo abre loopWifiOtaWeb() en esta misma tarea

// Quita espacios de los extremos y los caracteres que romperian el HTML del formulario cuando
// el valor se vuelve a escribir en value='...' (comillas y < >).
static void sanitizeUrl(char *url) {
  char *src = url, *dst = url;
  while (*src == ' ' || *src == '\t') src++;
  for (; *src; src++) { if (*src != '\'' && *src != '"' && *src != '<' && *src != '>') *dst++ = *src; }
  while (dst > url && (dst[-1] == ' ' || dst[-1] == '\t' || dst[-1] == '\r' || dst[-1] == '\n')) dst--;
  *dst = 0;
}

// ===== AJUSTES DE LA IMPRESORA: LOGICA COMPARTIDA (portal de WiFiManager y pagina /config) BEGIN =====
// Valida y aplica los ajustes, los guarda en la memoria flash y, si cambian los baudios o el control
// de flujo, pide a loop() (el nucleo que usa Serial2) que los re-aplique al puerto serie. Devuelve los
// avisos en texto plano, uno por linea, o "" si todo fue bien. Un valor no valido (baudios, direccion
// de la impresora) no impide guardar el resto: los baudios no validos se ignoran y la direccion no
// valida se guarda tal cual con un aviso.
static String applyPrinterSettings(const char *baudStr, bool hw, bool xon, bool autoLf, bool autoCr,
                                   bool autoPrint, const char *url, const char *origin) {
  String warn;
  bool serialChanged = false;

  // Baudios: solo si el valor es un numero valido; si no, se mantiene el actual.
  char *endp = nullptr;
  uint32_t b = strtoul(baudStr, &endp, 10);
  if (baudStr[0] != 0 && *endp == 0 && validBaud(b)) {
    if (b != serialBaud) serialChanged = true;
    serialBaud = b;
  } else {
    Serial.printf("[CONFIG] Baudios no validos ('%s'; se admite 300-1000000): se mantiene %lu\n", baudStr, (unsigned long)serialBaud);
    warn += "Baudios no validos ('" + String(baudStr) + "'; se admite 300-1000000): se mantiene " + String((unsigned long)serialBaud) + "\n";
  }

  if (hw != useHwFlowControl) serialChanged = true;
  useHwFlowControl = hw;
  useXonXoff = xon;
  autoLfOnCr = autoLf;
  autoCrOnLf = autoCr;
  autoIPPPrint = autoPrint;

  // Direccion de la impresora para el boton Print (socket crudo, puerto 9100 por defecto).
  strncpy(ippUrl, url, IPP_URL_MAX_LEN);
  ippUrl[IPP_URL_MAX_LEN] = 0;
  sanitizeUrl(ippUrl);
  {
    char hostChk[80]; uint16_t portChk;
    if (ippUrl[0] && !parsePrinterUrl(ippUrl, hostChk, sizeof(hostChk), &portChk)) {
      Serial.println("[CONFIG] AVISO: la direccion de la impresora no es valida (ej. socket://192.168.1.70:9100); se guarda igualmente.");
      warn += "La direccion de la impresora no es valida (ej. socket://192.168.1.70:9100); se ha guardado igualmente.\n";
    }
  }

  if (saveConfig()) printConfig(origin);
  else warn += "No se pudo guardar en la memoria flash: los ajustes valen hasta el proximo reinicio.\n";
  if (serialChanged) {
    serialConfigDirty = true; // loop() re-aplica baudios y control de flujo
    Serial.println("[CONFIG] Baudios / control de flujo cambiados: se re-aplican ahora al puerto serie.");
  }
  return warn;
}
// ===== AJUSTES DE LA IMPRESORA: LOGICA COMPARTIDA END =====

// Callback de WiFiManager: se llama al pulsar Save, DESPUES de copiar los campos a sus parametros.
static void onSavePrinterParams() {
  if (!gPortalWm || !pBaud || !pIpp) return;
  // Casillas de verificacion: marcada = el navegador envia el campo; desmarcada = no llega.
  applyPrinterSettings(pBaud->getValue(),
                       gPortalWm->server->hasArg("cfg_hwflow"), gPortalWm->server->hasArg("cfg_xonxoff"),
                       gPortalWm->server->hasArg("cfg_autolf"), gPortalWm->server->hasArg("cfg_autocr"),
                       gPortalWm->server->hasArg("cfg_autoprint"), pIpp->getValue(),
                       "Configuracion guardada desde la pagina de configuracion del WiFi:");
}

// Anade al portal los campos de la impresora, con los valores ACTUALES como valores por defecto.
static void addPrinterParams(WiFiManager &wm) {
  snprintf(cfgBaudStr, sizeof(cfgBaudStr), "%lu", (unsigned long)serialBaud);
  snprintf(cfgHtmlBuf, sizeof(cfgHtmlBuf),
           "<br/><label><input type='checkbox' name='cfg_hwflow' value='1'%s> Control de flujo por hardware (RTS/CTS)</label>"
           "<br/><label><input type='checkbox' name='cfg_xonxoff' value='1'%s> Control de flujo por software (XON/XOFF)</label>"
           "<br/><label><input type='checkbox' name='cfg_autolf' value='1'%s> Auto LF: un CR tambien hace salto de linea</label>"
           "<br/><label><input type='checkbox' name='cfg_autocr' value='1'%s> Auto CR: un LF tambien vuelve al margen izquierdo</label>"
           "<br/><label><input type='checkbox' name='cfg_autoprint' value='1'%s> Automatic print of complete page on configured IPP Printer</label>"
           "<br/>",
           useHwFlowControl ? " checked" : "", useXonXoff ? " checked" : "",
           autoLfOnCr ? " checked" : "", autoCrOnLf ? " checked" : "", autoIPPPrint ? " checked" : "");

  static WiFiManagerParameter paramHead("<br/><b>Ajustes de la impresora</b>");
  static WiFiManagerParameter paramBaud("cfg_baud", "Baudios del puerto serie (300-1000000)", cfgBaudStr, 7,
                                        "type='number' min='300' max='1000000'");
  static WiFiManagerParameter paramFlags(cfgHtmlBuf); // HTML libre: lee cfgHtmlBuf cada vez que se dibuja la pagina
  static WiFiManagerParameter paramIpp("cfg_ippurl", "Impresora para Print: socket://IP[:puerto] (9100 por defecto)", ippUrl, IPP_URL_MAX_LEN,
                                       "placeholder='socket://192.168.1.70:9100'");
  paramBaud.setValue(cfgBaudStr, 7);          // refresca el valor mostrado (el objeto es estatico y dura todo el programa)
  paramIpp.setValue(ippUrl, IPP_URL_MAX_LEN);
  pBaud = &paramBaud;
  pIpp = &paramIpp;
  gPortalWm = &wm;

  wm.addParameter(&paramHead);
  wm.addParameter(&paramBaud);
  wm.addParameter(&paramFlags);
  wm.addParameter(&paramIpp);
  wm.setSaveParamsCallback(onSavePrinterParams);
}

// --- /config: AJUSTES DE LA IMPRESORA, servidos por el servidor web normal (puerto 80, la misma red) ---
// GET muestra el formulario con los valores actuales; POST los aplica y guarda (sin reiniciar ni cambiar
// de red; los baudios y el control de flujo se re-aplican al puerto serie al momento) y vuelve a mostrar
// el formulario con el resultado. Los campos se llaman igual que en el portal de WiFiManager.
void handleConfig() {
  String notice;
  if (webServer.method() == HTTP_POST) {
    String warn = applyPrinterSettings(webServer.arg("cfg_baud").c_str(),
                                       webServer.hasArg("cfg_hwflow"), webServer.hasArg("cfg_xonxoff"),
                                       webServer.hasArg("cfg_autolf"), webServer.hasArg("cfg_autocr"),
                                       webServer.hasArg("cfg_autoprint"), webServer.arg("cfg_ippurl").c_str(),
                                       "Configuracion guardada desde la pagina /config del servidor web:");
    if (warn.length() == 0) {
      notice = "<p class='ok'>&#10004; Configuracion guardada y aplicada.</p>";
    } else {
      String w = htmlEscape(warn.c_str());
      w.replace("\n", "<br>");
      notice = "<p class='warn'>&#9888; Configuracion guardada, con avisos:<br>" + w + "</p>";
    }
  }
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Configurar impresora</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>body{font-family:sans-serif;max-width:34em;margin:1em auto;padding:0 1em}"
                "label{display:block;margin:.5em 0}input[type=text],input[type=number]{width:100%;box-sizing:border-box;padding:.3em}"
                ".ok{color:#070}.warn{color:#a50}small{color:#555}</style></head><body>"
                "<h2>Ajustes de la impresora</h2>" + notice +
                "<form method='POST' action='/config'>"
                "<label>Baudios del puerto serie (300-1000000)"
                "<input type='number' name='cfg_baud' min='300' max='1000000' value='" + String((unsigned long)serialBaud) + "'></label>"
                "<label><input type='checkbox' name='cfg_hwflow' value='1'" + (useHwFlowControl ? " checked" : "") + "> Control de flujo por hardware (RTS/CTS)</label>"
                "<label><input type='checkbox' name='cfg_xonxoff' value='1'" + (useXonXoff ? " checked" : "") + "> Control de flujo por software (XON/XOFF)</label>"
                "<label><input type='checkbox' name='cfg_autolf' value='1'" + (autoLfOnCr ? " checked" : "") + "> Auto LF: un CR tambien hace salto de linea</label>"
                "<label><input type='checkbox' name='cfg_autocr' value='1'" + (autoCrOnLf ? " checked" : "") + "> Auto CR: un LF tambien vuelve al margen izquierdo</label>"
                "<label><input type='checkbox' name='cfg_autoprint' value='1'" + (autoIPPPrint ? " checked" : "") + "> Automatic print of complete page on configured IPP Printer</label>"
                "<label>Impresora para Print: socket://IP[:puerto] (9100 por defecto)"
                "<input type='text' name='cfg_ippurl' maxlength='" + String((int)IPP_URL_MAX_LEN) + "' placeholder='socket://192.168.1.70:9100' value='" + htmlEscape(ippUrl) + "'></label>"
                "<p><button type='submit'>Guardar</button></p></form>"
                "<p><small>Los cambios se guardan en la memoria flash y se aplican al momento: no hace falta reiniciar. "
                "Para cambiar la <b>red WiFi</b> (SSID y contrase&ntilde;a) hay que usar el <a href='/wifi-portal'>portal de WiFi</a>, "
                "que abre una red propia.</small></p>"
                "<p><a href='/'>Inicio</a> &middot; <a href='/list'>Ver todas las paginas</a></p></body></html>";
  webServer.send(200, "text/html", html);
}

// --- /wifi-portal: abre a peticion el portal de WiFiManager para cambiar la RED WiFi (SSID y contrasena) ---
// El portal se monta en la tarea WiFi (nucleo 0), no en el manejador HTTP: lo abre loopWifiOtaWeb().
// Aqui si hace falta conectarse a la red propia del ESP32 (el cambio de red no se puede hacer desde la red vieja).
void handleConfigPortal() {
  webServer.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Cambiar red WiFi</title></head><body>"
    "<h2>Abriendo el portal de configuracion del WiFi</h2>"
    "<p>En unos segundos esta pagina dejara de responder. Conecta el movil u ordenador a la red WiFi "
    "<b>" WIFI_CONFIG_AP_NAME "</b> y abre <b>http://192.168.4.1</b> (se abre sola en la mayoria de los "
    "casos) &rarr; <i>Configure WiFi</i>.</p>"
    "<p>Elige la nueva red y su contrase&ntilde;a y pulsa <i>Save</i>. Si no quieres cambiar de red, "
    "pulsa <i>Exit</i>: el portal tambien se cierra solo a los 3 minutos. (Los ajustes de la impresora ya "
    "no hace falta cambiarlos aqui: estan en <b>/config</b>.)</p>"
    "</body></html>");
  configPortalRequested = true;
}

static void runOnDemandConfigPortal() {
  Serial.println("[WIFI] Abriendo a peticion la pagina de configuracion (AP \"" WIFI_CONFIG_AP_NAME "\")...");
  delay(600);          // dejar que el navegador reciba la respuesta de /wifi-portal antes de cerrar el servidor
  webServer.stop();    // el portal de WiFiManager necesita el puerto 80
  {
    WiFiManager wm;
    wm.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT_S);
    addPrinterParams(wm);
    wm.startConfigPortal(WIFI_CONFIG_AP_NAME); // bloquea hasta Save / Exit / timeout; con la red ya conectada la mantiene
    gPortalWm = nullptr;
  }
  webServer.begin();
  if (WiFi.status() != WL_CONNECTED) tryQuickWifiReconnect();
  Serial.println("[WIFI] Pagina de configuracion cerrada.");
}

void setupWifiOtaWeb() {
  // Antes de recurrir a WiFiManager (que si falla aqui, abre su propio
  // punto de acceso de configuracion), unos pocos intentos rapidos con la
  // red ya guardada: resuelve solo el rechazo transitorio de asociacion sin
  // molestar con el portal cuando la red es, de hecho, la correcta.
  bool connected = tryQuickWifiReconnect();

  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT_S);
  addPrinterParams(wm); // ajustes de la impresora en la pagina de configuracion del WiFi
  if (!connected) {
    // autoConnect() intenta de nuevo la ultima red guardada; si tampoco lo
    // consigue, monta el punto de acceso de configuracion con ese nombre y
    // se bloquea aqui hasta que alguien la configura o pasa el timeout de
    // arriba.
    connected = wm.autoConnect(WIFI_CONFIG_AP_NAME);
  }
  gPortalWm = nullptr; // el portal ya no esta abierto (y wm deja de existir al salir de esta funcion)
  if (!connected) {
    Serial.println("[WIFI] Sin conexion (no configurada o fuera de alcance). "
                    "La impresora sigue funcionando sin red; conectate al "
                    "punto de acceso \"" WIFI_CONFIG_AP_NAME "\" para configurarla.");
  } else {
    Serial.print("[WIFI] Conectado. IP: ");
    Serial.println(WiFi.localIP());
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Actualizacion de firmware iniciada."); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] Actualizacion completada, reiniciando."); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error (%u).\n", (unsigned)error);
  });
  ArduinoOTA.begin();

  webServer.on("/", handleRoot);
  webServer.on("/current.bmp", handleCurrentBmp);
  webServer.on("/list", handleList);
  webServer.on("/print", HTTP_POST, handlePrint); // solo POST: imprime la hoja (PWG raster, socket crudo)
  webServer.on("/delete", HTTP_POST, handleDelete); // solo POST: un GET a /delete cae en onNotFound (404), nunca borra
  webServer.on("/config", handleConfig);              // ajustes de la impresora (GET formulario, POST guardar)
  webServer.on("/wifi-portal", handleConfigPortal);   // abre el portal de WiFiManager (cambiar de red WiFi)
  webServer.on("/wifi-reset", handleWifiReset);
  webServer.onNotFound(handlePossibleBmpFile); // sirve /PAGEnnnn.BMP y .PDF (enlaces de /list)
  webServer.begin();
  Serial.println("[WEB] Servidor web escuchando en el puerto 80.");
}

void loopWifiOtaWeb() {
  ArduinoOTA.handle();
  webServer.handleClient();
  if (configPortalRequested) {
    configPortalRequested = false;
    runOnDemandConfigPortal();
  }
}

// --- Tarea de FreeRTOS: aqui es donde de verdad se evita el bloqueo ---
// setupWifiOtaWeb() incluye el autoConnect() bloqueante de WiFiManager (puede
// tardar hasta WIFI_CONFIG_PORTAL_TIMEOUT_S segundos). Al correr dentro de
// esta tarea propia -- anclada al nucleo 0 -- ese bloqueo no afecta en nada
// al bucle principal de Arduino (impresora), que sigue corriendo libremente
// en el nucleo 1 desde el instante en que termina setup().
void wifiWebTaskFn(void *parameter) {
  setupWifiOtaWeb();
  for (;;) {
    loopWifiOtaWeb();
    vTaskDelay(pdMS_TO_TICKS(2)); // cede CPU brevemente en cada vuelta
  }
}

void startWifiWebTask() {
  xTaskCreatePinnedToCore(
    wifiWebTaskFn,
    "wifi_web",
    WIFI_WEB_TASK_STACK_BYTES,
    nullptr,
    WIFI_WEB_TASK_PRIORITY,
    nullptr,
    WIFI_WEB_TASK_CORE
  );
}
