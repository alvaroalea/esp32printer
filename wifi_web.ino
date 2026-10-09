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
 *       /list          lista de todos los PAGEnnnn.BMP que hay en la SD, cada
 *                      uno con un boton para borrarlo (POST /delete)
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
#include <WiFiManager.h>   // libreria de terceros: "WiFiManager" de tzapu
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
void sendFileChunked(File &f, uint32_t totalSize) {
  webServer.setContentLength(totalSize);
  webServer.send(200, "image/bmp", "");
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
  sendFileChunked(f, size);
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
                 ".del{font-size:.7em;padding:0 .4em;margin-left:.4em;color:#b00;cursor:pointer}</style></head><body>"
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
          html += " <small>(imprimiendose ahora)</small>"; // la hoja en curso no se puede borrar
        } else if (isPageFileName(bare)) {
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

// --- /delete (solo POST): borra de la SD una hoja PAGEnnnn.BMP y vuelve al listado ---
void handleDelete() {
  String f = webServer.arg("f");
  if (!isPageFileName(f)) {
    webServer.send(400, "text/plain", "Nombre de fichero no valido.");
    return;
  }
  String path = "/" + f;
  int result = 0; // 0 = borrada, 1 = no existe, 2 = se esta imprimiendo, 3 = fallo al borrar
  sdAccessBegin(); // la comprobacion de "hoja en curso" va DENTRO del bloqueo: asi no puede cambiar mientras se borra
  if (pageOpen && pageFileCreated && path == String(currentFileName)) result = 2;
  else if (!SD.exists(path.c_str())) result = 1;
  else if (!SD.remove(path.c_str())) result = 3;
  sdAccessEnd();
  if (result == 2) {
    webServer.send(409, "text/plain", "Esa pagina se esta imprimiendo ahora mismo: no se puede borrar hasta que se cierre.");
    return;
  }
  if (result == 3) {
    webServer.send(500, "text/plain", "No se pudo borrar " + f + " de la SD.");
    return;
  }
  if (result == 0) Serial.printf("[INFO] Pagina borrada desde la web: %s\n", path.c_str());
  // 303 + Location: el navegador recarga /list con un GET, asi que F5 no repite el borrado.
  webServer.sendHeader("Location", "/list");
  webServer.send(303, "text/plain", "");
}

// --- Sirve cualquier .BMP que exista en la SD por su nombre de fichero
// (ej. /PAGE0007.BMP), que es a donde apuntan los enlaces de /list. Se
// registra con onNotFound() porque esos nombres no se conocen de antemano
// (se van creando segun se imprime), asi que no se puede dar de alta una
// ruta fija por cada uno con webServer.on() ---
void handlePossibleBmpFile() {
  String uri = webServer.uri(); // p.ej. "/PAGE0007.BMP"
  if (uri.endsWith(".BMP") || uri.endsWith(".bmp")) {
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

void setupWifiOtaWeb() {
  // Antes de recurrir a WiFiManager (que si falla aqui, abre su propio
  // punto de acceso de configuracion), unos pocos intentos rapidos con la
  // red ya guardada: resuelve solo el rechazo transitorio de asociacion sin
  // molestar con el portal cuando la red es, de hecho, la correcta.
  bool connected = tryQuickWifiReconnect();

  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT_S);
  if (!connected) {
    // autoConnect() intenta de nuevo la ultima red guardada; si tampoco lo
    // consigue, monta el punto de acceso de configuracion con ese nombre y
    // se bloquea aqui hasta que alguien la configura o pasa el timeout de
    // arriba.
    connected = wm.autoConnect(WIFI_CONFIG_AP_NAME);
  }
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
  webServer.on("/delete", HTTP_POST, handleDelete); // solo POST: un GET a /delete cae en onNotFound (404), nunca borra
  webServer.on("/wifi-reset", handleWifiReset);
  webServer.onNotFound(handlePossibleBmpFile); // sirve /PAGEnnnn.BMP (enlaces de /list)
  webServer.begin();
  Serial.println("[WEB] Servidor web escuchando en el puerto 80.");
}

void loopWifiOtaWeb() {
  ArduinoOTA.handle();
  webServer.handleClient();
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
