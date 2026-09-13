#pragma once

#include "OpenScopeSettings.h"

#include <QString>

class SettingsStorage final
{
public:
    SettingsStorage();

    OpenScopeSettings load() const;

    void save(
        const OpenScopeSettings& settings) const;

private:
    QString fileName_;
};