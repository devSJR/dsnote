/* Copyright (C) 2021-2023 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef MODELSLISTMODEL_H
#define MODELSLISTMODEL_H

#include <QByteArray>
#include <QDebug>
#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <optional>

#include "itemmodel.h"
#include "listmodel.h"
#include "models_manager.h"

class ModelsListModel : public SelectableItemModel {
    Q_OBJECT
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadingChanged)
    Q_PROPERTY(QString lang READ lang WRITE setLang NOTIFY langChanged)
    Q_PROPERTY(ModelRoleFilter roleFilter READ roleFilter WRITE setRoleFilter
                   NOTIFY roleFilterChanged)
   public:
    enum ModelRole { Stt = 0, Tts = 1, Mnt = 2, Ttt = 3 };
    Q_ENUM(ModelRole)
    enum ModelRoleFilter {
        AllModels = 0,
        SttModels = 1,
        TtsModels = 2,
        MntModels = 3,
        OtherModels = 4
    };
    Q_ENUM(ModelRoleFilter)

    explicit ModelsListModel(QObject *parent = nullptr);
    ~ModelsListModel() override;

   signals:
    void itemChanged(int idx);
    void downloadingChanged();
    void langChanged();
    void roleFilterChanged();

   private:
    int m_changedItem = -1;
    bool m_downloading = false;
    QString m_lang;
    ModelRoleFilter m_roleFilter = ModelRoleFilter::AllModels;

    QList<ListItem *> makeItems() override;
    static ListItem *makeItem(const models_manager::model_t &model);
    size_t firstChangedItemIdx(const QList<ListItem *> &oldItems,
                               const QList<ListItem *> &newItems) override;
    void updateItem(ListItem *oldItem, const ListItem *newItem) override;
    inline bool downloading() const { return m_downloading; }
    inline QString lang() const { return m_lang; }
    inline ModelRoleFilter roleFilter() const { return m_roleFilter; }
    void setLang(const QString &lang);
    void setRoleFilter(ModelRoleFilter roleFilter);
    void updateDownloading(const std::vector<models_manager::model_t> &models);
    bool roleFilterPass(const models_manager::model_t &model);
};

class ModelsListItem : public SelectableItem {
    Q_OBJECT
   public:
    enum Roles {
        NameRole = Qt::DisplayRole,
        IdRole = Qt::UserRole,
        LangIdRole,
        AvailableRole,
        DlMultiRole,
        DlOffRole,
        ScoreRole,
        DefaultRole,
        DownloadingRole,
        ProgressRole,
        ModelRole,
        LicenseIdRole,
        LicenseNameRole,
        LicenseUrlRole,
        LicenseAccceptRequiredRole,
        DownloadUrlsRole,
        DownloadSizeRole
    };

    struct License {
        QString id;
        QString name;
        QUrl url;
        bool accept_required = false;
    };

    struct DownloadInfo {
        QStringList urls;
        QString size = 0;
    };

    ModelsListItem(QObject *parent = nullptr) : SelectableItem{parent} {}
    ModelsListItem(const QString &id, QString name, QString lang_id,
                   ModelsListModel::ModelRole role, License license,
                   DownloadInfo download_info, bool available = true,
                   bool dl_multi = false, bool dl_off = false, int score = 2,
                   bool default_for_lang = false, bool downloading = false,
                   double progress = 0.0, QObject *parent = nullptr);
    QVariant data(int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    inline QString id() const override { return m_id; }
    inline QString name() const { return m_name; }
    inline QString lang_id() const { return m_lang_id; }
    inline ModelsListModel::ModelRole modelRole() const { return m_role; }
    inline bool available() const { return m_available; }
    inline bool dl_multi() const { return m_dl_multi; }
    inline bool dl_off() const { return m_dl_off; }
    inline int score() const { return m_score; }
    inline bool default_for_lang() const { return m_default_for_lang; }
    inline bool downloading() const { return m_downloading; }
    inline double progress() const { return m_progress; }
    inline QString license_id() const { return m_license.id; }
    inline QString license_name() const { return m_license.name; }
    inline QUrl license_url() const { return m_license.url; }
    inline bool license_accept_required() const {
        return m_license.accept_required;
    }
    inline QStringList download_urls() const { return m_download_info.urls; }
    inline QString download_size() const { return m_download_info.size; }
    void update(const ModelsListItem *item);

   private:
    QString m_id;
    QString m_name;
    QString m_lang_id;
    ModelsListModel::ModelRole m_role = ModelsListModel::ModelRole::Stt;
    License m_license;
    DownloadInfo m_download_info;
    bool m_available = false;
    bool m_dl_multi = false;
    bool m_dl_off = false;
    int m_score = 2;
    bool m_default_for_lang = false;
    bool m_downloading = false;
    double m_progress = 0.0;
};

#endif  // MODELSLISTMODEL_H
