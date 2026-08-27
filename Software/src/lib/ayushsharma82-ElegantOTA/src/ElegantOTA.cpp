#include "ElegantOTA.h"

#include "../../../devboard/safety/safety.h"
#include "../../../devboard/utils/flash_write_broker.h"

ElegantOTAClass::ElegantOTAClass(){}

void ElegantOTAClass::begin(ELEGANTOTA_WEBSERVER *server){
  _server = server;

    _server->on("/update", HTTP_GET, [&](AsyncWebServerRequest *request){
      AsyncWebServerResponse *response = request->beginResponse(200, "text/html", ELEGANT_HTML, sizeof(ELEGANT_HTML));
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
    });

    _server->on("/ota/start", HTTP_GET, [&](AsyncWebServerRequest *request) {

      // Get header x-ota-mode value, if present
      OTA_Mode mode = OTA_MODE_FIRMWARE;
      // Get mode from arg
      if (request->hasParam("mode")) {
        String argValue = request->getParam("mode")->value();
        if (argValue == "fs") {
          mode = OTA_MODE_FILESYSTEM;
        } else {
          mode = OTA_MODE_FIRMWARE;
        }
      }

      // Get file MD5 hash from arg
      if (request->hasParam("hash")) {
        String hash = request->getParam("hash")->value();
        if (!Update.setMD5(hash.c_str())) {
          return request->send(400, "text/plain", "MD5invalid");
        }
      }

      // Pre-OTA update callback
      if (preUpdateCallback != NULL) preUpdateCallback();

      // Start update process
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, mode == OTA_MODE_FILESYSTEM ? U_SPIFFS : U_FLASH)) {
          // Save error to string
          StreamString str;
          Update.printError(str);
          _update_error_str = str.c_str();
          _update_error_str.concat("\n");
        }        

      return request->send((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
    });

    _server->on("/ota/upload", HTTP_POST, [&](AsyncWebServerRequest *request) {
        // Post-OTA update callback
        if (postUpdateCallback != NULL) postUpdateCallback(!Update.hasError());
        AsyncWebServerResponse *response = request->beginResponse((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _update_error_str.c_str() : "OK");
        response->addHeader("Connection", "close");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);
    }, [&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        //Upload handler chunks in data
        if (!index) {
          // Reset progress size on first frame
          _current_progress_size = 0;
        }

        // Write chunked data to the free sketch space.
        // Brokered: UpdateClass buffers to a whole flash sector and
        // flushes it as an erase plus a program, which is by far the longest
        // cache-off window the firmware produces at runtime. A chunk never
        // exceeds a sector, so one chunk is at most one flush - the shortest
        // unit this API can be asked for - and the broker drains the CAN
        // receive FIFOs before it and yields after it.
        if(len){
            size_t written = 0;
            flash_write_broker().run([&]() { written = Update.write(data, len); });
            if (written != len) {
                return request->send(400, "text/plain", "FailWrite");
            }
            _current_progress_size += len;
            // Progress update callback
            if (progressUpdateCallback != NULL) progressUpdateCallback(_current_progress_size, request->contentLength());
        }
            
        if (final) { // if the final flag is set then this is the last frame of data
            // end() flushes the last partial sector and rewrites the OTA data
            // partition, so it is another erase-bearing window and is brokered
            // the same way.
            bool ended = false;
            flash_write_broker().run([&]() { ended = Update.end(true); }); //true to set the size to the current progress
            if (!ended) {
                // Save error to string
                StreamString str;
                Update.printError(str);
                _update_error_str = str.c_str();
                _update_error_str.concat("\n");
            }
        }else{
            return;
        }
    });

}

void ElegantOTAClass::onStart(std::function<void()> callable){
    preUpdateCallback = callable;
}

void ElegantOTAClass::onProgress(std::function<void(size_t current, size_t final)> callable){
    progressUpdateCallback= callable;
}

void ElegantOTAClass::onEnd(std::function<void(bool success)> callable){
    postUpdateCallback = callable;
}


ElegantOTAClass ElegantOTA;
