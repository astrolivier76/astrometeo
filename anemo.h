// anemo.h
#ifndef ANEMO_H
#define ANEMO_H

// Initialisation de l’anémomètre (pin + interruption)
void init_vent();

// Calcul et mise à jour de la vitesse du vent
// measurementPeriod = période de mesure en secondes (min 1s)
void getSendVitesseVent(int measurementPeriod);

// Récupération de la dernière vitesse calculée (km/h)
float getVent();

#endif
