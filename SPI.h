#ifndef SPIFFS_H
#define SPIFFS_H
#include <Arduino.h>

void initSPIFFS();
void createConfigDir();
void saveConstantsToFile();
void loadConstantsFromFile();
void listSPIFFSFiles(const char* dirname);
void handleConstantsPage(AsyncWebServerRequest *request);
String readFile(fs::FS &fs, const char *path);

#endif
