/* * This file is part of dui-im-framework *
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 * All rights reserved.
 * Contact: Nokia Corporation (directui@nokia.com)
 *
 * If you have questions regarding the use of this file, please contact
 * Nokia at directui@nokia.com.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * and appearing in the file LICENSE.LGPL included in the packaging
 * of this file.
 */

#include "duiimpluginmanager.h"
#include "duiimpluginmanager_p.h"

#include <DuiGConfItem>
#include <DuiKeyboardStateTracker>

#include "duiinputmethodplugin.h"
#include "duiinputcontextdbusconnection.h"

#include <QDir>
#include <QPluginLoader>
#include <QDebug>


namespace
{
    const int DeleteInputMethodTimeout = 60000;

    const QString DefaultPluginLocation("/usr/lib/dui-im-plugins/");

    const QString ConfigRoot          = "/Dui/InputMethods/";
    const QString DuiImPluginPaths    = ConfigRoot + "Paths";
    const QString DuiImPluginActive   = ConfigRoot + "ActivePlugins";
    const QString DuiImPluginDisabled = ConfigRoot + "DisabledPluginFiles";

    const QString PluginRoot          = "/Dui/InputMethods/Plugins/";
    const QString DuiImHandlerToPlugin  = PluginRoot + "Handler";
    const QString DuiImAccesoryEnabled  = "/Dui/InputMethods/AccessoryEnabled";
}



DuiIMPluginManagerPrivate::DuiIMPluginManagerPrivate(DuiInputContextConnection *connection,
                                                     DuiIMPluginManager *p)
    : parent(p),
      duiICConnection(connection),
      handlerToPluginConf(0),
      imAccessoryEnabledConf(0)
{
    deleteImTimer.setSingleShot(true);
    deleteImTimer.setInterval(DeleteInputMethodTimeout);
}


DuiIMPluginManagerPrivate::~DuiIMPluginManagerPrivate()
{
    delete duiICConnection;
}


void DuiIMPluginManagerPrivate::loadPlugins()
{
    foreach (QString path, paths) {
        QDir pluginsDir(path);

        foreach (const QString &fileName, pluginsDir.entryList(QDir::Files)) {
            if (blacklist.contains(fileName)) {
                qWarning() << __PRETTY_FUNCTION__ << fileName << "is on the blacklist, skipped.";
                continue;
            }

            QPluginLoader load(pluginsDir.absoluteFilePath(fileName));
            QObject *pluginInstance = load.instance();

            if (!pluginInstance) {
                qWarning() << __PRETTY_FUNCTION__ << "Error loading plugin from "
                           << path << fileName << load.errorString();
                continue;
            }

            DuiInputMethodPlugin *plugin = qobject_cast<DuiInputMethodPlugin *>(pluginInstance);

            if (plugin) {
                plugins[plugin] = 0;
                if (active.contains(plugin->name())) {
                    activatePlugin(plugin);
                }
            } else
                qWarning() << __PRETTY_FUNCTION__
                           << "Plugin is not DuiInputMethodPlugin: " << fileName;
        } // end foreach file in path
    } // end foreach path in paths
}


bool DuiIMPluginManagerPrivate::activatePlugin(const QString &name)
{
    foreach (DuiInputMethodPlugin *plugin, activePlugins) {
        if (plugin->name() == name) {
            return true;
        }
    }

    foreach (DuiInputMethodPlugin *plugin, plugins.keys()) {
        if (plugin->name() == name) {
            activatePlugin(plugin);
            return true;
        }
    }
    return false;
}

void DuiIMPluginManagerPrivate::activatePlugin(DuiInputMethodPlugin *plugin)
{
    if (!plugin || activePlugins.contains(plugin)) {
        return;
    }

    DuiInputMethodBase *inputMethod = 0;

    activePlugins.insert(plugin);
    if (!plugins[plugin]) {
        inputMethod = plugin->createInputMethod(duiICConnection);
        bool connected = false;

        plugins[plugin] = inputMethod;
        if (inputMethod) {
            connected = QObject::connect(inputMethod, SIGNAL(regionUpdated(const QRegion &)),
                                         parent, SIGNAL(regionUpdated(const QRegion &)));

            connected = QObject::connect(inputMethod,
                                         SIGNAL(inputMethodAreaUpdated(const QRegion &)),
                                         duiICConnection,
                                         SLOT(updateInputMethodArea(const QRegion &)))
                        && connected;
        }
        if (!connected) {
            qWarning() << __PRETTY_FUNCTION__ << "Plugin" << plugin->name()
                       << "Unable to connect plugin's signals with IC connection's slots";
        }
    } else {
        inputMethod = plugins[plugin];
    }

    duiICConnection->addTarget(inputMethod); // redirect incoming requests

    return;
}


void DuiIMPluginManagerPrivate::addHandlerMap(DuiIMHandlerState state, const QString &pluginName)
{
    foreach (DuiInputMethodPlugin *plugin, plugins.keys()) {
        if (plugin->name() == pluginName) {
            handlerToPlugin[state] = plugin;
            break;
        }
    }
}


void DuiIMPluginManagerPrivate::setActiveHandlers(const QSet<DuiIMHandlerState> &states)
{
    QSet<DuiInputMethodPlugin *> activatedPlugins;
    typedef QMap<DuiInputMethodBase *, QList<DuiIMHandlerState> > NewStates;
    NewStates newStates;
    DuiInputMethodBase *inputMethod = 0;

    //activate new plugins
    foreach (DuiIMHandlerState state, states) {
        HandlerMap::const_iterator iterator = handlerToPlugin.find(state);
        DuiInputMethodPlugin *plugin = 0;

        if (iterator != handlerToPlugin.end()) {
            plugin = iterator.value();
            if (!activePlugins.contains(plugin)) {
                activatePlugin(plugin);
            }
            inputMethod = plugins[plugin];
            if (inputMethod) {
                newStates[inputMethod].append(state);
                activatedPlugins.insert(plugin);
            }
        }
    }

    // notify plugins about new states
    for (NewStates::iterator iterator = newStates.begin();
         iterator != newStates.end();
         ++iterator) {
        iterator.key()->setState(iterator.value());
    }

    // deactivate unnecessary plugins
    QSet<DuiInputMethodPlugin *> previousActive = activePlugins;
    foreach (DuiInputMethodPlugin *plugin, previousActive) {
        if (!activatedPlugins.contains(plugin)) {
            deactivatePlugin(plugin);  //activePlugins is modified here
        }
    }
}


QSet<DuiIMHandlerState> DuiIMPluginManagerPrivate::activeHandlers() const
{
    QSet<DuiIMHandlerState> handlers;
    foreach (DuiInputMethodPlugin *plugin, activePlugins) {
        handlers << handlerToPlugin.key(plugin);
    }
    return handlers;
}


void DuiIMPluginManagerPrivate::deleteInactiveIM()
{
    QMap<DuiInputMethodPlugin *, DuiInputMethodBase *>::iterator iterator;

    for (iterator = plugins.begin(); iterator != plugins.end(); ++iterator) {
        if (!activePlugins.contains(iterator.key())) {
            delete *iterator;
            *iterator = 0;
        }
    }
}


void DuiIMPluginManagerPrivate::deactivatePlugin(DuiInputMethodPlugin *plugin)
{
    if (!activePlugins.contains(plugin))
        return;

    activePlugins.remove(plugin);

    DuiInputMethodBase *inputMethod = plugins[plugin];

    if (!inputMethod)
        return;

    inputMethod->hide();
    inputMethod->reset();
    duiICConnection->removeTarget(inputMethod);
}


void DuiIMPluginManagerPrivate::convertAndFilterHandlers(const QStringList &handlerNames,
                                                         QSet<DuiIMHandlerState> *handlers)
{
    bool ok = false;
    bool disableOnscreenKbd = false;

    foreach (const QString &name, handlerNames) {
        int handlerNumber = (DuiIMHandlerState)name.toInt(&ok);
        if (ok && handlerNumber >= OnScreen && handlerNumber <= Accessory) {
            if (!disableOnscreenKbd) {
                disableOnscreenKbd = handlerNumber != OnScreen;
            }
            handlers->insert((DuiIMHandlerState)handlerNumber);
        }
    }

    if (disableOnscreenKbd) {
        handlers->remove(OnScreen);
    }
}

QStringList DuiIMPluginManagerPrivate::loadedPluginsNames() const
{
    QStringList result;

    foreach (DuiInputMethodPlugin *plugin, plugins.keys()) {
        result.append(plugin->name());
    }

    return result;
}


QStringList DuiIMPluginManagerPrivate::activePluginsNames() const
{
    QStringList result;

    foreach (DuiInputMethodPlugin *plugin, activePlugins) {
        result.append(plugin->name());
    }

    return result;
}


QStringList DuiIMPluginManagerPrivate::activeInputMethodsNames() const
{
    QStringList result;

    for (Plugins::const_iterator iterator = plugins.begin();
            iterator != plugins.end(); ++iterator) {
        if (iterator.value()) {
            result.append(iterator.key()->name());
        }
    }

    return result;
}


///////////////
// actual class

DuiIMPluginManager::DuiIMPluginManager()
    : QObject(),
      d(new DuiIMPluginManagerPrivate(new DuiInputContextDBusConnection, this))
{
    d->paths     = DuiGConfItem(DuiImPluginPaths).value(QStringList(DefaultPluginLocation)).toStringList();
    d->blacklist = DuiGConfItem(DuiImPluginDisabled).value().toStringList();
    d->active    = DuiGConfItem(DuiImPluginActive).value().toStringList();

    d->loadPlugins();

    d->handlerToPluginConf = new DuiGConfItem(DuiImHandlerToPlugin, this);
    connect(d->handlerToPluginConf, SIGNAL(valueChanged()), this, SLOT(reloadHandlerMap()));

    reloadHandlerMap();

    if (DuiKeyboardStateTracker::instance()->isPresent()) {
        connect(DuiKeyboardStateTracker::instance(), SIGNAL(stateChanged()), this, SLOT(updateInputSource()));
    }

    d->imAccessoryEnabledConf = new DuiGConfItem(DuiImAccesoryEnabled, this);
    connect(d->imAccessoryEnabledConf, SIGNAL(valueChanged()), this, SLOT(updateInputSource()));

    updateInputSource();

    connect(&d->deleteImTimer, SIGNAL(timeout()), this, SLOT(deleteInactiveIM()));
}


DuiIMPluginManager::~DuiIMPluginManager()
{
    delete d;
    d = 0;
}


void DuiIMPluginManager::reloadHandlerMap()
{
    QList<QString> handlers = d->handlerToPluginConf->listEntries();
    const QString key = d->handlerToPluginConf->key() + "/";

    foreach (const QString &handlerName, handlers) {
        QStringList path = handlerName.split("/");
        QString pluginName = DuiGConfItem(handlerName).value().toString();
        d->addHandlerMap((DuiIMHandlerState)path.last().toInt(), pluginName);
    }
}


void DuiIMPluginManager::deleteInactiveIM()
{
    d->deleteInactiveIM();
}


QStringList DuiIMPluginManager::loadedPluginsNames() const
{
    return d->loadedPluginsNames();
}


QStringList DuiIMPluginManager::activePluginsNames() const
{
    return d->activePluginsNames();
}


QStringList DuiIMPluginManager::activeInputMethodsNames() const
{
    return d->activeInputMethodsNames();
}


void DuiIMPluginManager::setDeleteIMTimeout(int timeout)
{
    d->deleteImTimer.setInterval(timeout);
}

void DuiIMPluginManager::updateInputSource()
{
    QSet<DuiIMHandlerState> handlers = d->activeHandlers();
    if (DuiKeyboardStateTracker::instance()->isOpen()) {
        // hw keyboard is on
        handlers.remove(OnScreen);
        handlers.insert(Hardware);
    } else {
        // hw keyboard is off
        handlers.remove(Hardware);
        handlers.insert(OnScreen);
    }

    if (d->imAccessoryEnabledConf->value().toBool()) {
        handlers.remove(OnScreen);
        handlers.insert(Accessory);
    }

    if (!handlers.isEmpty()) {
        d->setActiveHandlers(handlers);
        d->deleteImTimer.start();
    }
}
