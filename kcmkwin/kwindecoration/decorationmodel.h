/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2009 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/

#ifndef KWIN_DECORATIONMODEL_H
#define KWIN_DECORATIONMODEL_H
#include <QAbstractListModel>
#include <QPixmap>
#include <KConfig>
#include <KSharedConfig>
#include <kdecoration.h>

class QWidget;
class KDesktopFile;
class KDecorationPlugins;
class KDecorationPreview;
class KDecorationPreviewOptions;

namespace KWin
{

class DecorationButtons;

class DecorationModelData
{
public:
    enum DecorationType {
        NativeDecoration = 0,
        AuroraeDecoration = 1,
        QmlDecoration = 2
    };
    QString name;
    QString pluginName;
    DecorationType type;
    QString comment;
    QString author;
    QString email;
    QString website;
    QString version;
    QString license;
    QString auroraeName;
    QString qmlPath;
    QString configTranslationDomain;
    KDecorationDefines::BorderSize borderSize;
    KDecorationDefines::BorderSize buttonSize;
    /**
     * Whether the window gets closed on double clicking the Menu Button.
     * Only applies for Aurorae and QML Decoration.
     **/
    bool closeDblClick;

    static bool less(const DecorationModelData& a, const DecorationModelData& b) {
        return a.name < b.name;
    }
};

class DecorationModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum {
        NameRole = Qt::UserRole,
        LibraryNameRole = Qt::UserRole + 1,
        TypeRole = Qt::UserRole + 3,
        AuroraeNameRole = Qt::UserRole + 4,
        PackageDescriptionRole = Qt::UserRole + 5,
        PackageAuthorRole = Qt::UserRole + 6,
        PackageEmailRole = Qt::UserRole + 7,
        PackageWebsiteRole = Qt::UserRole + 8,
        PackageVersionRole = Qt::UserRole + 9,
        PackageLicenseRole = Qt::UserRole + 10,
        BorderSizeRole = Qt::UserRole + 11,
        BorderSizesRole = Qt::UserRole + 12,
        ButtonSizeRole = Qt::UserRole + 13,
        QmlMainScriptRole = Qt::UserRole + 14,
        CloseOnDblClickRole = Qt::UserRole + 15,
        ConfigTranslationDomain = Qt::UserRole + 16
    };
    explicit DecorationModel(KSharedConfigPtr config, QObject* parent = nullptr);
    ~DecorationModel();

    virtual QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const;
    virtual int rowCount(const QModelIndex& parent = QModelIndex()) const;
    virtual bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole);

    void reload();

    /**
    * Changes the button state and regenerates the preview.
    */
    void changeButtons(const DecorationButtons *buttons);
    /**
    * Changes the button state without regenerating the preview.
    */
    void setButtons(bool custom, const QList<KDecorationDefines::DecorationButton>& left, const QList<KDecorationDefines::DecorationButton>& right);

    void setBorderSize(const QModelIndex& index, KDecorationDefines::BorderSize size);

    QModelIndex indexOfLibrary(const QString& libraryName) const;
    QModelIndex indexOfName(const QString& decoName) const;
    QModelIndex indexOfAuroraeName(const QString& auroraeName, const QString& type) const;

    Q_INVOKABLE QVariant readConfig(const QString &themeName, const QString &key, const QVariant &defaultValue = QVariant());

    void notifyConfigChanged(const QModelIndex &index);

Q_SIGNALS:
    void configChanged(QString themeName);
private:
    void findDecorations();
    void findAuroraeThemes();
    void metaData(DecorationModelData& data, const KDesktopFile& df);
    QList<DecorationModelData> m_decorations;
    KDecorationPlugins* m_plugins;
    KDecorationPreview* m_preview;
    bool m_customButtons;
    KSharedConfigPtr m_config;
    QScopedPointer<KDecorationPreviewOptions> m_options;
};

} // namespace KWin

#endif // KWIN_DECORATIONMODEL_H
