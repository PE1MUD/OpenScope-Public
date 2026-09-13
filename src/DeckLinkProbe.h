#pragma once

#include <QString>
#include <QStringList>
#include <QList>

class VideoEngine;

struct DeckLinkCompositeGainState
{
    bool lumaAvailable = false;
    bool chromaAvailable = false;

    double minimumDb = 0.0;
    double maximumDb = 0.0;
    double lumaDb = 0.0;
    double chromaDb = 0.0;
};

QString deckLinkProbe(VideoEngine* videoEngine, int deviceIndex = -1);
QStringList deckLinkDeviceNames();
QList<bool> deckLinkDeviceBusyStates();
void deckLinkRefreshDeviceList();
int deckLinkActiveDeviceIndex();
void deckLinkSetPreferredDeviceIndex(int deviceIndex);
void deckLinkStop();

DeckLinkCompositeGainState deckLinkCompositeGainState();
bool deckLinkSetCompositeLumaGain(double gainDb);
bool deckLinkSetCompositeChromaGain(double gainDb);
bool deckLinkCommitConfiguration();
