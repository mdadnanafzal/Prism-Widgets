// Prism Pet - the desktop pet's public API, used by app.cpp and arrange.cpp.
#pragma once
#include "../common.h"

void PetLoadSettings(const JVal* j);   // the "pet" object in config.json (may be null)
void PetSaveSettings(JVal& j);
void PetInit();                        // after LoadConfig: shows the pet if it is enabled
void PetShutdown();
void PetArrange(bool on);              // Arrange mode: the pet hides while it runs
