#pragma once

#include "OpenScopeSettings.h"
#include "SettingsStorage.h"

#include <QObject>

#include <functional>

class SettingsService final : public QObject
{
    Q_OBJECT

public:
    explicit SettingsService(
        QObject* parent = nullptr);

    const OpenScopeSettings& settings() const noexcept;

    void update(
        const std::function<void(OpenScopeSettings&)>& updater);

signals:
    void settingsChanged(
        const OpenScopeSettings& settings);

private:
    SettingsStorage storage_;
    OpenScopeSettings settings_;
};