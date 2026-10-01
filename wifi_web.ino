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
 *                      directamente desde la SD mientras se sigue
 *                      escribiendo -- la cabecera del BMP se mantiene
 *                      coherente en todo momento gracias a
 *                      patchHeaderNow() (ver impresora_epson_esp32.ino),
 *                      asi que esto funciona aunque la pagina no se haya
 *                      terminado de imprimir todavia
 *       /list          lista de todos los PAGEnnnn.BMP que hay en la SD
 *       /wifi-reset    borra la red WiFi guardada y reinicia (para volver
 *                      a configurar otra red)
 *
 * LIMITACION A TENER EN CUENTA: el servidor web es SINCRONO (se atiende
 * dentro del mismo loop() que procesa el puerto serie de la impresora), a
 * proposito -- asi nunca hay dos tareas tocando la tarjeta SD al mismo
 * tiempo, que es justo el tipo de problema dificil de depurar que conviene
 * evitar. La contrapartida es que mientras se sirve /current.bmp (puede
 * tardar unos segundos en una pagina grande) el puerto serie no se
 * atiende; para que no se pierdan datos en ese rato, el control de flujo
 * por hardware (RTS/CTS, ya configurado si USE_HW_FLOW_CONTROL es true)
 * hace que el host se quede esperando en vez de seguir mandando bytes.
 * ==========================================================================
 */

#include <WiFi.h>
#include <WiFiManager.h>   // libreria de terceros: "WiFiManager" de tzapu
#include <ArduinoOTA.h>    // incluida en el nucleo ESP32 de Arduino
#include <WebServer.h>     // incluida en el nucleo ESP32 de Arduino

#define OTA_HOSTNAME "impresora-epson"
#define OTA_PASSWORD "cambiame123"   // cambia esto antes de usarlo
#define WIFI_CONFIG_AP_NAME "ImpresoraEpson-Setup"
#define WIFI_CONFIG_PORTAL_TIMEOUT_S 180 // si nadie configura nada en 3 min,
                                          // sigue arrancando sin WiFi en vez
                                          // de quedarse esperando para siempre

WebServer webServer(80);

// --- Pagina principal ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<title>Impresora Epson ESP32</title>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<style>body{font-family:sans-serif;background:#222;color:#eee;text-align:center}"
                "img{max-width:95%;border:1px solid #555;margin-top:10px;background:#fff}"
                "a{color:#8cf}</style></head><body>"
                "<h2>Impresora Epson (ESP32)</h2>";
  if (pageOpen) {
    html += "<p>Imprimiendo ahora: " + String(currentFileName) + "</p>";
  } else if (pageIndex > 0) {
    html += "<p>Sin pagina abierta ahora mismo (se muestra la ultima generada).</p>";
  } else {
    html += "<p>Todavia no se ha generado ninguna pagina.</p>";
  }
  html += "<img id='pg' src='/current.bmp' onerror=\"this.style.display='none'\">"
          "<p><a href='/list'>Ver todas las paginas</a> &middot; "
          "<a href='/wifi-reset'>Reconfigurar WiFi</a></p>"
          "<script>setInterval(function(){"
          "document.getElementById('pg').style.display='';"
          "document.getElementById('pg').src='/current.bmp?' + Date.now();"
          "}, 2000);</script>"
          "</body></html>";
  webServer.send(200, "text/html", html);
}

// --- Sirve un fichero .BMP de la SD como respuesta HTTP ---
void streamBmpFile(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f) {
    webServer.send(404, "text/plain", "No hay ninguna pagina generada todavia.");
    return;
  }
  webServer.sendHeader("Cache-Control", "no-store"); // que el navegador no la guarde en cache
  webServer.streamFile(f, "image/bmp"); // reparte Content-Length + los bytes en trozos
  f.close();
}

// --- /current.bmp: la pagina en curso, o la ultima cerrada si no hay ninguna abierta ---
void handleCurrentBmp() {
  if (pageOpen) {
    streamBmpFile(currentFileName);
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
void handleList() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Paginas</title>"
                 "<style>body{font-family:sans-serif}</style></head><body>"
                 "<h2>Paginas en la SD</h2><ul>";
  File root = SD.open("/");
  if (root) {
    File entry = root.openNextFile();
    while (entry) {
      String name = entry.name();
      if (name.endsWith(".BMP") || name.endsWith(".bmp")) {
        html += "<li><a href='" + name + "'>" + name + "</a> (" +
                String(entry.size()) + " bytes)</li>";
      }
      entry = root.openNextFile();
    }
    root.close();
  }
  html += "</ul><p><a href='/'>Volver</a></p></body></html>";
  webServer.send(200, "text/html", html);
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

void setupWifiOtaWeb() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT_S);
  // autoConnect() intenta primero la ultima red guardada; si no lo consigue,
  // monta el punto de acceso de configuracion con ese nombre y se bloquea
  // aqui hasta que alguien la configura o pasa el timeout de arriba.
  bool connected = wm.autoConnect(WIFI_CONFIG_AP_NAME);
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
  webServer.on("/wifi-reset", handleWifiReset);
  webServer.begin();
  Serial.println("[WEB] Servidor web escuchando en el puerto 80.");
}

void loopWifiOtaWeb() {
  ArduinoOTA.handle();
  webServer.handleClient();
}
