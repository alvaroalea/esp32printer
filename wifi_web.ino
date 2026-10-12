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
 *   - La pagina de configuracion del WiFi incluye ademas los AJUSTES DE LA
 *     IMPRESORA (baudios del puerto serie, control de flujo por hardware y por
 *     software, Auto LF / Auto CR y la URL de la impresora IPP). Se guardan en
 *     la memoria flash (Preferences) y se aplican al momento. Para cambiar solo
 *     eso, deja el SSID en blanco y pulsa Save: la red WiFi no se toca.
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
 *       /config        abre a peticion la pagina de configuracion del WiFi
 *                      (la misma del primer arranque) sin borrar la red
 *                      guardada: sirve para cambiar los ajustes de la impresora
 *       /list          lista de todos los PAGEnnnn.BMP que hay en la SD, cada
 *                      uno con un boton pdf (si existe su PAGEnnnn.PDF), un
 *                      boton Print (POST /print: envia ese PDF por IPP a la
 *                      URL ippUrl) y un boton para borrarlo (POST /delete,
 *                      que borra el BMP y su PDF)
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
#include <WiFiClientSecure.h> // IPP por TLS (ipps://, https://) -- incluida en el nucleo ESP32 de Arduino
#include <WiFiManager.h>   // libreria de terceros: "WiFiManager" de tzapu, aqui solo para el debug en PC.
#include <ArduinoOTA.h>    // incluida en el nucleo ESP32 de Arduino
#include <WebServer.h>     // incluida en el nucleo ESP32 de Arduino
#include <freertos/task.h> // xTaskCreatePinnedToCore -- incluida en el nucleo ESP32 de Arduino

#define WIFI_WEB_TASK_STACK_BYTES 16384 // subela si ves un stack overflow en esta tarea (16K: el handshake TLS de IPP por ipps:// gasta mucha pila)
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
          "<a href='/wifi-reset'>Reconfigurar WiFi</a></p>"
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
          // pdf: descarga del PDF de la hoja (PAGEnnnn.PDF), solo si existe. Los PDF no se listan aparte.
          String pdfName = bare.substring(0, 8) + ".PDF";
          if (SD.exists(("/" + pdfName).c_str())) {
            html += " <a class='pdf' href='/" + pdfName + "' download='" + pdfName + "' title='Descargar el PDF de la hoja'>pdf</a>";
            // Print: manda el PDF a la impresora IPP (ippUrl). POST + confirmacion, como Borrar (gasta papel).
            html += " <form method='POST' action='/print' style='display:inline' "
                    "onsubmit=\"return confirm('Enviar " + pdfName + " a la impresora IPP?')\">"
                    "<input type='hidden' name='f' value='" + bare + "'>"
                    "<button type='submit' class='prn' title='Imprimir el PDF en la impresora IPP'>Print</button></form>";
          }
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

// ===== IPP CLIENT BEGIN =====
// --- Impresion IPP del PDF de una hoja (boton "Print" de /list) ---
// Envia el PDF a la impresora de ippUrl con una operacion IPP Print-Job (RFC 8011): un POST HTTP
// con Content-Type application/ipp cuyo cuerpo es la peticion IPP (version 1.1, atributos
// minimos) seguida de los bytes del documento (document-format application/pdf). ipp:// y http://
// van en claro; ipps:// y https:// por TLS SIN validar el certificado (las impresoras usan
// certificados autofirmados). Puertos por defecto: ipp/ipps 631, http 80, https 443. El
// printer-uri que se manda es siempre ipp[s]://host:puerto/ruta, aunque la URL sea http[s]://.
// Bloquea la tarea web mientras dura el envio (el servidor no atiende otras peticiones).
#define IPP_CONNECT_TIMEOUT_MS    8000   // conectar con la impresora
#define IPP_IO_TIMEOUT_MS        30000   // maximo sin poder enviar un solo byte
#define IPP_RESPONSE_TIMEOUT_MS  90000   // espera de la respuesta tras enviar todo (la impresora procesa el trabajo)
#define IPP_SEND_CHUNK_BYTES      8192   // trozo de lectura de la SD / escritura al socket
#define IPP_USER_NAME "impresora-epson"  // requesting-user-name

static uint8_t ippChunk[IPP_SEND_CHUNK_BYTES]; // estaticos: la pila de esta tarea es pequena
static uint8_t ippHead[640];                   // peticion IPP (sin el documento)

// Descompone una URL de impresora. Devuelve false si no es valida (esquema desconocido, host
// vacio o demasiado largo, puerto incorrecto, IPv6 entre corchetes no soportado...).
bool parseIppUrl(const char *url, bool *tls, char *host, size_t hostCap, uint16_t *port, char *path, size_t pathCap) {
  uint16_t defPort;
  if      (!strncmp(url, "ipp://", 6))   { *tls = false; defPort = 631; url += 6; }
  else if (!strncmp(url, "ipps://", 7))  { *tls = true;  defPort = 631; url += 7; }
  else if (!strncmp(url, "http://", 7))  { *tls = false; defPort = 80;  url += 7; }
  else if (!strncmp(url, "https://", 8)) { *tls = true;  defPort = 443; url += 8; }
  else return false;
  size_t hl = 0;
  while (url[hl] && url[hl] != ':' && url[hl] != '/') hl++;
  if (hl == 0 || hl >= hostCap) return false;
  memcpy(host, url, hl); host[hl] = 0; url += hl;
  *port = defPort;
  if (*url == ':') {
    url++;
    uint32_t pv = 0; int digits = 0;
    while (*url >= '0' && *url <= '9') { pv = pv * 10 + (uint32_t)(*url - '0'); if (pv > 65535) return false; url++; digits++; }
    if (digits == 0 || pv == 0) return false;
    *port = (uint16_t)pv;
  }
  if (*url && *url != '/') return false;
  const char *pp = *url ? url : "/";
  if (strlen(pp) >= pathCap) return false;
  strcpy(path, pp);
  return true;
}

// Anade un atributo IPP (valor-tag, nombre, valor) en b[n..]. false si no cabe.
bool ippPutAttr(uint8_t *b, size_t *n, size_t cap, uint8_t tag, const char *name, const char *value) {
  size_t nl = strlen(name), vl = strlen(value);
  if (*n + 5 + nl + vl > cap) return false;
  size_t k = *n;
  b[k++] = tag;
  b[k++] = (uint8_t)(nl >> 8); b[k++] = (uint8_t)nl; memcpy(b + k, name, nl);  k += nl;
  b[k++] = (uint8_t)(vl >> 8); b[k++] = (uint8_t)vl; memcpy(b + k, value, vl); k += vl;
  *n = k;
  return true;
}

// Peticion IPP Print-Job: version 1.1, operacion 0x0002, request-id 1, grupo de atributos de
// operacion (charset, idioma, printer-uri, usuario, nombre del trabajo, formato PDF) y fin de
// atributos (0x03). Los bytes del documento van justo detras. Devuelve su longitud, 0 si no cabe.
size_t buildIppPrintJob(uint8_t *b, size_t cap, const char *printerUri, const char *jobName) {
  if (cap < 16) return 0;
  size_t n = 0;
  b[n++] = 0x01; b[n++] = 0x01;                        // version 1.1
  b[n++] = 0x00; b[n++] = 0x02;                        // Print-Job
  b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x00; b[n++] = 0x01; // request-id
  b[n++] = 0x01;                                       // operation-attributes-tag
  if (!ippPutAttr(b, &n, cap, 0x47, "attributes-charset", "utf-8")) return 0;
  if (!ippPutAttr(b, &n, cap, 0x48, "attributes-natural-language", "en")) return 0;
  if (!ippPutAttr(b, &n, cap, 0x45, "printer-uri", printerUri)) return 0;
  if (!ippPutAttr(b, &n, cap, 0x42, "requesting-user-name", IPP_USER_NAME)) return 0;
  if (!ippPutAttr(b, &n, cap, 0x42, "job-name", jobName)) return 0;
  if (!ippPutAttr(b, &n, cap, 0x49, "document-format", "application/pdf")) return 0;
  if (n + 1 > cap) return 0;
  b[n++] = 0x03;                                       // end-of-attributes-tag
  return n;
}

// Envia todos los bytes (write() puede aceptar menos de los pedidos). false si el socket se
// cierra, pasan IPP_IO_TIMEOUT_MS sin avanzar, o la impresora empieza a CONTESTAR antes de
// haber recibido todo (se esta negando a aceptar el trabajo: connected() seguiria siendo true
// mientras quede esa respuesta sin leer, y se esperaria en balde; ippPrintPdf() la lee despues).
bool ippSendAll(WiFiClient *c, const uint8_t *p, size_t n) {
  size_t off = 0;
  uint32_t last = millis();
  while (off < n) {
    size_t w = c->write(p + off, n - off);
    if (w > 0) { off += w; last = millis(); continue; }
    if (c->available() > 0 || !c->connected() || (uint32_t)(millis() - last) > IPP_IO_TIMEOUT_MS) return false;
    delay(1);
  }
  return true;
}

// Lee un byte de la respuesta; -1 si se cierra la conexion sin datos o vence deadline.
int ippReadByte(WiFiClient *c, uint32_t deadline) {
  while (true) {
    if (c->available()) return c->read();
    if (!c->connected() || (int32_t)(millis() - deadline) >= 0) return -1;
    delay(1);
  }
}
// Igual, con un presupuesto de bytes para no leer sin fin una respuesta rara.
int ippGet(WiFiClient *c, uint32_t deadline, uint16_t *budget) {
  if (*budget == 0) return -1;
  (*budget)--;
  return ippReadByte(c, deadline);
}

// Lee una linea HTTP (sin CR/LF) en buf. false si falla. Lo que sobre de la linea se descarta.
bool ippReadLine(WiFiClient *c, uint32_t deadline, char *buf, size_t cap) {
  size_t n = 0;
  while (true) {
    int ch = ippReadByte(c, deadline);
    if (ch < 0) return false;
    if (ch == '\n') break;
    if (ch != '\r' && n + 1 < cap) buf[n++] = (char)ch;
  }
  buf[n] = 0;
  return true;
}

// Lee la respuesta: linea de estado HTTP, cabeceras y, si es 200, el estado IPP (8 primeros bytes
// del cuerpo) y, como mejora opcional, status-message y job-id. Devuelve true si llego el estado
// IPP (ippStatus valido); si no, httpStatus dice hasta donde se llego (0 = sin respuesta).
bool ippReadResponse(WiFiClient *c, uint32_t timeoutMs, int *httpStatus, int *ippStatus,
                     char *statusMsg, size_t msgCap, uint32_t *jobId) {
  uint32_t deadline = millis() + timeoutMs;
  char line[160];
  *httpStatus = 0; *ippStatus = -1; statusMsg[0] = 0; *jobId = 0;
  bool chunked = false;
  while (true) { // salta los "100 Continue" intermedios
    if (!ippReadLine(c, deadline, line, sizeof(line))) return false;
    const char *sp = strchr(line, ' ');
    if (strncmp(line, "HTTP/", 5) != 0 || !sp) return false;
    *httpStatus = atoi(sp + 1);
    chunked = false;
    while (true) { // cabeceras
      if (!ippReadLine(c, deadline, line, sizeof(line))) return false;
      if (line[0] == 0) break;
      for (char *q = line; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
      if (!strncmp(line, "transfer-encoding:", 18) && strstr(line, "chunked")) chunked = true;
    }
    if (*httpStatus != 100) break;
  }
  if (*httpStatus != 200) return false;
  if (chunked && !ippReadLine(c, deadline, line, sizeof(line))) return false; // tamano del primer trozo (hex)
  uint16_t budget = 2048;
  uint8_t hd[8];
  for (int i = 0; i < 8; i++) {
    int ch = ippGet(c, deadline, &budget);
    if (ch < 0) return false;
    hd[i] = (uint8_t)ch;
  }
  *ippStatus = (hd[2] << 8) | hd[3];
  // Mejora opcional (si algo falla, el estado ya esta): atributos de la respuesta
  while (true) {
    int t = ippGet(c, deadline, &budget);
    if (t < 0 || t == 0x03) break;
    if (t <= 0x0F) continue; // delimitador de grupo
    int a = ippGet(c, deadline, &budget), b2 = ippGet(c, deadline, &budget);
    if (a < 0 || b2 < 0) break;
    int nl = (a << 8) | b2;
    char name[24]; int nk = 0;
    for (int i = 0; i < nl; i++) { int ch = ippGet(c, deadline, &budget); if (ch < 0) return true; if (nk < (int)sizeof(name) - 1) name[nk++] = (char)ch; }
    name[nk] = 0;
    a = ippGet(c, deadline, &budget); b2 = ippGet(c, deadline, &budget);
    if (a < 0 || b2 < 0) break;
    int vl = (a << 8) | b2;
    uint8_t val[96]; int vk = 0;
    for (int i = 0; i < vl; i++) { int ch = ippGet(c, deadline, &budget); if (ch < 0) return true; if (vk < (int)sizeof(val)) val[vk++] = (uint8_t)ch; }
    if (nl == 0) continue; // valor adicional del atributo anterior
    if (t == 0x41 && !strcmp(name, "status-message")) {
      size_t m = (size_t)vk < msgCap - 1 ? (size_t)vk : msgCap - 1;
      memcpy(statusMsg, val, m); statusMsg[m] = 0;
    } else if (t == 0x21 && !strcmp(name, "job-id") && vl == 4 && vk == 4) {
      *jobId = ((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) | ((uint32_t)val[2] << 8) | val[3];
    }
  }
  return true;
}

// Nombre del codigo de estado IPP (solo los habituales)
const char *ippStatusName(int st) {
  switch (st) {
    case 0x0000: return "successful-ok";
    case 0x0001: return "successful-ok-ignored-or-substituted-attributes";
    case 0x0002: return "successful-ok-conflicting-attributes";
    case 0x0400: return "client-error-bad-request";
    case 0x0401: return "client-error-forbidden";
    case 0x0402: return "client-error-not-authenticated";
    case 0x0403: return "client-error-not-authorized";
    case 0x0404: return "client-error-not-possible";
    case 0x0405: return "client-error-timeout";
    case 0x0406: return "client-error-not-found";
    case 0x0408: return "client-error-request-entity-too-large";
    case 0x040A: return "client-error-document-format-not-supported";
    case 0x040B: return "client-error-attributes-or-values-not-supported";
    case 0x0500: return "server-error-internal-error";
    case 0x0501: return "server-error-operation-not-supported";
    case 0x0502: return "server-error-service-unavailable";
    case 0x0504: return "server-error-device-error";
    case 0x0505: return "server-error-temporary-error";
    case 0x0506: return "server-error-not-accepting-jobs";
    case 0x0507: return "server-error-busy";
    case 0x0508: return "server-error-job-canceled";
    default:     return "";
  }
}

// Envia el PDF pdfPath a la impresora ippUrl. true si la impresora acepto el trabajo (estado IPP
// de la clase "successful", < 0x0100). msg recibe un texto para el usuario (exito o error).
bool ippPrintPdf(const char *pdfPath, const char *jobName, char *msg, size_t msgCap) {
  msg[0] = 0;
  if (!ippUrl[0]) { snprintf(msg, msgCap, "No hay URL de impresora IPP configurada (usa Configurar impresora)."); return false; }
  bool tls; char host[80]; uint16_t port; char path[128];
  if (!parseIppUrl(ippUrl, &tls, host, sizeof(host), &port, path, sizeof(path))) {
    snprintf(msg, msgCap, "URL de impresora IPP no valida: %s", ippUrl);
    return false;
  }
  char uri[320];
  snprintf(uri, sizeof(uri), "%s://%s:%u%s", tls ? "ipps" : "ipp", host, (unsigned)port, path);
  size_t headLen = buildIppPrintJob(ippHead, sizeof(ippHead), uri, jobName);
  if (!headLen) { snprintf(msg, msgCap, "La peticion IPP no cabe en el buffer (URL demasiado larga)."); return false; }

  sdAccessBegin();
  File f = SD.open(pdfPath, FILE_READ);
  uint32_t size = f ? (uint32_t)f.size() : 0;
  sdAccessEnd();
  if (!f || size == 0) {
    if (f) { sdAccessBegin(); f.close(); sdAccessEnd(); }
    snprintf(msg, msgCap, "No se pudo abrir el PDF %s.", pdfPath);
    return false;
  }

  WiFiClient plain;
  WiFiClientSecure secure;
  WiFiClient *c = &plain;
  if (tls) { secure.setInsecure(); c = &secure; } // sin validar certificado (autofirmados)
  Serial.printf("[IPP] Enviando %s (%lu bytes) a %s\n", pdfPath, (unsigned long)size, uri);
  if (!c->connect(host, port, IPP_CONNECT_TIMEOUT_MS)) {
    sdAccessBegin(); f.close(); sdAccessEnd();
    snprintf(msg, msgCap, "No se pudo conectar con la impresora (%s:%u).", host, (unsigned)port);
    return false;
  }
  c->setNoDelay(true);

  char hdr[512];
  int hl = snprintf(hdr, sizeof(hdr),
                    "POST %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: ESP32-Epson-Emulator\r\n"
                    "Content-Type: application/ipp\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
                    path, host, (unsigned)port, (unsigned long)(headLen + size));
  bool netOk = (hl > 0 && hl < (int)sizeof(hdr)) && ippSendAll(c, (const uint8_t *)hdr, (size_t)hl) &&
               ippSendAll(c, ippHead, headLen);
  bool sdFail = false;
  uint32_t left = size;
  while (netOk && left > 0) {
    size_t want = (left < sizeof(ippChunk)) ? left : sizeof(ippChunk);
    sdAccessBegin();
    size_t n = f.read(ippChunk, want);
    sdAccessEnd();
    if (n == 0) { sdFail = true; break; }
    netOk = ippSendAll(c, ippChunk, n);
    left -= (uint32_t)n;
    yield();
  }
  sdAccessBegin(); f.close(); sdAccessEnd();
  if (sdFail) {
    c->stop();
    snprintf(msg, msgCap, "Error leyendo el PDF de la SD; envio cancelado.");
    return false;
  }

  // Si el envio se corto, la impresora a veces ha contestado antes de cerrar (p. ej. rechazando
  // el trabajo): se intenta leer el motivo con poca espera.
  int httpSt, ippSt; char smsg[96]; uint32_t jobId;
  bool got = ippReadResponse(c, netOk ? IPP_RESPONSE_TIMEOUT_MS : 3000, &httpSt, &ippSt, smsg, sizeof(smsg), &jobId);
  c->stop();

  bool ok = false;
  if (got && ippSt >= 0 && ippSt < 0x0100) {
    ok = true;
    int m = snprintf(msg, msgCap, "Trabajo enviado a la impresora (IPP 0x%04X %s", ippSt, ippStatusName(ippSt));
    if (jobId && m > 0 && (size_t)m < msgCap) m += snprintf(msg + m, msgCap - (size_t)m, ", trabajo %lu", (unsigned long)jobId);
    if (m > 0 && (size_t)m < msgCap) snprintf(msg + m, msgCap - (size_t)m, ").");
  } else if (got) {
    snprintf(msg, msgCap, "La impresora rechazo el trabajo: IPP 0x%04X %s%s%s", ippSt, ippStatusName(ippSt),
             smsg[0] ? " - " : "", smsg);
  } else if (httpSt == 426) {
    snprintf(msg, msgCap, "La impresora exige conexion segura (HTTP 426): usa una URL ipps://.");
  } else if (httpSt == 401 || httpSt == 403) {
    snprintf(msg, msgCap, "La impresora pide autenticacion o no permite imprimir (HTTP %d).", httpSt);
  } else if (httpSt != 0) {
    snprintf(msg, msgCap, "Respuesta HTTP %d de la impresora (compruebe la ruta de la URL).", httpSt);
  } else if (!netOk) {
    snprintf(msg, msgCap, "Se corto la conexion con la impresora al enviar el PDF.");
  } else {
    snprintf(msg, msgCap, "La impresora no contesto (o la respuesta no es valida).");
  }
  Serial.printf("[IPP] %s %s\n", ok ? "OK:" : "ERROR:", msg);
  return ok;
}
// ===== IPP CLIENT END =====

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

// --- /print (solo POST): envia el PDF de una hoja PAGEnnnn a la impresora IPP de ippUrl ---
// Responde con una pagina con el resultado (no redirige: asi se ve el motivo si falla).
void handlePrint() {
  String f = webServer.arg("f");
  if (!isPageFileName(f)) {
    webServer.send(400, "text/plain", "Nombre de fichero no valido.");
    return;
  }
  String bmpPath = "/" + f;
  String pdfName = f.substring(0, 8) + ".PDF";
  String pdfPath = "/" + pdfName;
  bool printing, hasPdf;
  sdAccessBegin();
  printing = pageOpen && pageFileCreated && bmpPath == String(currentFileName);
  hasPdf = SD.exists(pdfPath.c_str());
  sdAccessEnd();
  if (printing) {
    webServer.send(409, "text/plain", "Esa pagina se esta imprimiendo ahora mismo: espere a que se cierre.");
    return;
  }
  if (!hasPdf) {
    webServer.send(404, "text/plain", "No existe " + pdfName + " en la SD.");
    return;
  }
  char msg[256];
  bool ok = ippPrintPdf(pdfPath.c_str(), pdfName.c_str(), msg, sizeof(msg));
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Imprimir</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>body{font-family:sans-serif}</style></head><body><h2>Imprimir " + htmlEscape(pdfName.c_str()) + "</h2><p>" +
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
volatile bool configPortalRequested = false;  // /config pide abrir el portal; lo abre loopWifiOtaWeb() en esta misma tarea

// Quita espacios de los extremos y los caracteres que romperian el HTML del formulario cuando
// el valor se vuelve a escribir en value='...' (comillas y < >).
static void sanitizeUrl(char *url) {
  char *src = url, *dst = url;
  while (*src == ' ' || *src == '\t') src++;
  for (; *src; src++) { if (*src != '\'' && *src != '"' && *src != '<' && *src != '>') *dst++ = *src; }
  while (dst > url && (dst[-1] == ' ' || dst[-1] == '\t' || dst[-1] == '\r' || dst[-1] == '\n')) dst--;
  *dst = 0;
}

// Callback de WiFiManager: se llama al pulsar Save, DESPUES de copiar los campos a sus parametros.
static void onSavePrinterParams() {
  if (!gPortalWm || !pBaud || !pIpp) return;
  bool serialChanged = false;

  // Baudios: solo si el valor es un numero valido; si no, se mantiene el actual.
  char *endp = nullptr;
  uint32_t b = strtoul(pBaud->getValue(), &endp, 10);
  if (pBaud->getValue()[0] != 0 && *endp == 0 && validBaud(b)) {
    if (b != serialBaud) serialChanged = true;
    serialBaud = b;
  } else {
    Serial.printf("[CONFIG] Baudios no validos ('%s'; se admite 300-1000000): se mantiene %lu\n",
                  pBaud->getValue(), (unsigned long)serialBaud);
  }

  // Casillas de verificacion: marcada = el navegador envia el campo; desmarcada = no llega.
  bool hw = gPortalWm->server->hasArg("cfg_hwflow");
  if (hw != useHwFlowControl) serialChanged = true;
  useHwFlowControl = hw;
  useXonXoff = gPortalWm->server->hasArg("cfg_xonxoff");
  autoLfOnCr = gPortalWm->server->hasArg("cfg_autolf");
  autoCrOnLf = gPortalWm->server->hasArg("cfg_autocr");
  autoIPPPrint = gPortalWm->server->hasArg("cfg_autoprint");

  // URL de la impresora IPP (para uso futuro): solo se guarda.
  strncpy(ippUrl, pIpp->getValue(), IPP_URL_MAX_LEN);
  ippUrl[IPP_URL_MAX_LEN] = 0;
  sanitizeUrl(ippUrl);
  if (ippUrl[0] && strncmp(ippUrl, "ipp://", 6) && strncmp(ippUrl, "ipps://", 7) &&
      strncmp(ippUrl, "http://", 7) && strncmp(ippUrl, "https://", 8)) {
    Serial.println("[CONFIG] AVISO: la URL de la impresora IPP no empieza por ipp://, ipps://, http:// ni https:// (se guarda igualmente).");
  }

  if (saveConfig()) printConfig("Configuracion guardada desde la pagina de configuracion del WiFi:");
  if (serialChanged) {
    serialConfigDirty = true; // loop() (el nucleo que usa Serial2) re-aplica baudios y control de flujo
    Serial.println("[CONFIG] Baudios / control de flujo cambiados: se re-aplican ahora al puerto serie.");
  }
}

// Anade al portal los campos de la impresora, con los valores ACTUALES como valores por defecto.
static void addPrinterParams(WiFiManager &wm) {
  snprintf(cfgBaudStr, sizeof(cfgBaudStr), "%lu", (unsigned long)serialBaud);
  snprintf(cfgHtmlBuf, sizeof(cfgHtmlBuf),
           "<br/><label><input type='checkbox' name='cfg_hwflow' value='1'%s> Control de flujo por hardware (RTS/CTS)</label>"
           "<br/><label><input type='checkbox' name='cfg_xonxoff' value='1'%s> Control de flujo por software (XON/XOFF)</label>"
           "<br/><label><input type='checkbox' name='cfg_autolf' value='1'%s> Auto LF: un CR tambien hace salto de linea</label>"
           "<br/><label><input type='checkbox' name='cfg_autocr' value='1'%s> Auto CR: un LF tambien vuelve al margen izquierdo</label>"
           "<br/><label><input type='checkbox' name='cfg_autoprint' value='1'%s> Imprimir automaticamente al completar la hoja en la Impresora IPP preconfigurada</label>"
           "<br/>",
           useHwFlowControl ? " checked" : "", useXonXoff ? " checked" : "",
           autoLfOnCr ? " checked" : "", autoCrOnLf ? " checked" : "", autoIPPPrint ? " checked" : "");

  static WiFiManagerParameter paramHead("<br/><b>Ajustes de la impresora</b> ");
  static WiFiManagerParameter paramBaud("cfg_baud", "Baudios del puerto serie (300-1000000)", cfgBaudStr, 7,
                                        "type='number' min='300' max='1000000'");
  static WiFiManagerParameter paramFlags(cfgHtmlBuf); // HTML libre: lee cfgHtmlBuf cada vez que se dibuja la pagina
  static WiFiManagerParameter paramIpp("cfg_ippurl", "URL de la impresora IPP", ippUrl, IPP_URL_MAX_LEN,
                                       "placeholder='ipp://192.168.1.50/ipp/print'");
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

// --- /config: abre a peticion la pagina de configuracion del WiFi (con los ajustes de la impresora) ---
// El portal se monta en la tarea WiFi (nucleo 0), no en el manejador HTTP: lo abre loopWifiOtaWeb().
void handleConfigPortal() {
  webServer.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Configurar</title></head><body>"
    "<h2>Abriendo la pagina de configuracion</h2>"
    "<p>En unos segundos esta pagina dejara de responder. Conecta el movil u ordenador a la red WiFi "
    "<b>" WIFI_CONFIG_AP_NAME "</b> y abre <a href=\"http://192.168.4.1\">http://192.168.4.1</a> (se abre sola en la mayoria de los "
    "casos) &rarr; <i>Configure WiFi</i>.</p>"
    "<p>Para cambiar <b>solo los ajustes de la impresora</b>, deja el SSID en blanco y pulsa <i>Save</i>: "
    "la red WiFi no se toca. El portal se cierra solo a los 3 minutos o con <i>Exit</i>.</p>"
    "</body></html>");
  configPortalRequested = true;
}

static void runOnDemandConfigPortal() {
  Serial.println("[WIFI] Abriendo a peticion la pagina de configuracion (AP \"" WIFI_CONFIG_AP_NAME "\")...");
  delay(600);          // dejar que el navegador reciba la respuesta de /config antes de cerrar el servidor
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
  webServer.on("/print", HTTP_POST, handlePrint); // solo POST: envia el PDF a la impresora IPP
  webServer.on("/delete", HTTP_POST, handleDelete); // solo POST: un GET a /delete cae en onNotFound (404), nunca borra
  webServer.on("/config", handleConfigPortal);
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
