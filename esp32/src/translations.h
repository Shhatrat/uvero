#pragma once
#include <pgmspace.h>

// Add a new language by adding another entry to the T object below.
// Keys must match those already used in main.cpp (data-i18n attributes and JS references).
static const char TRANSLATIONS_JS[] PROGMEM = R"js(const T={
  pl:{
    speed:     'Prędkość',
    dist:      'Dystans',
    temp:      'Temperatura',
    clients:   'klientów',
    ram:       'RAM wolny',
    reset:     'Reset dystansu',
    connecting:'łączenie...',
    updated:   'aktualizacja:',
    error:     'błąd'
  },
  en:{
    speed:     'Speed',
    dist:      'Distance',
    temp:      'Temperature',
    clients:   'clients',
    ram:       'Free RAM',
    reset:     'Reset distance',
    connecting:'connecting...',
    updated:   'updated:',
    error:     'error'
  }
};)js";
