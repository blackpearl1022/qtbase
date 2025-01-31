// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qsettings.h"
#ifndef QT_NO_SETTINGS

#include "qsettings_p.h"
#ifndef QT_NO_QOBJECT
#include "qcoreapplication.h"
#include <QFile>
#endif // QT_NO_QOBJECT
#include <QDebug>

#include <QFileInfo>
#include <QDir>
#include <QList>
#include <QSet>

#include <emscripten.h>
#include <emscripten/val.h>

QT_BEGIN_NAMESPACE

using emscripten::val;
using namespace Qt::StringLiterals;

namespace {
QStringView keyNameFromPrefixedStorageName(QStringView prefix, QStringView prefixedStorageName)
{
    // Return the key slice after m_keyPrefix, or an empty string view if no match
    if (!prefixedStorageName.startsWith(prefix))
        return QStringView();
    return prefixedStorageName.sliced(prefix.length());
}
} // namespace

//
// Native settings implementation for WebAssembly using window.localStorage
// as the storage backend. localStorage is a key-value store with a synchronous
// API and a 5MB storage limit.
//
class QWasmLocalStorageSettingsPrivate final : public QSettingsPrivate
{
public:
    QWasmLocalStorageSettingsPrivate(QSettings::Scope scope, const QString &organization,
                                     const QString &application);
    ~QWasmLocalStorageSettingsPrivate() final = default;

    void remove(const QString &key) final;
    void set(const QString &key, const QVariant &value) final;
    std::optional<QVariant> get(const QString &key) const final;
    QStringList children(const QString &prefix, ChildSpec spec) const final;
    void clear() final;
    void sync() final;
    void flush() final;
    bool isWritable() const final;
    QString fileName() const final;

private:
    val m_localStorage = val::global("window")["localStorage"];
    QStringList m_keyPrefixes;
};

QWasmLocalStorageSettingsPrivate::QWasmLocalStorageSettingsPrivate(QSettings::Scope scope,
                                                                   const QString &organization,
                                                                   const QString &application)
    : QSettingsPrivate(QSettings::NativeFormat, scope, organization, application)
{
    if (organization.isEmpty()) {
        setStatus(QSettings::AccessError);
        return;
    }

    // The key prefix contians "qt" to separate Qt keys from other keys on localStorage, a
    // version tag to allow for making changes to the key format in the future, the org
    // and app names.
    //
    // User code could could create separate settings object with different org and app names,
    // and would expect them to have separate settings. Also, different webassembly instances
    // on the page could write to the same window.localStorage. Add the org and app name
    // to the key prefix to differentiate, even if that leads to keys with redundant sections
    // for the common case of a single org and app name.
    //
    // Also, the common Qt mechanism for user/system scope and all-application settings are
    // implemented, using different prefixes.
    const QString allAppsSetting = QStringLiteral("all-apps");
    const QString systemSetting = QStringLiteral("sys-tem");

    const QLatin1String separator("-");
    const QLatin1String doubleSeparator("--");
    const QString escapedOrganization = QString(organization).replace(separator, doubleSeparator);
    const QString escapedApplication = QString(application).replace(separator, doubleSeparator);
    const QString prefix = "qt-v0-" + escapedOrganization + separator;
    if (scope == QSettings::Scope::UserScope) {
        if (!escapedApplication.isEmpty())
            m_keyPrefixes.push_back(prefix + escapedApplication + separator);
        m_keyPrefixes.push_back(prefix + allAppsSetting + separator);
    }
    if (!escapedApplication.isEmpty()) {
        m_keyPrefixes.push_back(prefix + escapedApplication + separator + systemSetting
                                + separator);
    }
    m_keyPrefixes.push_back(prefix + allAppsSetting + separator + systemSetting + separator);
}

void QWasmLocalStorageSettingsPrivate::remove(const QString &key)
{
    const std::string removed = QString(m_keyPrefixes.first() + key).toStdString();

    std::vector<std::string> children = { removed };
    const int length = m_localStorage["length"].as<int>();
    for (int i = 0; i < length; ++i) {
        const QString storedKeyWithPrefix =
                QString::fromStdString(m_localStorage.call<val>("key", i).as<std::string>());

        const QStringView storedKey = keyNameFromPrefixedStorageName(
                m_keyPrefixes.first(), QStringView(storedKeyWithPrefix));
        if (storedKey.isEmpty() || !storedKey.startsWith(key))
            continue;

        children.push_back(storedKeyWithPrefix.toStdString());
    }

    for (const auto &child : children)
        m_localStorage.call<val>("removeItem", child);
}

void QWasmLocalStorageSettingsPrivate::set(const QString &key, const QVariant &value)
{
    const std::string keyString = QString(m_keyPrefixes.first() + key).toStdString();
    const std::string valueString = QSettingsPrivate::variantToString(value).toStdString();
    m_localStorage.call<void>("setItem", keyString, valueString);
}

std::optional<QVariant> QWasmLocalStorageSettingsPrivate::get(const QString &key) const
{
    for (const auto &prefix : m_keyPrefixes) {
        const std::string keyString = QString(prefix + key).toStdString();
        const emscripten::val value = m_localStorage.call<val>("getItem", keyString);
        if (!value.isNull())
            return QSettingsPrivate::stringToVariant(
                    QString::fromStdString(value.as<std::string>()));
        if (!fallbacks)
            return std::nullopt;
    }
    return std::nullopt;
}

QStringList QWasmLocalStorageSettingsPrivate::children(const QString &prefix, ChildSpec spec) const
{
    QSet<QString> nodes;
    // Loop through all keys on window.localStorage, return Qt keys belonging to
    // this application, with the correct prefix, and according to ChildSpec.
    QStringList children;
    const int length = m_localStorage["length"].as<int>();
    for (int i = 0; i < length; ++i) {
        for (const auto &storagePrefix : m_keyPrefixes) {
            const QString keyString =
                    QString::fromStdString(m_localStorage.call<val>("key", i).as<std::string>());

            const QStringView key =
                    keyNameFromPrefixedStorageName(storagePrefix, QStringView(keyString));
            if (!key.isEmpty() && key.startsWith(prefix)) {
                QStringList children;
                QSettingsPrivate::processChild(key.sliced(prefix.length()), spec, children);
                if (!children.isEmpty())
                    nodes.insert(children.first());
            }
            if (!fallbacks)
                break;
        }
    }

    return QStringList(nodes.begin(), nodes.end());
}

void QWasmLocalStorageSettingsPrivate::clear()
{
    // Get all Qt keys from window.localStorage
    const int length = m_localStorage["length"].as<int>();
    QStringList keys;
    keys.reserve(length);
    for (int i = 0; i < length; ++i)
        keys.append(QString::fromStdString((m_localStorage.call<val>("key", i).as<std::string>())));

    // Remove all Qt keys. Note that localStorage does not guarantee a stable
    // iteration order when the storage is mutated, which is why removal is done
    // in a second step after getting all keys.
    for (const QString &key : keys) {
        if (!keyNameFromPrefixedStorageName(m_keyPrefixes.first(), key).isEmpty())
            m_localStorage.call<val>("removeItem", key.toStdString());
    }
}

void QWasmLocalStorageSettingsPrivate::sync() { }

void QWasmLocalStorageSettingsPrivate::flush() { }

bool QWasmLocalStorageSettingsPrivate::isWritable() const
{
    return true;
}

QString QWasmLocalStorageSettingsPrivate::fileName() const
{
    return QString();
}

//
// Native settings implementation for WebAssembly using the indexed database as
// the storage backend
//
class QWasmIDBSettingsPrivate : public QConfFileSettingsPrivate
{
public:
    QWasmIDBSettingsPrivate(QSettings::Scope scope, const QString &organization,
                            const QString &application);
    ~QWasmIDBSettingsPrivate();
    static QWasmIDBSettingsPrivate *get(void *userData);

    std::optional<QVariant> get(const QString &key) const override;
    QStringList children(const QString &prefix, ChildSpec spec) const override;
    void clear() override;
    void sync() override;
    void flush() override;
    bool isWritable() const override;

    void syncToLocal(const char *data, int size);
    void loadLocal(const QByteArray &filename);
    void setReady();
    void initAccess() override;

private:
    QString databaseName;
    QString id;
    static QList<QWasmIDBSettingsPrivate *> liveSettings;
};

QList<QWasmIDBSettingsPrivate *> QWasmIDBSettingsPrivate::liveSettings;
static bool isReadReady = false;

static void QWasmIDBSettingsPrivate_onLoad(void *userData, void *dataPtr, int size)
{
    QWasmIDBSettingsPrivate *settings = QWasmIDBSettingsPrivate::get(userData);
    if (!settings)
        return;

    QFile file(settings->fileName());
    QFileInfo fileInfo(settings->fileName());
    QDir dir(fileInfo.path());
    if (!dir.exists())
        dir.mkpath(fileInfo.path());

    if (file.open(QFile::WriteOnly)) {
        file.write(reinterpret_cast<char *>(dataPtr), size);
        file.close();
        settings->setReady();
    }
}

static void QWasmIDBSettingsPrivate_onError(void *userData)
{
    if (QWasmIDBSettingsPrivate *settings = QWasmIDBSettingsPrivate::get(userData))
        settings->setStatus(QSettings::AccessError);
}

static void QWasmIDBSettingsPrivate_onStore(void *userData)
{
    if (QWasmIDBSettingsPrivate *settings = QWasmIDBSettingsPrivate::get(userData))
        settings->setStatus(QSettings::NoError);
}

static void QWasmIDBSettingsPrivate_onCheck(void *userData, int exists)
{
    if (QWasmIDBSettingsPrivate *settings = QWasmIDBSettingsPrivate::get(userData)) {
        if (exists)
            settings->loadLocal(settings->fileName().toLocal8Bit());
        else
            settings->setReady();
    }
}

QWasmIDBSettingsPrivate::QWasmIDBSettingsPrivate(QSettings::Scope scope,
                                                 const QString &organization,
                                                 const QString &application)
    : QConfFileSettingsPrivate(QSettings::NativeFormat, scope, organization, application)
{
    liveSettings.push_back(this);

    setStatus(QSettings::AccessError); // access error until sandbox gets loaded
    databaseName = organization;
    id = application;

    emscripten_idb_async_exists("/home/web_user",
                                fileName().toLocal8Bit(),
                                reinterpret_cast<void*>(this),
                                QWasmIDBSettingsPrivate_onCheck,
                                QWasmIDBSettingsPrivate_onError);
}

QWasmIDBSettingsPrivate::~QWasmIDBSettingsPrivate()
{
    liveSettings.removeAll(this);
}

QWasmIDBSettingsPrivate *QWasmIDBSettingsPrivate::get(void *userData)
{
    if (QWasmIDBSettingsPrivate::liveSettings.contains(userData))
        return reinterpret_cast<QWasmIDBSettingsPrivate *>(userData);
    return nullptr;
}

void QWasmIDBSettingsPrivate::initAccess()
{
     if (isReadReady)
         QConfFileSettingsPrivate::initAccess();
}

std::optional<QVariant> QWasmIDBSettingsPrivate::get(const QString &key) const
{
    if (isReadReady)
        return QConfFileSettingsPrivate::get(key);

    return std::nullopt;
}

QStringList QWasmIDBSettingsPrivate::children(const QString &prefix, ChildSpec spec) const
{
    return QConfFileSettingsPrivate::children(prefix, spec);
}

void QWasmIDBSettingsPrivate::clear()
{
    QConfFileSettingsPrivate::clear();
    emscripten_idb_async_delete("/home/web_user",
                                fileName().toLocal8Bit(),
                                reinterpret_cast<void*>(this),
                                QWasmIDBSettingsPrivate_onStore,
                                QWasmIDBSettingsPrivate_onError);
}

void QWasmIDBSettingsPrivate::sync()
{
    QConfFileSettingsPrivate::sync();

    QFile file(fileName());
    if (file.open(QFile::ReadOnly)) {
        QByteArray dataPointer = file.readAll();

        emscripten_idb_async_store("/home/web_user",
                                  fileName().toLocal8Bit(),
                                   reinterpret_cast<void *>(dataPointer.data()),
                                   dataPointer.length(),
                                   reinterpret_cast<void*>(this),
                                   QWasmIDBSettingsPrivate_onStore,
                                   QWasmIDBSettingsPrivate_onError);
    }
}

void QWasmIDBSettingsPrivate::flush()
{
    sync();
}

bool QWasmIDBSettingsPrivate::isWritable() const
{
    return isReadReady && QConfFileSettingsPrivate::isWritable();
}

void QWasmIDBSettingsPrivate::syncToLocal(const char *data, int size)
{
    QFile file(fileName());

    if (file.open(QFile::WriteOnly)) {
        file.write(data, size + 1);
        QByteArray data = file.readAll();

        emscripten_idb_async_store("/home/web_user",
                                   fileName().toLocal8Bit(),
                                   reinterpret_cast<void *>(data.data()),
                                   data.length(),
                                   reinterpret_cast<void*>(this),
                                   QWasmIDBSettingsPrivate_onStore,
                                   QWasmIDBSettingsPrivate_onError);
        setReady();
    }
}

void QWasmIDBSettingsPrivate::loadLocal(const QByteArray &filename)
{
    emscripten_idb_async_load("/home/web_user",
                              filename.data(),
                              reinterpret_cast<void*>(this),
                              QWasmIDBSettingsPrivate_onLoad,
                              QWasmIDBSettingsPrivate_onError);
}

void QWasmIDBSettingsPrivate::setReady()
{
    isReadReady = true;
    setStatus(QSettings::NoError);
    QConfFileSettingsPrivate::initAccess();
}

QSettingsPrivate *QSettingsPrivate::create(QSettings::Format format, QSettings::Scope scope,
                                           const QString &organization, const QString &application)
{
    const auto WebLocalStorageFormat = QSettings::IniFormat + 1;
    const auto WebIdbFormat = QSettings::IniFormat + 2;

    // Make WebLocalStorageFormat the default native format
    if (format == QSettings::NativeFormat)
        format = QSettings::Format(WebLocalStorageFormat);

    // Check if cookies are enabled (required for using persistent storage)
    const bool cookiesEnabled = val::global("navigator")["cookieEnabled"].as<bool>();
    constexpr QLatin1StringView cookiesWarningMessage
        ("QSettings::%1 requires cookies, falling back to IniFormat with temporary file");
    if (format == WebLocalStorageFormat && !cookiesEnabled) {
        qWarning() << cookiesWarningMessage.arg("WebLocalStorageFormat");
        format = QSettings::IniFormat;
    } else if (format == WebIdbFormat && !cookiesEnabled) {
        qWarning() << cookiesWarningMessage.arg("WebIdbFormat");
        format = QSettings::IniFormat;
    }

    if (format == WebLocalStorageFormat)
        return new QWasmLocalStorageSettingsPrivate(scope, organization, application);
    if (format == WebIdbFormat)
        return new QWasmIDBSettingsPrivate(scope, organization, application);

    // Create settings backend according to selected format
    switch (format) {
    case QSettings::Format::IniFormat:
    case QSettings::Format::CustomFormat1:
    case QSettings::Format::CustomFormat2:
    case QSettings::Format::CustomFormat3:
    case QSettings::Format::CustomFormat4:
    case QSettings::Format::CustomFormat5:
    case QSettings::Format::CustomFormat6:
    case QSettings::Format::CustomFormat7:
    case QSettings::Format::CustomFormat8:
    case QSettings::Format::CustomFormat9:
    case QSettings::Format::CustomFormat10:
    case QSettings::Format::CustomFormat11:
    case QSettings::Format::CustomFormat12:
    case QSettings::Format::CustomFormat13:
    case QSettings::Format::CustomFormat14:
    case QSettings::Format::CustomFormat15:
    case QSettings::Format::CustomFormat16:
        return new QConfFileSettingsPrivate(format, scope, organization, application);
    case QSettings::Format::InvalidFormat:
        return nullptr;
    case QSettings::Format::NativeFormat:
        /* NOTREACHED */ assert(0);
        break;
    }
}

QT_END_NAMESPACE
#endif // QT_NO_SETTINGS
