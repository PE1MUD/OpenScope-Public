#include "SettingsService.h"

SettingsService::SettingsService(
    QObject* parent)
    : QObject(parent)
    , settings_(storage_.load())
{}

const OpenScopeSettings&
SettingsService::settings() const noexcept
{
    return settings_;
}

void SettingsService::update(
    const std::function<void(OpenScopeSettings&)>& updater)
{
    updater(settings_);

    storage_.save(settings_);

    emit settingsChanged(settings_);
}